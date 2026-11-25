/*-------------------------------------------------------------------------
 *
 * lsmctrl.c
 *	  Control-page, shared-memory and child-btree management for the LSM AM.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/lsm3.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xloginsert.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/indexing.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "storage/bulk_write.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

Lsm3Shared *lsm3_shared = NULL;

/* ---------- control page ---------- */

Lsm3Control *
lsm3_read_control(Relation index)
{
	Buffer		buf;
	Page		page;
	Lsm3Control *ctl;

	ctl = (Lsm3Control *) palloc(sizeof(Lsm3Control));
	buf = ReadBuffer(index, LSM3_CONTROL_BLOCK);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	memcpy(ctl, PageGetContents(page), sizeof(Lsm3Control));
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	if (ctl->magic != LSM3_MAGIC)
		elog(ERROR, "lsm index \"%s\" has an invalid control page",
			 RelationGetRelationName(index));
	return ctl;
}

void
lsm3_write_control(Relation index, Lsm3Control *ctl)
{
	Buffer		buf;
	Page		page;

	buf = ReadBuffer(index, LSM3_CONTROL_BLOCK);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	memcpy(PageGetContents(page), ctl, sizeof(Lsm3Control));
	MarkBufferDirty(buf);
	/* page_std=false: payload lives in the pd_lower..pd_upper hole */
	log_newpage_buffer(buf, false);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	CacheInvalidateRelcache(index);
}

void
lsm3_init_control(Relation index)
{
	Buffer		buf;
	Page		page;
	Lsm3Control *ctl;

	buf = ReadBufferExtended(index, MAIN_FORKNUM, LSM3_CONTROL_BLOCK,
							 RBM_ZERO_AND_LOCK, NULL);
	page = BufferGetPage(buf);
	PageInit(page, BLCKSZ, 0);

	ctl = (Lsm3Control *) palloc0(sizeof(Lsm3Control));
	ctl->magic = LSM3_MAGIC;
	ctl->generation = 1;

	memcpy(PageGetContents(page), ctl, sizeof(Lsm3Control));
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, false);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
	pfree(ctl);
}

/* ---------- shared memory registration ---------- */

void
lsm3_update_shared(Oid indexOid, Lsm3Control *ctl)
{
	int			i;

	if (lsm3_shared == NULL)
		return;
	LWLockAcquire(&lsm3_shared->lock[0], LW_EXCLUSIVE);
	for (i = 0; i < LSM3_MAX_INDEXES; i++)
	{
		Lsm3IndexState *s = &lsm3_shared->indexes[i];

		if (s->indexOid == indexOid)
		{
			Oid			oldmerging = s->merging;

			s->active = ctl->active;
			s->merging = ctl->merging;
			s->spare = ctl->spare;
			s->base = ctl->base;
			s->active_entries = ctl->active_entries;
			/* merging changed identity -> the new merging needs a drain */
			if (ctl->merging != InvalidOid &&
				(OidIsValid(oldmerging) ? oldmerging != ctl->merging : true))
				s->merge_pending = true;
			break;
		}
	}
	LWLockRelease(&lsm3_shared->lock[0]);
}

void
lsm3_register(Relation index, Lsm3Control *ctl)
{
	int			i;
	int			free = -1;

	if (lsm3_shared == NULL)
		return;
	LWLockAcquire(&lsm3_shared->lock[0], LW_EXCLUSIVE);
	for (i = 0; i < LSM3_MAX_INDEXES; i++)
	{
		Lsm3IndexState *s = &lsm3_shared->indexes[i];

		if (s->indexOid == RelationGetRelid(index))
		{
			/* already registered; just refresh */
			LWLockRelease(&lsm3_shared->lock[0]);
			lsm3_update_shared(RelationGetRelid(index), ctl);
			return;
		}
		if (s->indexOid == InvalidOid && free < 0)
			free = i;
	}
	if (free >= 0)
	{
		Lsm3IndexState *s = &lsm3_shared->indexes[free];

		s->indexOid = RelationGetRelid(index);
		s->databaseOid = MyDatabaseId;
		s->active = ctl->active;
		s->merging = ctl->merging;
		s->spare = ctl->spare;
		s->base = ctl->base;
		s->active_entries = ctl->active_entries;
		pg_atomic_init_u32(&s->active_counter, ctl->active_entries);
		s->merge_pending = false;
		s->merge_running = false;
		lsm3_shared->nindexes++;
	}
	LWLockRelease(&lsm3_shared->lock[0]);
}

/*
 * lsm3_ensure_registered: register this index in shared memory on first use
 * in a backend (needed after a postmaster restart, which clears shmem).
 */
void
lsm3_ensure_registered(Relation index, Lsm3Control *ctl)
{
	int			i;
	bool		present = false;

	if (lsm3_shared == NULL)
		return;
	LWLockAcquire(&lsm3_shared->lock[0], LW_SHARED);
	for (i = 0; i < LSM3_MAX_INDEXES; i++)
	{
		if (lsm3_shared->indexes[i].indexOid == RelationGetRelid(index))
		{
			present = true;
			break;
		}
	}
	LWLockRelease(&lsm3_shared->lock[0]);
	if (!present)
		lsm3_register(index, ctl);
}

void
lsm3_unregister(Oid indexOid)
{
	int			i;

	if (lsm3_shared == NULL)
		return;
	LWLockAcquire(&lsm3_shared->lock[0], LW_EXCLUSIVE);
	for (i = 0; i < LSM3_MAX_INDEXES; i++)
	{
		if (lsm3_shared->indexes[i].indexOid == indexOid)
		{
			memset(&lsm3_shared->indexes[i], 0, sizeof(Lsm3IndexState));
			lsm3_shared->nindexes--;
			break;
		}
	}
	LWLockRelease(&lsm3_shared->lock[0]);
}

Lsm3IndexState *
lsm3_lookup_shared(Oid indexOid)
{
	Lsm3IndexState *result = NULL;
	int			i;

	if (lsm3_shared == NULL)
		return NULL;
	LWLockAcquire(&lsm3_shared->lock[0], LW_SHARED);
	for (i = 0; i < LSM3_MAX_INDEXES; i++)
	{
		if (lsm3_shared->indexes[i].indexOid == indexOid)
		{
			result = &lsm3_shared->indexes[i];
			break;
		}
	}
	/* NOTE: caller must hold the lock while using the returned pointer */
	if (result == NULL)
		LWLockRelease(&lsm3_shared->lock[0]);
	return result;
}

void
lsm3_release_shared(void)
{
	if (lsm3_shared)
		LWLockRelease(&lsm3_shared->lock[0]);
}

/*
 * lsm3_get_trees: fetch the three tree OIDs from shared memory (cheap).
 * On first use after a restart the index may not be registered yet, in which
 * case fall back to reading the persistent control page once.
 */
/*
 * lsm3_refresh_generation: lsm3_create_tree() bumps the on-disk generation
 * counter.  Callers that later write a stale control struct must refresh the
 * generation first, or a future tree name will collide with an older one.
 */
void
lsm3_refresh_generation(Relation index, Lsm3Control *ctl)
{
	Lsm3Control *fresh = lsm3_read_control(index);

	ctl->generation = fresh->generation;
	pfree(fresh);
}

void
lsm3_get_trees(Relation index, Oid *active, Oid *merging, Oid *base)
{
	Oid			indexOid = RelationGetRelid(index);
	int			i;

	*active = *merging = *base = InvalidOid;
	if (lsm3_shared != NULL)
	{
		LWLockAcquire(&lsm3_shared->lock[0], LW_SHARED);
		for (i = 0; i < LSM3_MAX_INDEXES; i++)
		{
			Lsm3IndexState *s = &lsm3_shared->indexes[i];

			if (s->indexOid == indexOid)
			{
				*active = s->active;
				*merging = s->merging;
				*base = s->base;
				break;
			}
		}
		LWLockRelease(&lsm3_shared->lock[0]);
	}
	if (!OidIsValid(*active))
	{
		Lsm3Control *ctl = lsm3_read_control(index);

		*active = ctl->active;
		*merging = ctl->merging;
		*base = ctl->base;
		lsm3_register(index, ctl);	/* repopulate shared memory */
		pfree(ctl);
	}
}

/*
 * lsm3_insert_begin: single-lock hot path.  Returns the current T_active OID
 * and bumps the in-memory counter in one LWLock acquisition.  Never touches
 * the control page (its full-page WAL write was the source of insert latency).
 */
/*
 * lsm3_try_swap: called when the backend-local counter reaches the cap.
 * Returns true if a role swap should proceed (no merge pending/running).
 */
bool
lsm3_try_swap(Oid indexOid)
{
	bool		ok = false;
	int			i;

	if (lsm3_shared == NULL)
		return false;
	LWLockAcquire(&lsm3_shared->lock[0], LW_EXCLUSIVE);
	for (i = 0; i < LSM3_MAX_INDEXES; i++)
	{
		Lsm3IndexState *s = &lsm3_shared->indexes[i];

		if (s->indexOid == indexOid)
		{
			if (!s->merge_pending && !s->merge_running)
			{
				pg_atomic_write_u32(&s->active_counter, 0);
				ok = true;
			}
			break;
		}
	}
	LWLockRelease(&lsm3_shared->lock[0]);
	return ok;
}

bool
lsm3_insert_begin(Oid indexOid, Oid *activeOid, int *cached_slot)
{
	Lsm3IndexState *s = NULL;
	uint32		n;
	int			i;

	*activeOid = InvalidOid;
	if (lsm3_shared == NULL)
		return false;

	/* Fast path: reuse the slot index cached from a previous insert. */
	if (*cached_slot >= 0 && *cached_slot < LSM3_MAX_INDEXES &&
		lsm3_shared->indexes[*cached_slot].indexOid == indexOid)
		s = &lsm3_shared->indexes[*cached_slot];
	else
	{
		for (i = 0; i < LSM3_MAX_INDEXES; i++)
		{
			if (lsm3_shared->indexes[i].indexOid == indexOid)
			{
				s = &lsm3_shared->indexes[i];
				*cached_slot = i;
				break;
			}
		}
	}
	if (s == NULL)
		return false;

	*activeOid = s->active;			/* changes only at swap (rare) */
	n = pg_atomic_fetch_add_u32(&s->active_counter, 1) + 1;

	/* Only take the LWLock when the threshold is actually crossed. */
	if (n >= LSM3_TOP_CAP_TUPLES)
	{
		bool		do_swap;

		LWLockAcquire(&lsm3_shared->lock[0], LW_EXCLUSIVE);
		do_swap = !s->merge_pending && !s->merge_running;
		if (do_swap)
			pg_atomic_write_u32(&s->active_counter, 0);
		LWLockRelease(&lsm3_shared->lock[0]);
		return do_swap;
	}
	return false;
}

/* ---------- child btrees ---------- */

static void
lsm3_mark_child_flags(Oid childOid, Oid parentOid)
{
	Relation	pg_index;
	HeapTuple	tup;
	Form_pg_index form;

	pg_index = table_open(IndexRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(INDEXRELID, ObjectIdGetDatum(childOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for index %u", childOid);
	form = (Form_pg_index) GETSTRUCT(tup);
	form->indisvalid = false;
	form->indisready = false;	/* executor must not touch child trees */
	CatalogTupleUpdate(pg_index, &tup->t_self, tup);
	heap_freetuple(tup);
	table_close(pg_index, RowExclusiveLock);
	CacheInvalidateRelcacheByRelid(parentOid);
}

Oid
lsm3_create_tree(Relation index, Relation heap, IndexInfo *indexInfo,
				 const char *role)
{
	char	   *name;
	Oid			parentOid = RelationGetRelid(index);
	Oid			childOid;
	List	   *colnames = NIL;
	Oid			opclasses[INDEX_MAX_KEYS];
	Oid			collations[INDEX_MAX_KEYS];
	int16		coloptions[INDEX_MAX_KEYS];
	HeapTuple	tuple;
	Datum		d;
	oidvector  *old_indclass;
	int			n = indexInfo->ii_NumIndexAttrs;
	int			i;

	/* use a generation counter from the control page (also sets the name) */
	{
		Lsm3Control *ctl = lsm3_read_control(index);

		name = psprintf("lsm3_%u_%s_%u", parentOid, role, ctl->generation++);
		lsm3_write_control(index, ctl);
		pfree(ctl);
	}

	for (i = 0; i < n; i++)
	{
		char	   *attname = get_attname(parentOid, i + 1, false);

		colnames = lappend(colnames, makeString(attname));
	}

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(parentOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", parentOid);
	d = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indclass);
	old_indclass = (oidvector *) DatumGetPointer(d);
	for (i = 0; i < n; i++)
	{
		opclasses[i] = old_indclass->values[i];
		collations[i] = index->rd_indcollation[i];
		coloptions[i] = index->rd_indoption[i];
	}
	ReleaseSysCache(tuple);

	childOid = index_create(heap, name, InvalidOid, InvalidOid, InvalidOid,
							InvalidRelFileNumber, indexInfo, colnames,
							BTREE_AM_OID, index->rd_rel->reltablespace,
							collations, opclasses,
							NULL,				/* opclassOptions */
							coloptions,
							NULL,				/* stattargets */
							(Datum) 0,			/* reloptions */
							INDEX_CREATE_SKIP_BUILD | INDEX_CREATE_INVALID,
							0, false, false, NULL);

	lsm3_mark_child_flags(childOid, parentOid);

	/* internal dependency so DROP INDEX cascades to the child */
	{
		ObjectAddress parent,
					child;

		parent.classId = RelationRelationId;
		parent.objectId = parentOid;
		parent.objectSubId = 0;
		child.classId = RelationRelationId;
		child.objectId = childOid;
		child.objectSubId = 0;
		recordDependencyOn(&child, &parent, DEPENDENCY_INTERNAL);
	}

	return childOid;
}

void
lsm3_init_tree_metapage(Relation rel)
{
	BulkWriteState *bulkstate;
	BulkWriteBuffer metabuf;

	/*
	 * Write block 0 via the bulk-write path so the file is genuinely extended
	 * to one block (RBM_ZERO_AND_LOCK leaves a valid buffer beyond EOF and a
	 * later normal read errors).  Since trees are dropped rather than
	 * truncated in place, there is no mixing of bulk and buffer writes.
	 */
	bulkstate = smgr_bulk_start_rel(rel, MAIN_FORKNUM);
	metabuf = smgr_bulk_get_buf(bulkstate);
	_bt_initmetapage((Page) metabuf, P_NONE, 0,
					 _bt_allequalimage(rel, false));
	smgr_bulk_write(bulkstate, BTREE_METAPAGE, metabuf, true);
	smgr_bulk_finish(bulkstate);
}

Relation
lsm3_open_tree(Oid relid, LOCKMODE lockmode)
{
	Relation	rel;

	rel = relation_open(relid, lockmode);
	if (rel->rd_rel->relkind != RELKIND_INDEX ||
		rel->rd_rel->relam != BTREE_AM_OID)
		elog(ERROR, "relation %u is not an lsm child btree", relid);
	return rel;
}

/*
 * Empty a top btree in preparation for reuse as the new T_active.  We drop
 * and recreate the relation so its file is truly empty; this is done by the
 * worker inside a transaction, so catalog access is safe.
 */
void
lsm3_drop_tree(Oid relid)
{
	/* remove the internal dependency so DROP INDEX won't chase a dead OID */
	deleteDependencyRecordsFor(RelationRelationId, relid, false);
	index_drop(relid, false, false);
}
