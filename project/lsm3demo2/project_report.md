# Project Title : LSM trees in PostgreSQL

## Project Goals

| Roll Number | Name |
|---|---|
| 25M0767 | Chinchili Mohit |
| 25M0818 | Sancheet Basarikatti |
| 25M0771 | Vaibhav Joshi |
| 25M0763 | Arghyadeep Dhar |

---

Built on top of B-tree infrastructure; you can use a separate B-tree for each
of the LSM sub-trees and have a top-level index that wraps the lower level
B-tree indices. The interface to the rest of PostgreSQL remains that of a
B-tree: `CREATE INDEX ... USING btree` is unchanged, and `USING lsm` selects
the LSM implementation.

---

## Execution plan

1. **Design LSM structure** — levels, runs, memtable, compaction, and top-level index.
2. **Use PostgreSQL APIs** — represent each LSM run using a PostgreSQL table with a B-tree index instead of modifying PostgreSQL source.
3. **Implement core operations** — insert, search, delete, and range query.
4. **Implement compaction** — merge multiple runs and create a new B-tree-backed run.
5. **Implement top-level index** — track which runs/levels need to be searched.
6. **Benchmark** — compare our LSM implementation against a normal PostgreSQL B-tree for reads, writes, range queries, and storage.
7. **Analyze results** — discuss read/write amplification and performance trade-offs.

---

## LSM Tree Architecture

The implementation follows the "three-tree pattern" (as used by the
`postgrespro/lsm3` extension). One logical `lsm` index is backed by three real
nbtree B+ tree indexes, all created as ordinary PostgreSQL indexes on the same
heap table:

| Tree | Role |
|---|---|
| **T_active** (active top B-tree) | Small mutable tree that receives **all** inserts. Capped at 4,096 tuples so it stays resident in `shared_buffers`; writes therefore avoid random disk I/O into the large index. |
| **T_merging** (merging top B-tree) | A frozen batch of writes, or a pre-created empty "spare" tree. While frozen it is drained by a background worker into the base tree. |
| **T_base** (base B-tree) | The large persistent main index holding the bulk of the data. |

A small **control page** (block 0 of the parent index) persistently records the
three tree OIDs, a generation counter (used to name child trees uniquely), the
number of swaps, and the total number of entries drained into the base. A
**shared-memory structure** holds the live copy of these OIDs, a lock-free
insert counter, and flags coordinating the swap with the worker.

### How the operations work over nbtree

- **Write (INSERT):** routed exclusively to `T_active`. The hot path is as
  cheap as a normal B-tree insert: the child `Relation` and `IndexInfo` are
  cached in the parent's `rd_amcache`, and the insert counter is backend-local,
  so no control-page read/WAL write or catalog DDL happens per row.
- **Swap (compaction trigger):** once `T_active` crosses the 4,096-tuple cap,
  the access method atomically swaps roles in shared memory — the full
  `T_active` becomes `T_merging`, and the empty spare becomes the new
  `T_active`. This is a pure OID exchange, so writers are never blocked by
  index creation.
- **Background merge:** a `bgworker` (`lsm3merge`, registered at startup via
  `shared_preload_libraries = 'lsm'`) scans `T_merging` in sorted key order and
  inserts the entries into `T_base`, benefiting from sorted/batched locality.
  It discards tombstones permanently, then creates a fresh empty spare for the
  next swap — all in its own transaction, off the writer's path.
- **Read (SELECT):** index scans are opened on all three B-trees at once and
  the result streams are **k-way merged by key**. Heap TIDs already returned
  are de-duplicated with a hash table (the newest tree wins) so a TID caught
  mid-merge in both a top tree and the base is returned only once. The merge
  preserves global key order, so ordered scans and merge joins work.
- **Delete:** `DELETE` + `VACUUM` invoke `ambulkdelete`, which records a
  tombstone (high bit of the heap TID offset) in the newest tree; the worker
  permanently discards it during the drain into `T_base`.
- **Unique lookups:** `T_active` is checked first and the scan can short-circuit;
  the AM reports `amcanunique = false` as cross-level uniqueness enforcement is
  out of scope.

### Implementation notes

The AM is compiled as a loadable module `lsm.so` (so the AM and the bgworker
share the same global shared-memory pointer). Catalog entries in `pg_am.dat`
and `pg_proc.dat` register the `lsm` access method, its handler, and three SQL
functions. `indexcmds.c` aliases LSM operator-class lookups to the btree
operator families, so any type with a btree opclass works without duplicating
the operator-class catalog.

---

## How the benchmarks are conducted

All benchmarks use the bundled, compiled PostgreSQL 18.6 with the `lsm`
module, started by `./start.sh`.

- **Dataset:** `sample_data.csv` contains 500,000 rows `(id, category, payload)`.
  `load_sample_data.sql` loads it into two identical tables: `bench_btree`
  (regular B+ tree index) and `bench_lsm` (LSM index).
- **Cache configuration:** the server runs with `shared_buffers = 4 MB`. The
  B+ tree index on 500,000 rows is about **11 MB**, so it does not fit in
  cache. A `CHECKPOINT` is issued before each timed write so the B+ tree is
  cold and random inserts must fetch and split leaf pages on disk.
- **Workload:** inserts use random keys (`(random()*500000000)::int`), which is
  the write pattern LSM trees are designed for; sequential in-cache inserts
  are a B+ tree best case and are intentionally not used for the headline
  result.
- **Worker isolation:** the `lsm3.worker_naptime` GUC is raised to 3600 s to
  pause the background worker **only during each timed insert**, so the
  asynchronous drain into the base tree is not charged to the writer. It is
  reset immediately afterward, and `pg_sleep(3)` lets the worker drain between
  rounds. `lsm_status()` confirms each drain via the `swaps` and `extracted`
  counters.
- **Timing:** `\timing on` in psql measures wall-clock time. `EXPLAIN` is used
  to confirm both queries use `Index Scan`. Correctness is checked by equal
  row counts and a sample-key comparison (`mismatched = 0`).
- **Extended test:** `benchmark_inserts.sql` runs **four rounds of 150,000
  random-key inserts** (600,000 new rows, 1,100,000 total), each round doing a
  fresh swap + drain, so the trend as the B+ tree grows cold is visible.

Reproduce with a single command: `./run_demo.sh` (loads data, runs the
single-round benchmark, then the four-round insert benchmark).

---

## Results

### Point and range queries (500,000-row dataset)

| Operation | B+ tree | LSM tree | Observation |
|---|---|---|---|
| Point lookup `id = 250000` | 0.450 ms | 0.496 ms | Comparable; both use `Index Scan` |
| Range scan `200000..210000` (10,001 rows) | 4.369 ms | 5.387 ms | LSM ~23% slower: k-way merge of three B-trees |
| Ordered range (LSM) | — | 0.309 ms | Global key order preserved |

### Write benchmark — four rounds of 150,000 random-key inserts

| Round | B+ tree (ms) | LSM tree (ms) | LSM speedup |
|---|---|---|---|
| 1 | 1010.8 | 1137.1 | 0.89× (first-swap / cache warm-up) |
| 2 | 1855.9 | 1056.3 | 1.76× |
| 3 | 2503.0 | 1025.8 | 2.44× |
| 4 | 2779.9 | 1080.0 | **2.57×** |

The B+ tree insert latency grows ~2.8× (1011 → 2780 ms) as its index grows
beyond the 4 MB cache and random inserts cause disk I/O and page splits. The
LSM stays essentially flat (~1.0–1.1 s) because every insert goes to the small
memory-resident `T_active`; the expensive merge into `T_base` is done by the
background worker between rounds.

### LSM level progression (from the same run)

| After round | swaps | entries extracted into T_base |
|---|---|---|
| 1 | 1 | 150,000 |
| 2 | 2 | 300,000 |
| 3 | 3 | 450,000 |
| 4 | 4 | 600,000 |

**Correctness:** both tables ended with 1,100,000 rows; the sample-key
comparison returned `mismatched = 0`; the `lsm3merge` worker remained alive
throughout; no errors occurred.

---

## Analysis: read/write amplification and trade-offs

- **Write amplification:** the LSM converts random inserts into sequential,
  sorted merges into the base tree. The writer pays only a small in-memory
  insert; the worker pays the cost of merging into `T_base` asynchronously.
  This is why LSM insert latency stays flat while the B+ tree degrades, and why
  the speedup widens from ~0.9× to ~2.6× as the tree grows cold. The trade-off
  is extra total work (each entry is inserted into a top tree and later again
  into the base).
- **Read amplification:** a B+ tree lookup descends one tree; an LSM lookup may
  probe up to three B-trees and merge their streams, so wide range scans are
  slower (~23% in our run). Bloom-style short-circuiting and keeping the newest
  data in `T_active` keep point lookups competitive.
- **Storage:** three child B-trees plus a control page exist per logical index,
  and drained trees are retained until `DROP INDEX` (which cascades to all
  children via internal dependencies). This is modest overhead but higher than
  a single B+ tree.
- **Build cost:** creating the LSM index (~214 ms) is slightly slower than the
  B+ tree (~184 ms) because it builds three child trees and a control page.
- **Overall:** the design achieves its goal — fast, steady random-write
  latency by confining writes to a hot top tree and moving compaction to a
  background worker — at the cost of somewhat slower range reads and higher
  storage/build overhead. This is the classic LSM trade-off, now demonstrated
  inside PostgreSQL on top of ordinary nbtree B+ trees.
