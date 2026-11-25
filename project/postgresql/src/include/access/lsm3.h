/*-------------------------------------------------------------------------
 *
 * lsm3.h
 *	  LSM-tree index AM: a memory-resident "top" B-tree fronting a large
 *	  on-disk "base" B-tree, with a background merge worker.
 *
 * Design (3-tree pattern, after postgrespro/lsm3):
 *   T_active   - small top index; all INSERTs land here (stays in cache)
 *   T_merging  - frozen batch while the bgworker drains it into T_base
 *   T_base     - large persistent main index
 *
 * When T_active crosses a size threshold the two top indexes swap roles in
 * shared memory.  A bgworker scans T_merging in key order, inserts into
 * T_base, then truncates T_merging so it becomes the next empty T_active.
 * Reads open all three btrees and k-way merge the result TIDs, de-duping
 * any TID present in both a top tree and the base (newest value wins).
 *-------------------------------------------------------------------------
 */
#ifndef LSM3_H
#define LSM3_H

#include "access/itup.h"
#include "access/relscan.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "postgres.h"
#include "storage/bufmgr.h"
#include "port/atomics.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "utils/rel.h"

/* module magic (this is a loadable library) */
#define LSM3_MODULE_NAME "lsm"

/* control block (block 0 of the parent index main fork) */
#define LSM3_CONTROL_BLOCK	0
#define LSM3_MAGIC			0x4C534D33	/* "LSM3" */
#define LSM3_MAX_SSTABLES	8

/* shared-memory layout sizes */
#define LSM3_MAX_INDEXES	32
#define LSM3_TOP_CAP_BYTES	(4 * 1024 * 1024)	/* ~4 MB top tree -> swap */
#define LSM3_TOP_CAP_TUPLES	4096

/* tombstone marker: high bit of the heap TID offset number */
#define LSM3_TOMBSTONE_BIT	0x8000

/* ---------- persistent control page ---------- */

typedef struct Lsm3TreeDesc
{
	Oid			relid;			/* child btree relation OID */
	uint64		nentries;		/* entries inserted into this tree */
} Lsm3TreeDesc;

typedef struct Lsm3Control
{
	uint32		magic;
	uint32		generation;
	Oid			active;			/* T_active */
	Oid			merging;		/* T_merging (may be InvalidOid) */
	Oid			spare;			/* pre-created empty tree, swapped in at threshold */
	Oid			base;			/* T_base */
	uint32		active_entries;
	uint32		nswaps;
	uint32		nextracted;		/* total entries drained into base */
} Lsm3Control;

/* ---------- shared memory: coordination of the 3 trees ---------- */

typedef struct Lsm3IndexState
{
	Oid			indexOid;		/* parent lsm index OID (0 = free slot) */
	Oid			databaseOid;	/* database that owns the index */
	Oid			active;
	Oid			merging;
	Oid			spare;
	Oid			base;
	uint32		active_entries;
	pg_atomic_uint32 active_counter;	/* lock-free insert counter (hot path) */
	bool		merge_pending;	/* merging has data needing drain */
	bool		merge_running;	/* worker currently draining */
} Lsm3IndexState;

typedef struct Lsm3Shared
{
	LWLock		lock[1];		/* protects all fields below (1 tranche) */
	Latch		worker_latch;	/* worker waits here; backends SetLatch */
	int			nindexes;
	Lsm3IndexState indexes[LSM3_MAX_INDEXES];
} Lsm3Shared;

extern Lsm3Shared *lsm3_shared;
extern int	lsm3_naptime_ms;	/* GUC: worker poll interval (ms) */

/* ---------- scan state ---------- */

typedef struct Lsm3ChildScan
{
	Oid			relid;
	int			priority;		/* 0 = active (newest), 1 = merging, 2 = base */
	Relation	rel;
	IndexScanDesc scan;
	bool		exhausted;
	IndexTuple	cur;			/* current index tuple (palloc'd) */
	ItemPointerData curtid;		/* normalized TID (tombstone bit cleared) */
	bool		curTomb;
} Lsm3ChildScan;

typedef struct Lsm3ScanState
{
	Lsm3ChildScan children[3];
	int			nchildren;
	bool		primed;
	bool		is_backward;
	HTAB	   *seen;
	MemoryContext scancontext;
} Lsm3ScanState;

/* ---------- control page / shared memory ---------- */
extern Lsm3Control *lsm3_read_control(Relation index);
extern void lsm3_write_control(Relation index, Lsm3Control *ctl);
extern void lsm3_init_control(Relation index);
extern void lsm3_register(Relation index, Lsm3Control *ctl);
extern void lsm3_ensure_registered(Relation index, Lsm3Control *ctl);
extern void lsm3_unregister(Oid indexOid);
extern void lsm3_update_shared(Oid indexOid, Lsm3Control *ctl);
extern Lsm3IndexState *lsm3_lookup_shared(Oid indexOid);
extern void lsm3_release_shared(void);
extern void lsm3_refresh_generation(Relation index, Lsm3Control *ctl);
extern void lsm3_get_trees(Relation index, Oid *active, Oid *merging, Oid *base);
extern bool lsm3_insert_begin(Oid indexOid, Oid *activeOid, int *cached_slot);
extern bool lsm3_try_swap(Oid indexOid);

/* ---------- child btrees ---------- */
extern Oid	lsm3_create_tree(Relation index, Relation heap,
							 IndexInfo *indexInfo, const char *role);
extern void lsm3_init_tree_metapage(Relation rel);
extern Relation lsm3_open_tree(Oid relid, LOCKMODE lockmode);
extern void lsm3_drop_tree(Oid relid);

/* ---------- merge (used by insert-time swap and by worker) ---------- */
extern void lsm3_swap_top(Relation index, Lsm3Control *ctl);
extern void lsm3_drain_merging(Oid indexOid);

/* ---------- tombstone helpers ---------- */
extern bool lsm3_is_tombstone(ItemPointer tid);
extern void lsm3_clear_tombstone(ItemPointer tid);
extern void lsm3_make_tombstone(ItemPointer src, ItemPointer dst);

/* ---------- comparison ---------- */
extern int	lsm3_compare_tuples(Relation index, TupleDesc desc,
								IndexTuple a, IndexTuple b, int nkeyattrs);
extern int	lsm3_gettreeheight(Relation rel);

/* ---------- AM interface ---------- */
extern PGDLLEXPORT Datum lsm3handler(PG_FUNCTION_ARGS);
extern IndexBuildResult *lsm3build(Relation heap, Relation index, IndexInfo *indexInfo);
extern bool lsm3insert(Relation rel, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique, bool indexUnchanged,
					   IndexInfo *indexInfo);
extern IndexScanDesc lsm3beginscan(Relation rel, int nkeys, int norderbys);
extern void lsm3rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);
extern bool lsm3gettuple(IndexScanDesc scan, ScanDirection dir);
extern void lsm3endscan(IndexScanDesc scan);
extern void lsm3markpos(IndexScanDesc scan);
extern void lsm3restrpos(IndexScanDesc scan);
extern IndexBulkDeleteResult *lsm3bulkdelete(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats,
											  IndexBulkDeleteCallback callback,
											  void *callback_state);
extern IndexBulkDeleteResult *lsm3vacuumcleanup(IndexVacuumInfo *info,
												 IndexBulkDeleteResult *stats);

/* ---------- module init / bgworker ---------- */
extern void _PG_init(void);
extern PGDLLEXPORT pg_noreturn void lsm3_worker_main(Datum main_arg);

/* ---------- SQL-callable demo functions ---------- */
extern PGDLLEXPORT Datum lsm3_flush(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum lsm3_compact(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum lsm3_status(PG_FUNCTION_ARGS);

#endif							/* LSM3_H */
