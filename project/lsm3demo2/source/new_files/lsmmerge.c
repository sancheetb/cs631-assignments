/*-------------------------------------------------------------------------
 *
 * lsmmerge.c
 *	  Role swap and the background merge worker's drain routine.
 *
 * T_active fills up -> swap roles with T_merging -> worker drains the frozen
 * T_merging into T_base in sorted order, then truncates it so it becomes the
 * next empty T_active.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/lsm3.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* ---------- role swap (called from aminsert when T_active is full) ---------- */

void
lsm3_swap_top(Relation index, Lsm3Control *ctl)
{
	Oid			tmp;

	/*
	 * Pure OID exchange — no DDL in the insert path (that was the source of
	 * the remaining insert slowdown).  The full T_active is frozen as
	 * T_merging; the pre-created empty T_spare becomes the new T_active.
	 * The worker later drains T_merging and creates a fresh spare off-path.
	 */
	if (lsm3_shared)
	{
		Lsm3IndexState *s = lsm3_lookup_shared(RelationGetRelid(index));

		if (s != NULL)
		{
			bool		busy = s->merge_pending || s->merge_running;

			lsm3_release_shared();
			if (busy)
				return;
		}
	}

	if (OidIsValid(ctl->merging) || !OidIsValid(ctl->spare))
		return;						/* no ready spare (worker behind) */

	tmp = ctl->active;
	ctl->active = ctl->spare;		/* empty spare becomes new active */
	ctl->spare = InvalidOid;		/* worker replenishes after drain */
	ctl->merging = tmp;				/* frozen batch for the worker */
	ctl->active_entries = 0;
	ctl->nswaps++;

	lsm3_refresh_generation(index, ctl);
	lsm3_write_control(index, ctl);
	lsm3_update_shared(RelationGetRelid(index), ctl);

	if (lsm3_shared)
		SetLatch(&lsm3_shared->worker_latch);
}

/* ---------- drain (called by the bgworker) ---------- */

/*
 * Scan the merging tree in key order and insert each live tuple into the base
 * tree.  Tombstones are dropped (their heap TID is dead).  After draining,
 * truncate the merging tree so it becomes the next empty T_active.
 */
static void
drain_one_index(Lsm3IndexState *state)
{
	Relation	parent,
				heap,
				merging,
				base;
	IndexScanDesc scan;
	IndexInfo  *baseInfo;
	IndexInfo  *parentInfo;
	uint64		n = 0;
	bool		hadTombstones = false;

	parent = relation_open(state->indexOid, AccessShareLock);
	baseInfo = BuildIndexInfo(parent);

	if (!OidIsValid(state->merging) || !OidIsValid(state->base))
	{
		relation_close(parent, AccessShareLock);
		return;
	}

	heap = table_open(parent->rd_index->indrelid, RowExclusiveLock);
	merging = lsm3_open_tree(state->merging, RowExclusiveLock);
	base = lsm3_open_tree(state->base, RowExclusiveLock);
	baseInfo = BuildIndexInfo(base);

	scan = btbeginscan(merging, 0, 0);
	/* RelationGetIndexScan() does not zero parallel_scan */
	scan->parallel_scan = NULL;
	scan->xs_snapshot = SnapshotAny;
	scan->xs_want_itup = true;
	btrescan(scan, NULL, 0, NULL, 0);

	while (btgettuple(scan, ForwardScanDirection))
	{
		IndexTuple	itup = scan->xs_itup;
		ItemPointerData tid = scan->xs_heaptid;
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];

		if (lsm3_is_tombstone(&tid))
		{
			hadTombstones = true;
			continue;				/* discard deleted entries permanently */
		}
		index_deform_tuple(itup, RelationGetDescr(merging), values, isnull);
		Assert(ItemPointerIsValid(&tid));
		btinsert(base, values, isnull, &tid, heap,
				 UNIQUE_CHECK_NO, false, baseInfo);
		n++;
	}

	btendscan(scan);
	relation_close(merging, RowExclusiveLock);

	relation_close(base, RowExclusiveLock);

	/*
	 * Replenish the spare tree OFF THE INSERT PATH (we are in the worker's
	 * own transaction).  Creating a new empty btree here is cheap relative
	 * to inserting rows and avoids doing catalog DDL in aminsert().  We do
	 * not drop the drained tree — that would race with a backend opening it
	 * from a stale control-page snapshot; it is cleaned by DROP INDEX.
	 */
	{
		Lsm3Control *ctl = lsm3_read_control(parent);
		IndexInfo  *iinfo = BuildIndexInfo(parent);
		Oid			newspare;

		ctl->nextracted += n;
		ctl->generation++;
		ctl->merging = InvalidOid;
		newspare = lsm3_create_tree(parent, heap, iinfo, "spare");
		{
			Relation	t = lsm3_open_tree(newspare, RowExclusiveLock);

			lsm3_init_tree_metapage(t);
			relation_close(t, RowExclusiveLock);
		}
		ctl->spare = newspare;
		lsm3_refresh_generation(parent, ctl);
		lsm3_write_control(parent, ctl);

		/* refresh shared state's merging OID and replenished spare */
		{
			Lsm3IndexState *s = lsm3_lookup_shared(state->indexOid);

			if (s != NULL)
			{
				s->merging = InvalidOid;
				s->spare = newspare;
				lsm3_release_shared();
			}
		}
		pfree(ctl);
	}

	relation_close(heap, RowExclusiveLock);
	relation_close(parent, AccessShareLock);

	(void) parentInfo;
	(void) hadTombstones;
}

void
lsm3_drain_merging(Oid indexOid)
{
	Lsm3IndexState *state;
	bool		claimed = false;

	/* find the entry and claim it under the shmem lock */
	state = lsm3_lookup_shared(indexOid);
	if (state != NULL)
	{
		if (state->merge_pending && !state->merge_running)
		{
			state->merge_running = true;
			claimed = true;
		}
		lsm3_release_shared();
	}
	if (!claimed)
		return;

	/* do the actual drain inside a transaction */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		drain_one_index(state);
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_END_TRY();

	/* mark done */
	state = lsm3_lookup_shared(indexOid);
	if (state != NULL)
	{
		state->merge_running = false;
		state->merge_pending = false;
		lsm3_release_shared();
	}
}
