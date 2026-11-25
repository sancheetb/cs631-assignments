/*-------------------------------------------------------------------------
 *
 * lsmworker.c
 *	  Module init, shared-memory coordination and the background merge worker
 *	  for the LSM index AM.
 *
 * Loaded via shared_preload_libraries='lsm'.  _PG_init installs shmem hooks
 * and registers a bgworker that periodically drains T_merging into T_base.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/lsm3.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/wait_event.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

int	lsm3_naptime_ms = 1000;		/* worker poll interval */

static int	lsm3_worker_naptime = 1;	/* seconds (GUC) */
static int	lsm3_tranche_id = 0;
static uint32 lsm3_wait_event = 0;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void lsm3_shmem_request(void);
static void lsm3_shmem_startup(void);

/* ---------- shmem hooks ---------- */

static void
lsm3_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(sizeof(Lsm3Shared));
}

static void
lsm3_shmem_startup(void)
{
	bool		found;
	Size		size = sizeof(Lsm3Shared);

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	lsm3_shared = (Lsm3Shared *) ShmemInitStruct("lsm3 shared state",
												  size, &found);
	if (!found)
	{
		int			i;

		lsm3_tranche_id = LWLockNewTrancheId();
		LWLockRegisterTranche(lsm3_tranche_id, "lsm3");
		LWLockInitialize(&lsm3_shared->lock[0], lsm3_tranche_id);
		InitLatch(&lsm3_shared->worker_latch);
		lsm3_shared->nindexes = 0;
		for (i = 0; i < LSM3_MAX_INDEXES; i++)
		{
			lsm3_shared->indexes[i].indexOid = InvalidOid;
			pg_atomic_init_u32(&lsm3_shared->indexes[i].active_counter, 0);
		}
	}
}

/* ---------- module init ---------- */

void
_PG_init(void)
{
	BackgroundWorker worker;

	DefineCustomIntVariable("lsm3.worker_naptime",
							"Seconds between LSM merge-worker polls.",
							NULL,
							&lsm3_worker_naptime,
							1, 1, INT_MAX,
							PGC_SIGHUP, 0,
							NULL, NULL, NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	prev_shmem_request_hook = shmem_request_hook;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_request_hook = lsm3_shmem_request;
	shmem_startup_hook = lsm3_shmem_startup;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 1;
	strlcpy(worker.bgw_library_name, LSM3_MODULE_NAME, BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "lsm3_worker_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "lsm3 merge worker", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "lsm3merge", BGW_MAXLEN);
	worker.bgw_main_arg = Int32GetDatum(0);
	worker.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&worker);
}

/* ---------- worker main ---------- */

pg_noreturn void
PGDLLEXPORT lsm3_worker_main(Datum main_arg)
{
	Assert(lsm3_shared != NULL);

	BackgroundWorkerInitializeConnection("postgres", NULL, 0);
	lsm3_wait_event = WaitEventExtensionNew("LSM3MergeWorker");
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	elog(LOG, "lsm3 merge worker started");

	for (;;)
	{
		int			rc;
		int			i;
		Oid			pending[LSM3_MAX_INDEXES];
		int			npending = 0;

		/* collect indexes in this database that need a drain */
		LWLockAcquire(&lsm3_shared->lock[0], LW_SHARED);
		for (i = 0; i < LSM3_MAX_INDEXES; i++)
		{
			Lsm3IndexState *s = &lsm3_shared->indexes[i];

			if (OidIsValid(s->indexOid) &&
				s->databaseOid == MyDatabaseId &&
				s->merge_pending && !s->merge_running)
				pending[npending++] = s->indexOid;
		}
		LWLockRelease(&lsm3_shared->lock[0]);

		for (i = 0; i < npending; i++)
			lsm3_drain_merging(pending[i]);

		/* wait for latch (set by backends on swap) or timeout */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   lsm3_worker_naptime * 1000L,
					   lsm3_wait_event);
		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		CHECK_FOR_INTERRUPTS();
	}
}
