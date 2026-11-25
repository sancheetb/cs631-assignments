-- ============================================================================
-- EXTENDED INSERT BENCHMARK: B+ tree vs LSM tree (MULTI-ROUND)
--
-- 500,000 rows + one B+ tree index + one LSM index.  FOUR rounds of 150,000
-- random-key inserts (600,000 new rows).  The B+ tree index (~11 MB) exceeds
-- shared_buffers (4 MB) so inserts do cold random I/O.  The LSM writes to its
-- small hot T_active and drains into T_base with the lsm3merge bgworker.
--
-- For each round the worker is paused only during the timed insert, then
-- resumed so it merges T_merging into T_base before the next round.  This
-- shows repeated swap+drain cycles and keeps T_active small.
-- ============================================================================
\set ON_ERROR_STOP on

SET enable_seqscan = off;
SET enable_bitmapscan = off;

\echo '===== SETUP: 500,000 rows + both indexes ====='
DROP TABLE IF EXISTS bench_btree CASCADE;
DROP TABLE IF EXISTS bench_lsm CASCADE;
CREATE TABLE bench_btree (id integer, category integer, payload text);
CREATE TABLE bench_lsm   (id integer, category integer, payload text);
\copy bench_btree (id, category, payload) FROM 'sample_data.csv' WITH (FORMAT csv, HEADER true)
\copy bench_lsm   (id, category, payload) FROM 'sample_data.csv' WITH (FORMAT csv, HEADER true)
CREATE INDEX bench_btree_idx ON bench_btree USING btree (id);
CREATE INDEX bench_lsm_idx   ON bench_lsm   USING lsm   (id);
SELECT pg_size_pretty(pg_relation_size('bench_btree_idx')) AS btree_index_size,
       pg_relation_size('bench_btree_idx') > 4*1024*1024 AS btree_exceeds_cache;
CHECKPOINT;

\echo ''
\echo '===== INSERT ROUNDS: 150,000 random keys each ====='

\echo '--- ROUND 1 ---'
ALTER SYSTEM SET lsm3.worker_naptime = 3600; SELECT pg_reload_conf(); SELECT pg_sleep(0.3);
CHECKPOINT;
\timing on
INSERT INTO bench_btree SELECT (random()*500000000)::int,(g%100)+1,'r1_'||g FROM generate_series(1,150000) g;
INSERT INTO bench_lsm   SELECT (random()*500000000)::int,(g%100)+1,'r1_'||g FROM generate_series(1,150000) g;
\timing off
SELECT * FROM lsm_status('bench_lsm_idx');
ALTER SYSTEM RESET lsm3.worker_naptime; SELECT pg_reload_conf();
SELECT pg_sleep(3);  -- worker drains T_merging -> T_base
SELECT * FROM lsm_status('bench_lsm_idx');

\echo '--- ROUND 2 ---'
ALTER SYSTEM SET lsm3.worker_naptime = 3600; SELECT pg_reload_conf(); SELECT pg_sleep(0.3);
CHECKPOINT;
\timing on
INSERT INTO bench_btree SELECT (random()*500000000)::int,(g%100)+1,'r2_'||g FROM generate_series(1,150000) g;
INSERT INTO bench_lsm   SELECT (random()*500000000)::int,(g%100)+1,'r2_'||g FROM generate_series(1,150000) g;
\timing off
SELECT * FROM lsm_status('bench_lsm_idx');
ALTER SYSTEM RESET lsm3.worker_naptime; SELECT pg_reload_conf();
SELECT pg_sleep(3);
SELECT * FROM lsm_status('bench_lsm_idx');

\echo '--- ROUND 3 ---'
ALTER SYSTEM SET lsm3.worker_naptime = 3600; SELECT pg_reload_conf(); SELECT pg_sleep(0.3);
CHECKPOINT;
\timing on
INSERT INTO bench_btree SELECT (random()*500000000)::int,(g%100)+1,'r3_'||g FROM generate_series(1,150000) g;
INSERT INTO bench_lsm   SELECT (random()*500000000)::int,(g%100)+1,'r3_'||g FROM generate_series(1,150000) g;
\timing off
SELECT * FROM lsm_status('bench_lsm_idx');
ALTER SYSTEM RESET lsm3.worker_naptime; SELECT pg_reload_conf();
SELECT pg_sleep(3);
SELECT * FROM lsm_status('bench_lsm_idx');

\echo '--- ROUND 4 ---'
ALTER SYSTEM SET lsm3.worker_naptime = 3600; SELECT pg_reload_conf(); SELECT pg_sleep(0.3);
CHECKPOINT;
\timing on
INSERT INTO bench_btree SELECT (random()*500000000)::int,(g%100)+1,'r4_'||g FROM generate_series(1,150000) g;
INSERT INTO bench_lsm   SELECT (random()*500000000)::int,(g%100)+1,'r4_'||g FROM generate_series(1,150000) g;
\timing off
SELECT * FROM lsm_status('bench_lsm_idx');
ALTER SYSTEM RESET lsm3.worker_naptime; SELECT pg_reload_conf();
SELECT pg_sleep(3);
SELECT * FROM lsm_status('bench_lsm_idx');

\echo ''
\echo '===== FINAL STATE ====='
SELECT * FROM lsm_status('bench_lsm_idx');
SELECT count(*) AS btree_rows FROM bench_btree;
SELECT count(*) AS lsm_rows   FROM bench_lsm;
SELECT count(*) AS mismatched FROM (
  (SELECT id FROM bench_btree WHERE id IN (1,250000,499999))
  EXCEPT
  (SELECT id FROM bench_lsm WHERE id IN (1,250000,499999))
) d;
SELECT pid, backend_type FROM pg_stat_activity WHERE backend_type='lsm3merge';
\echo '===== DONE ====='
