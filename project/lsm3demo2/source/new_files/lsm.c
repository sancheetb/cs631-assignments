/*-------------------------------------------------------------------------
 *
 * lsm.c
 *	  LSM-tree index AM: handler + interface routines over three btrees.
 *
 * Externally this is a B-tree.  Internally INSERTs go to a small top tree;
 * a background worker drains the other top tree into the base tree.  Reads
 * merge all three btrees and de-duplicate heap TIDs.
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
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

PG_FUNCTION_INFO_V1(lsm3handler);

/*
 * Backend-local cache of the active top tree.  A plain B-tree insert is
 * handed an already-open Relation and IndexInfo by the executor; our child
 * tree is internal, so we cache these here to avoid relation_open() and
 * BuildIndexInfo() on every row.  The entry is rebuilt when the active tree
 * changes at a swap (detected by the OID from shared memory).
 */
typedef struct Lsm3ActiveCache
{
	Oid			activeOid;		/* T_active OID the cache is valid for */
	IndexInfo  *iinfo;			/* cached IndexInfo for the active tree */
	uint32		local_count;	/* backend-local insert counter for the active tree */
} Lsm3ActiveCache;

static MemoryContext lsm3_cache_cxt = NULL;
static int			lsm3_cached_slot = -1;	/* cached shmem slot for insert hot path */
static Oid			lsm3_cache_parent = InvalidOid;
static Lsm3ActiveCache *lsm3_cache = NULL;

/*
 * Cache only the IndexInfo (built once per active tree) in the parent's
 * rd_amcache, which PostgreSQL frees on relcache invalidation (we invalidate
 * the parent on every swap/drain).  The Relation itself is opened/closed
 * within each insert — balanced, so the resource owner stays clean.
 */
static IndexInfo *
lsm3_get_active(Relation parent, Oid activeOid, Relation *rel_out, int *cached_slot)
{
	Lsm3ActiveCache *c;

	if (lsm3_cache_parent != RelationGetRelid(parent) || lsm3_cache == NULL ||
		parent->rd_amcache == NULL)
	{
		if (lsm3_cache_cxt == NULL)
			lsm3_cache_cxt = AllocSetContextCreate(TopMemoryContext,
												  "lsm3 active cache",
												  ALLOCSET_DEFAULT_SIZES);
		c = (Lsm3ActiveCache *) MemoryContextAllocZero(lsm3_cache_cxt,
													  sizeof(Lsm3ActiveCache));
		parent->rd_amcache = c;
		lsm3_cache_parent = RelationGetRelid(parent);
		lsm3_cache = c;
	}
	c = (Lsm3ActiveCache *) parent->rd_amcache;
	lsm3_cache = c;

	if (c->activeOid != activeOid || c->iinfo == NULL)
	{
		Relation	t = relation_open(activeOid, NoLock);

		c->activeOid = activeOid;
		MemoryContextSwitchTo(lsm3_cache_cxt);
		c->iinfo = BuildIndexInfo(t);
		MemoryContextSwitchTo(CurrentMemoryContext);
		relation_close(t, NoLock);
		c->local_count = 0;
	}

	/* Open the relation for this insert; the caller closes it. */
	*rel_out = relation_open(activeOid, NoLock);
	(void) cached_slot;
	return c->iinfo;
}

/* ---------- tombstone helpers ---------- */

bool
lsm3_is_tombstone(ItemPointer tid)
{
	return (tid->ip_posid & LSM3_TOMBSTONE_BIT) != 0;
}

void
lsm3_clear_tombstone(ItemPointer tid)
{
	tid->ip_posid &= ~LSM3_TOMBSTONE_BIT;
}

void
lsm3_make_tombstone(ItemPointer src, ItemPointer dst)
{
	*dst = *src;
	dst->ip_posid |= LSM3_TOMBSTONE_BIT;
}

/* ---------- comparison for the k-way merge ---------- */

int
lsm3_compare_tuples(Relation index, TupleDesc desc,
					IndexTuple a, IndexTuple b, int nkeyattrs)
{
	Datum		va[INDEX_MAX_KEYS],
				vb[INDEX_MAX_KEYS];
	bool		na[INDEX_MAX_KEYS],
				nb[INDEX_MAX_KEYS];
	int			i;

	index_deform_tuple(a, desc, va, na);
	index_deform_tuple(b, desc, vb, nb);

	for (i = 0; i < nkeyattrs; i++)
	{
		Oid			opfamily = index->rd_opfamily[i];
		Oid			ltype = index->rd_opcintype[i];
		Oid			proc = get_opfamily_proc(opfamily, ltype, ltype,
											 BTORDER_PROC);
		int32		cmp;

		if (!OidIsValid(proc))
			elog(ERROR, "no btree order proc for opfamily %u", opfamily);
		if (na[i] && nb[i])
			continue;
		if (na[i])
			return 1;
		if (nb[i])
			return -1;
		cmp = DatumGetInt32(OidFunctionCall2Coll(proc,
												index->rd_indcollation[i],
												va[i], vb[i]));
		if (cmp != 0)
			return cmp;
	}
	/* TID tiebreaker, tombstone bit ignored */
	{
		ItemPointerData ta = a->t_tid,
					tb = b->t_tid;

		lsm3_clear_tombstone(&ta);
		lsm3_clear_tombstone(&tb);
		if (ItemPointerGetBlockNumber(&ta) < ItemPointerGetBlockNumber(&tb))
			return -1;
		if (ItemPointerGetBlockNumber(&ta) > ItemPointerGetBlockNumber(&tb))
			return 1;
		if (ta.ip_posid < tb.ip_posid)
			return -1;
		if (ta.ip_posid > tb.ip_posid)
			return 1;
	}
	return 0;
}

/* ---------- planner cost hint ---------- */

int
lsm3_gettreeheight(Relation rel)
{
	/* 2 top btrees + base, each usually shallow */
	return 6;
}

/* ---------- AM handler ---------- */

Datum
PGDLLEXPORT lsm3handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amoptsprocnum = BTOPTIONS_PROC;
	amroutine->amcanorder = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanhash = false;
	amroutine->amconsistentequality = true;
	amroutine->amconsistentordering = true;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = true;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = false;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions = 0;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = lsm3build;
	amroutine->ambuildempty = NULL;
	amroutine->aminsert = lsm3insert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = lsm3bulkdelete;
	amroutine->amvacuumcleanup = lsm3vacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = btcostestimate;
	amroutine->amgettreeheight = lsm3_gettreeheight;
	amroutine->amoptions = btoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = NULL;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = lsm3beginscan;
	amroutine->amrescan = lsm3rescan;
	amroutine->amgettuple = lsm3gettuple;
	amroutine->amgetbitmap = NULL;
	amroutine->amendscan = lsm3endscan;
	amroutine->ammarkpos = lsm3markpos;
	amroutine->amrestrpos = lsm3restrpos;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;
	amroutine->amtranslatestrategy = bttranslatestrategy;
	amroutine->amtranslatecmptype = bttranslatecmptype;

	PG_RETURN_POINTER(amroutine);
}

/* ---------- build ---------- */

IndexBuildResult *
lsm3build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	Lsm3Control *ctl;
	IndexBuildResult *result;
	Relation	base;
	IndexInfo  *baseInfo;

	lsm3_init_control(index);
	ctl = lsm3_read_control(index);

	/* T_base: bulk-build from the existing heap rows */
	ctl->base = lsm3_create_tree(index, heap, indexInfo, "base");
	base = lsm3_open_tree(ctl->base, RowExclusiveLock);
	baseInfo = BuildIndexInfo(base);
	result = btbuild(heap, base, baseInfo);
	relation_close(base, NoLock);

	/* T_active and a pre-created empty spare.  Swapping just exchanges OIDs
	 * (no DDL in the insert path); the worker replenishes the spare. */
	ctl->active = lsm3_create_tree(index, heap, indexInfo, "active");
	ctl->spare = lsm3_create_tree(index, heap, indexInfo, "spare");
	{
		Relation	t = lsm3_open_tree(ctl->active, RowExclusiveLock);

		lsm3_init_tree_metapage(t);
		relation_close(t, RowExclusiveLock);
		t = lsm3_open_tree(ctl->spare, RowExclusiveLock);
		lsm3_init_tree_metapage(t);
		relation_close(t, RowExclusiveLock);
	}
	ctl->merging = InvalidOid;
	ctl->active_entries = 0;
	lsm3_refresh_generation(index, ctl);
	lsm3_write_control(index, ctl);
	lsm3_register(index, ctl);
	pfree(ctl);

	return result;
}

/* ---------- insert ---------- */

bool
lsm3insert(Relation rel, Datum *values, bool *isnull, ItemPointer ht_ctid,
		   Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged,
		   IndexInfo *indexInfo)
{
	Oid			activeOid;
	Relation	active;
	IndexInfo  *activeInfo;
	bool		result;
	Lsm3ActiveCache *c;

	/*
	 * Hot path identical in cost to a plain B-tree insert: all state is in
	 * the parent's rd_amcache.  Shared memory / control page are touched only
	 * on the first insert or when the local cap is reached (a swap).
	 */
	/*
	 * Read the cache directly from the parent's rd_amcache.  A swap calls
	 * CacheInvalidateRelcache(), which frees rd_amcache; a static pointer to
	 * it would dangle across statements in the same backend (yielding a
	 * garbage active OID).  rd_amcache == NULL means "re-fetch from shmem".
	 */
	{
		Lsm3ActiveCache *cc = (Lsm3ActiveCache *) rel->rd_amcache;

		if (cc != NULL && OidIsValid(cc->activeOid))
			activeOid = cc->activeOid;
		else
			activeOid = InvalidOid;
	}
	if (!OidIsValid(activeOid))
	{
		/* first insert (or new xact): fetch T_active from shared memory */
		lsm3_insert_begin(RelationGetRelid(rel), &activeOid, &lsm3_cached_slot);
		if (!OidIsValid(activeOid))
		{
			Oid			mergingOid,
						baseOid;

			lsm3_get_trees(rel, &activeOid, &mergingOid, &baseOid);
		}
	}
	activeInfo = lsm3_get_active(rel, activeOid, &active, &lsm3_cached_slot);

	result = btinsert(active, values, isnull, ht_ctid, heapRel,
					  UNIQUE_CHECK_NO, indexUnchanged, activeInfo);
	relation_close(active, NoLock);

	/* local counter; consult shared memory/cap only at the threshold */
	c = (Lsm3ActiveCache *) rel->rd_amcache;
	if (c != NULL && ++c->local_count >= LSM3_TOP_CAP_TUPLES)
	{
		bool		needswap = lsm3_try_swap(RelationGetRelid(rel));

		c->local_count = 0;
		if (needswap)
		{
			Lsm3Control *ctl = lsm3_read_control(rel);

			lsm3_swap_top(rel, ctl);
			lsm3_update_shared(RelationGetRelid(rel), ctl);
			pfree(ctl);
			/*
			 * swap_top invalidated the parent relcache, which freed our
			 * rd_amcache entry (c).  Do NOT touch c here — use-after-free.
			 * The next insert sees rd_amcache == NULL and rebuilds it.
			 */
		}
	}

	return result;
}

/* ---------- scan ---------- */

IndexScanDesc
lsm3beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	Lsm3ScanState *state;
	Lsm3Control *ctl;
	int			pri = 0;

	scan = (IndexScanDesc) palloc0(sizeof(IndexScanDescData));
	scan->heapRelation = NULL;
	scan->indexRelation = rel;
	scan->xs_snapshot = InvalidSnapshot;
	scan->numberOfKeys = nkeys;
	scan->numberOfOrderBys = norderbys;
	scan->keyData = (ScanKey) palloc0(nkeys * sizeof(ScanKeyData));
	scan->orderByData = (ScanKey) palloc0(norderbys * sizeof(ScanKeyData));

	state = (Lsm3ScanState *) palloc0(sizeof(Lsm3ScanState));
	state->scancontext = CurrentMemoryContext;

	ctl = lsm3_read_control(rel);
	lsm3_ensure_registered(rel, ctl);
	/* priority: active(newest) = 0, merging = 1, base(oldest) = 2 */
	state->children[state->nchildren].relid = ctl->active;
	state->children[state->nchildren].priority = pri++;
	state->nchildren++;
	if (OidIsValid(ctl->merging))
	{
		state->children[state->nchildren].relid = ctl->merging;
		state->children[state->nchildren].priority = pri++;
		state->nchildren++;
	}
	state->children[state->nchildren].relid = ctl->base;
	state->children[state->nchildren].priority = pri;
	state->nchildren++;
	pfree(ctl);

	scan->opaque = state;
	return scan;
}

void
lsm3rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		   ScanKey orderbys, int norderbys)
{
	Lsm3ScanState *state = (Lsm3ScanState *) scan->opaque;
	int			i;

	for (i = 0; i < state->nchildren; i++)
	{
		Lsm3ChildScan *c = &state->children[i];

		c->rel = lsm3_open_tree(c->relid, AccessShareLock);
		c->scan = btbeginscan(c->rel, nscankeys, norderbys);
		c->scan->parallel_scan = NULL;
		c->scan->xs_snapshot = scan->xs_snapshot;
		c->scan->xs_want_itup = true;
		c->scan->kill_prior_tuple = false;
		c->scan->ignore_killed_tuples = scan->ignore_killed_tuples;
		btrescan(c->scan, scankey, nscankeys, orderbys, norderbys);
		c->exhausted = false;
		c->cur = NULL;
	}

	state->primed = false;
	state->is_backward = false;

	if (state->seen != NULL)
		hash_destroy(state->seen);
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ItemPointerData);
		ctl.entrysize = sizeof(ItemPointerData);
		ctl.hcxt = state->scancontext;
		state->seen = hash_create("lsm3 scan tid set", 1024, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
}

static void
prime_child(Lsm3ScanState *state, Lsm3ChildScan *c, ScanDirection dir)
{
	if (c->exhausted)
		return;
	if (btgettuple(c->scan, dir))
	{
		IndexTuple	itup = c->scan->xs_itup;

		if (c->cur)
			pfree(c->cur);
		c->cur = (IndexTuple) palloc(IndexTupleSize(itup));
		memcpy(c->cur, itup, IndexTupleSize(itup));
		c->curtid = c->scan->xs_heaptid;
		c->curTomb = lsm3_is_tombstone(&c->curtid);
		lsm3_clear_tombstone(&c->curtid);
	}
	else
	{
		c->exhausted = true;
		if (c->cur)
			pfree(c->cur);
		c->cur = NULL;
	}
	(void) state;
}

bool
lsm3gettuple(IndexScanDesc scan, ScanDirection dir)
{
	Lsm3ScanState *state = (Lsm3ScanState *) scan->opaque;
	Relation	index = scan->indexRelation;
	bool		backward = ScanDirectionIsBackward(dir);
	int			i,
				best;

	if (!state->primed)
	{
		for (i = 0; i < state->nchildren; i++)
			prime_child(state, &state->children[i], dir);
		state->primed = true;
		state->is_backward = backward;
	}
	else if (backward != state->is_backward)
	{
		for (i = 0; i < state->nchildren; i++)
		{
			btrescan(state->children[i].scan, NULL, 0, NULL, 0);
			state->children[i].exhausted = false;
			prime_child(state, &state->children[i], dir);
		}
		state->is_backward = backward;
	}

	while (true)
	{
		TupleDesc	desc = RelationGetDescr(state->children[0].rel);
		int			nkeyattrs = index->rd_index->indnatts;

		best = -1;
		for (i = 0; i < state->nchildren; i++)
		{
			Lsm3ChildScan *c = &state->children[i];
			int			cmp;

			if (c->exhausted || c->cur == NULL)
				continue;
			if (best < 0)
			{
				best = i;
				continue;
			}
			cmp = lsm3_compare_tuples(index, desc, c->cur,
									  state->children[best].cur, nkeyattrs);
			if ((!backward && cmp < 0) || (backward && cmp > 0))
				best = i;
		}
		if (best < 0)
			return false;

		{
			Lsm3ChildScan *c = &state->children[best];
			bool		found;

			(void) hash_search(state->seen, &c->curtid, HASH_FIND, &found);
			if (!found)
			{
				(void) hash_search(state->seen, &c->curtid, HASH_ENTER, NULL);
				if (!c->curTomb)
				{
					scan->xs_heaptid = c->curtid;
					if (scan->xs_want_itup)
					{
						if (scan->xs_itup)
							pfree(scan->xs_itup);
						scan->xs_itup = (IndexTuple) palloc(IndexTupleSize(c->cur));
						memcpy(scan->xs_itup, c->cur, IndexTupleSize(c->cur));
						scan->xs_itupdesc = desc;
					}
					prime_child(state, c, dir);
					return true;
				}
			}
			prime_child(state, c, dir);
		}
	}
}

void
lsm3endscan(IndexScanDesc scan)
{
	Lsm3ScanState *state = (Lsm3ScanState *) scan->opaque;
	int			i;

	for (i = 0; i < state->nchildren; i++)
	{
		if (state->children[i].scan)
		{
			btendscan(state->children[i].scan);
			relation_close(state->children[i].rel, AccessShareLock);
		}
		if (state->children[i].cur)
			pfree(state->children[i].cur);
	}
	if (state->seen)
		hash_destroy(state->seen);
	pfree(state);
	scan->opaque = NULL;
}

void
lsm3markpos(IndexScanDesc scan)
{
	Lsm3ScanState *state = (Lsm3ScanState *) scan->opaque;
	int			i;

	for (i = 0; i < state->nchildren; i++)
		btmarkpos(state->children[i].scan);
}

void
lsm3restrpos(IndexScanDesc scan)
{
	Lsm3ScanState *state = (Lsm3ScanState *) scan->opaque;
	int			i;

	for (i = 0; i < state->nchildren; i++)
	{
		btrestrpos(state->children[i].scan);
		prime_child(state, &state->children[i],
					state->is_backward ? BackwardScanDirection
					: ForwardScanDirection);
	}
}

/* ---------- vacuum / tombstones ---------- */

static void
insert_tombstone(Relation index, Relation heap, Datum *values, bool *isnull,
				 ItemPointer tid)
{
	Lsm3Control *ctl;
	Relation	active;
	IndexInfo  *activeInfo;
	ItemPointerData ttid;

	ctl = lsm3_read_control(index);
	active = lsm3_open_tree(ctl->active, RowExclusiveLock);
	activeInfo = BuildIndexInfo(active);
	lsm3_make_tombstone(tid, &ttid);
	btinsert(active, values, isnull, &ttid, heap,
			 UNIQUE_CHECK_NO, false, activeInfo);
	ctl->active_entries++;
	relation_close(active, RowExclusiveLock);
	lsm3_write_control(index, ctl);
	pfree(ctl);
}

IndexBulkDeleteResult *
lsm3bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	Relation	heap;
	Lsm3Control *ctl;
	int			i;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	ctl = lsm3_read_control(index);

	for (i = 0; i < 3; i++)
	{
		Oid			relid = (i == 0) ? ctl->active :
			(i == 1) ? ctl->merging : ctl->base;
		Relation	rel;
		IndexScanDesc scan;

		if (!OidIsValid(relid))
			continue;
		rel = lsm3_open_tree(relid, AccessShareLock);
		scan = btbeginscan(rel, 0, 0);
		scan->parallel_scan = NULL;
		scan->xs_snapshot = SnapshotAny;
		scan->xs_want_itup = true;
		btrescan(scan, NULL, 0, NULL, 0);
		while (btgettuple(scan, ForwardScanDirection))
		{
			ItemPointerData tid = scan->xs_heaptid;
			Datum		values[INDEX_MAX_KEYS];
			bool		isnull[INDEX_MAX_KEYS];

			if (lsm3_is_tombstone(&tid))
				continue;
			if (!callback(&tid, callback_state))
				continue;
			index_deform_tuple(scan->xs_itup, RelationGetDescr(rel),
							   values, isnull);
			insert_tombstone(index, heap, values, isnull, &tid);
			stats->tuples_removed++;
		}
		btendscan(scan);
		relation_close(rel, AccessShareLock);
	}
	pfree(ctl);
	table_close(heap, AccessShareLock);
	return stats;
}

IndexBulkDeleteResult *
lsm3vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->num_index_tuples = 0;
	return stats;
}
