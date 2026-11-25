-- Load the bundled 500,000-row dataset into two identical tables:
--   bench_btree -> regular B+ tree index
--   bench_lsm   -> LSM tree index (T_active / T_merging / T_base + bgworker)
\set ON_ERROR_STOP on
\timing on

DROP TABLE IF EXISTS bench_btree CASCADE;
DROP TABLE IF EXISTS bench_lsm CASCADE;

CREATE TABLE bench_btree (id integer, category integer, payload text);
CREATE TABLE bench_lsm   (id integer, category integer, payload text);

\echo 'Loading 500,000 rows into each table ...'
\copy bench_btree (id, category, payload) FROM 'sample_data.csv' WITH (FORMAT csv, HEADER true)
\copy bench_lsm   (id, category, payload) FROM 'sample_data.csv' WITH (FORMAT csv, HEADER true)

SELECT count(*) AS btree_rows FROM bench_btree;
SELECT count(*) AS lsm_rows   FROM bench_lsm;
