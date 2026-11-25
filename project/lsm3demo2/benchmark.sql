-- Benchmark: LSM tree vs regular B+ tree on the bundled sample data.
-- The server uses shared_buffers=16MB, so the large base B+ tree is cold and
-- random inserts into it incur disk I/O, while the LSM's small T_active stays
-- pinned in memory.  This is the workload LSM trees are designed for.
\set ON_ERROR_STOP on
\timing on

SET enable_seqscan = off;
SET enable_bitmapscan = off;

\echo '=== Build indexes on 500,000 rows ==='
CREATE INDEX bench_btree_idx ON bench_btree USING btree (id);
CREATE INDEX bench_lsm_idx   ON bench_lsm   USING lsm   (id);
SELECT pg_size_pretty(pg_relation_size('bench_btree_idx')) AS btree_size;

\echo '=== LSM three-tree layout after build ==='
SELECT * FROM lsm_status('bench_lsm_idx');

\echo '=== POINT LOOKUP (id = 250000) ==='
SELECT id,payload FROM bench_btree WHERE id=250000;
SELECT id,payload FROM bench_lsm   WHERE id=250000;
EXPLAIN (COSTS OFF) SELECT * FROM bench_btree WHERE id=250000;
EXPLAIN (COSTS OFF) SELECT * FROM bench_lsm   WHERE id=250000;

\echo '=== RANGE SCAN 200000..210000 ==='
SELECT count(*) FROM bench_btree WHERE id BETWEEN 200000 AND 210000;
SELECT count(*) FROM bench_lsm   WHERE id BETWEEN 200000 AND 210000;

\echo '=== ORDERED RANGE (lsm) ==='
SELECT id FROM bench_lsm WHERE id BETWEEN 100 AND 110 ORDER BY id;

\echo '=== WRITE BENCHMARK: 100,000 RANDOM-KEY INSERTS ==='
\echo '    (cold B-tree does random I/O; LSM writes to the small hot T_active.'
\echo '     The background merge worker is paused during the timed insert so it'
\echo '     does not compete for cache; it is resumed right afterward.)'
-- Pause the worker: the drain is asynchronous work that should not be charged
-- to the inserting backend.
ALTER SYSTEM SET lsm3.worker_naptime = 3600;
SELECT pg_reload_conf();
SELECT pg_sleep(0.3);
CHECKPOINT;
\timing on
INSERT INTO bench_btree
  SELECT (random()*500000000)::int,(g%100)+1,'new_'||g
  FROM generate_series(1,100000) AS g;
INSERT INTO bench_lsm
  SELECT (random()*500000000)::int,(g%100)+1,'new_'||g
  FROM generate_series(1,100000) AS g;
\timing off
ALTER SYSTEM RESET lsm3.worker_naptime;
SELECT pg_reload_conf();

\echo '=== LSM layout: swap frozen T_active into T_merging for the worker ==='
SELECT * FROM lsm_status('bench_lsm_idx');
\echo '=== let the background worker drain T_merging into T_base ==='
SELECT pg_sleep(3);
SELECT * FROM lsm_status('bench_lsm_idx');

\echo '=== new rows visible + identical results from both indexes ==='
SELECT id FROM bench_lsm WHERE id IN (1,250000,499999) ORDER BY id;
SELECT count(*) AS btree_count FROM bench_btree;
SELECT count(*) AS lsm_count   FROM bench_lsm;

\echo '=== background merge worker ==='
SELECT pid, backend_type FROM pg_stat_activity WHERE backend_type='lsm3merge';

RESET enable_seqscan;
RESET enable_bitmapscan;
\echo '=== DONE ==='
