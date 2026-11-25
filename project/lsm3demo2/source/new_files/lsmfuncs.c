/*-------------------------------------------------------------------------
 *
 * lsmfuncs.c
 *	  SQL-callable demonstration / maintenance functions for LSM indexes.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/lsm3.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(lsm3_flush);
PG_FUNCTION_INFO_V1(lsm3_compact);
PG_FUNCTION_INFO_V1(lsm3_status);

typedef struct StatusContext
{
	Lsm3Control *ctl;
	char	   *relname;
	int			idx;
} StatusContext;

static Datum
lsm3_maintenance(PG_FUNCTION_ARGS, bool do_flush, bool do_compact)
{
	Oid			indexOid = PG_GETARG_OID(0);
	Relation	index;
	Lsm3Control *ctl;

	index = relation_open(indexOid, RowExclusiveLock);
	if (index->rd_rel->relam != LSM_AM_OID)
		ereport(ERROR,
				(errmsg("relation \"%s\" is not an lsm index",
						RelationGetRelationName(index))));
	ctl = lsm3_read_control(index);

	if (do_flush)
	{
		/* force a role swap now */
		lsm3_swap_top(index, ctl);
	}
	if (do_compact)
	{
		/* signal the worker and wait one poll interval */
		if (lsm3_shared)
			SetLatch(&lsm3_shared->worker_latch);
	}

	lsm3_write_control(index, ctl);
	lsm3_update_shared(indexOid, ctl);
	pfree(ctl);
	relation_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

Datum
PGDLLEXPORT lsm3_flush(PG_FUNCTION_ARGS)
{
	return lsm3_maintenance(fcinfo, true, false);
}

Datum
PGDLLEXPORT lsm3_compact(PG_FUNCTION_ARGS)
{
	return lsm3_maintenance(fcinfo, false, true);
}

Datum
PGDLLEXPORT lsm3_status(PG_FUNCTION_ARGS)
{
	Oid			indexOid = PG_GETARG_OID(0);
	FuncCallContext *funcctx;
	StatusContext *ctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldctx;
		Relation	index;

		funcctx = SRF_FIRSTCALL_INIT();
		index = relation_open(indexOid, AccessShareLock);
		if (index->rd_rel->relam != LSM_AM_OID)
			ereport(ERROR,
					(errmsg("relation \"%s\" is not an lsm index",
							RelationGetRelationName(index))));
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		ctx = (StatusContext *) palloc0(sizeof(StatusContext));
		ctx->ctl = lsm3_read_control(index);
		ctx->relname = pstrdup(RelationGetRelationName(index));
		funcctx->user_fctx = ctx;
		MemoryContextSwitchTo(oldctx);
		ctx->idx = -1;
		relation_close(index, AccessShareLock);
	}

	funcctx = SRF_PERCALL_SETUP();
	ctx = (StatusContext *) funcctx->user_fctx;

	if (ctx->idx == -1)
	{
		ctx->idx = 0;
		SRF_RETURN_NEXT(funcctx,
						CStringGetTextDatum(psprintf(
							"LSM index \"%s\": gen=%u swaps=%u extracted=%llu active_entries=%u",
							ctx->relname, ctx->ctl->generation, ctx->ctl->nswaps,
							(unsigned long long) ctx->ctl->nextracted,
							ctx->ctl->active_entries)));
	}
	if (ctx->idx == 0)
	{
		ctx->idx++;
		SRF_RETURN_NEXT(funcctx,
						CStringGetTextDatum(psprintf(
							"  T_active   oid=%u  (insert target, memory resident)",
							ctx->ctl->active)));
	}
	if (ctx->idx == 1)
	{
		ctx->idx++;
		SRF_RETURN_NEXT(funcctx,
						CStringGetTextDatum(psprintf(
							"  T_merging  oid=%u  (worker draining into base)",
							ctx->ctl->merging)));
	}
	if (ctx->idx == 2)
	{
		ctx->idx++;
		SRF_RETURN_NEXT(funcctx,
						CStringGetTextDatum(psprintf(
							"  T_base     oid=%u  (large persistent index)",
							ctx->ctl->base)));
	}

	SRF_RETURN_DONE(funcctx);
}
