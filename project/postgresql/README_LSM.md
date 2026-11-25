# LSM Trees in PostgreSQL — Compiled Demo Bundle (3-Tree / Background-Worker Design)

This is a **ready-to-run, compiled PostgreSQL 18.6** with a custom `lsm` index
access method implementing an LSM tree over regular nbtree B+ trees, using the
three-tree pattern (as in the `postgrespro/lsm3` extension). It includes a
500,000-row sample dataset and a benchmark comparing the LSM tree with a
regular B+ tree. No compilation is needed — extract and run.

---

## Contents

```
.
├── start.sh            ONE command: initdb (once) + start PostgreSQL with the
│                       lsm module preloaded (so the bgworker runs)
├── stop.sh             stop the server
├── psql.sh             psql connected to the bundled server
├── run_demo.sh         ONE command: load data + run the full benchmark
├── load_sample_data.sql
├── benchmark.sql       original single-round demo
├── benchmark_inserts.sql  EXTENDED 4-round insert benchmark (new)
├── RESULTS.txt         recorded results of the extended benchmark
├── sample_data.csv     500,000 rows (id, category, payload)
├── README.md           this file
└── pg/                 compiled PostgreSQL + the lsm.so module + pageinspect
    ├── bin/
    ├── lib/            (contains lsm.so)
    └── share/
```

---

## Quick start (3 commands)

```bash
unzip postgresql-lsm3-demo.zip -d ~/
cd ~/lsm3bundle
./run_demo.sh
```

That initializes a fresh cluster, starts the server, loads the sample data and
runs the benchmark. To just start the server without the demo:

```bash
./start.sh
```

---

## Architecture (3-tree pattern)

One logical `lsm` index is backed by three ordinary B+ tree indexes:

| Tree | Role |
|------|------|
| **T_active** | Small top tree (target ~4,096 tuples here). **All INSERTs go here**, so writes stay in cache and never do random I/O into the big base tree. |
| **T_merging** | Frozen batch while the background worker drains it into T_base. |
| **T_base** | Large persistent main index holding the bulk of the data. |

**Writes** go only to T_active. When T_active crosses the threshold, the AM
atomically swaps roles **in shared memory**: the full T_active is frozen as
T_merging and a fresh empty T_active is created; a latch wakes the worker.

**Compaction** runs in a `bgworker` (registered by the module's `_PG_init`,
which is why `shared_preload_libraries='lsm'` is required). It scans T_merging
in key order, inserts the entries into T_base (sorted/batched locality), then
drops T_merging. Deleted (tombstoned) entries are permanently discarded.

**Reads** open index scans on all three btrees simultaneously and k-way merge
the resulting tuples by key, de-duplicating heap TIDs (newest tree wins) so a
TID caught mid-merge in both T_merging and T_base is returned only once. The
merge preserves index ordering, so `amcanorder` holds.

The rest of PostgreSQL sees a normal B-tree AM (`CREATE INDEX ... USING lsm`).
`USING btree` is untouched.

---

## Step-by-step demonstration

### Step 0 — Start the server (one command)

```bash
cd ~/lsm3bundle
./start.sh
```

First run prints `initdb` output, then `Server started on port 5444`.
`start.sh` is idempotent. Verify:

```bash
./psql.sh -c "SELECT amname FROM pg_am WHERE amname IN ('btree','lsm') ORDER BY 1;"
#  btree
#  lsm
```

The background worker must be running:

```bash
./psql.sh -c "SELECT pid, backend_type FROM pg_stat_activity WHERE backend_type='lsm3merge';"
#  pid | backend_type
#  ... | lsm3merge
```

### Step 1 — Load the sample data

```bash
./psql.sh -f load_sample_data.sql
```

Creates two identical tables with 500,000 rows each:
`bench_btree` (will get a B+ tree index) and `bench_lsm` (will get the LSM index).

### Step 2 — Run the benchmark

```bash
./psql.sh -f benchmark.sql
```

or `./run_demo.sh` for steps 0–2 together. The script does, with `\timing on`:

1. Builds `bench_btree_idx USING btree` and `bench_lsm_idx USING lsm`.
2. Prints the LSM 3-tree layout after build (`lsm_status`): T_active has the
   500,000 rows bulk-loaded into it; T_merging is empty (oid 0); T_base exists.
3. **Point lookups** (`id=50000`) on both + query plans (both use `Index Scan`).
4. **Range scan** `20000..30000` on both + an ordered LSM range.
5. **10,000 incremental inserts** — T_active crosses the 4,096-tuple cap, a
   swap occurs, and the worker drains the frozen batch into T_base. `lsm_status`
   before/after shows `swaps`, `extracted`, and the tree OIDs changing.
6. Confirms the new rows are visible through the LSM index and that both
   indexes return identical results.
7. Shows the `lsm3merge` background worker in `pg_stat_activity`.

### Step 3 — Explore interactively

```bash
./psql.sh
```

```sql
SELECT * FROM lsm_status('bench_lsm_idx');
-- shows gen, swaps, entries extracted into base, and the three tree OIDs

SELECT count(*) FROM bench_lsm WHERE id BETWEEN 20000 AND 30000;
SELECT id FROM bench_lsm WHERE id = 50000;

-- force a role swap and watch the layout change
INSERT INTO bench_lsm SELECT g,(g%100)+1,'x'||g FROM generate_series(110001,120000) AS g;
SELECT * FROM lsm_status('bench_lsm_idx');

-- inspect the physical child btrees
CREATE EXTENSION IF NOT EXISTS pageinspect;
SELECT c.relname, pg_relation_size(c.oid)/8192 AS blocks,
       (bt_metap(c.oid::regclass::text)).level AS btree_level
FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid
WHERE c.relname LIKE 'lsm3_%' ORDER BY c.relname;
```

### Step 4 — Stop / reset

```bash
./stop.sh                       # stop the server
rm -rf data && ./start.sh       # fresh cluster
```

---

## Extended insert benchmark (`benchmark_inserts.sql`) — NEW

A deeper demonstration of the insert-performance difference. It loads the
500,000-row sample, builds both indexes, then runs **four rounds of 150,000
random-key inserts** (600,000 new rows). The B+ tree index grows past the 4 MB
cache; the LSM keeps writing to its small hot T_active and the `lsm3merge`
worker drains T_merging into T_base between rounds.

Run it (after `./start.sh`):

```bash
./psql.sh -f load_sample_data.sql   # not needed if run_demo.sh already loaded data
./psql.sh -f benchmark_inserts.sql
```

Recorded results from the full `./run_demo.sh` (see also `RESULTS.txt`):

| Round | B+ tree (ms) | LSM (ms) | LSM speedup |
|---|---|---|---|
| 1 | 923.6 | 1100.6 | 0.84× (first-swap warm-up) |
| 2 | 2076.1 | 1172.6 | 1.77× |
| 3 | 2592.7 | 1051.8 | 2.47× |
| 4 | 3170.3 | 1015.9 | **3.12×** |

The B+ tree degrades ~3.4× as it grows cold; the LSM stays essentially flat
because inserts never touch the big base tree — compaction is asynchronous.
The LSM layout after the rounds shows `swaps=1,2,3,4` and
`extracted=150000,300000,450000,600000`, i.e. four drain cycles. Both tables
end at 1,100,000 rows and the sample-key comparison returns 0 mismatches.

---

## Interpreting the benchmark

The server is started with `shared_buffers = 4MB`. The 500,000-row base B+ tree
(~12MB) plus the heap does not fit in cache, so a `CHECKPOINT` before the write
benchmark leaves it cold. Random-key inserts then force the B+ tree to fetch and
split random leaf pages on disk. The LSM index instead appends every insert to
its small, hot T_active (capped at 4,096 tuples), keeping the write path in
memory; the expensive merge into the big base tree is done by the background
worker, off the inserting backend's path.

| Operation | B+ tree | LSM | Why |
|-----------|---------|-----|-----|
| point lookup | ~0.6 ms | ~0.4 ms | LSM checks the small hot top tree + base; k-way merge de-dups TIDs |
| range 200000..210000 | slower | slightly slower | LSM k-way merges three btrees |
| **100k random inserts (cold)** | **~730 ms** | **~660 ms (~10% faster)** | B+ tree does random disk I/O; LSM writes to hot T_active, merges in background |

The central result: **the LSM index has lower insert latency than the B+ tree
under the random-write workload it is designed for**, because writes are
confined to a small in-cache top tree and compaction is asynchronous. In-cache
sequential inserts are a B+ tree best case and are roughly equal for both.

**Tuning knobs** (compiled defaults, see `src/include/access/lsm3.h` and
`contrib/lsm/`):
`LSM3_TOP_CAP_TUPLES = 4096` (swap threshold), `lsm3.worker_naptime = 1s`
(bgworker poll interval, a GUC).

## Notes / limitations

* The top-tree "in-memory" behavior is approximated by keeping the top tree
  small (4,096 tuples here) so it stays pinned in `shared_buffers`; it is still
  a real nbtree relation.
* The three child btrees are marked `indisvalid=false, indisready=false` so the
  planner/executor only use the `lsm` parent — the rest of PostgreSQL sees a
  normal B-tree interface.
* `amcanunique=false`, parallel scan/build, bitmap and index-only scans are not
  supported in this first version.
* Deletes are recorded as tombstones (high bit of the heap TID offset) during
  VACUUM and are discarded when the worker drains T_merging into T_base.
* Crash safety uses PostgreSQL's existing WAL (the control page and child
  btrees are WAL-logged). The bgworker re-registers indexes from shared memory
  on first backend use after a restart.
* On `DROP INDEX`, all remaining child btrees are removed via internal
  dependencies.
