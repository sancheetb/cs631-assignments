#!/usr/bin/env bash
# One-command demo: start the server, load the sample data, run the original
# single-round benchmark, then the extended 4-round insert benchmark that
# demonstrates the widening B+ tree vs LSM write-performance difference.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
"$ROOT/start.sh"

echo; echo "################ STEP 1: LOAD SAMPLE DATA ################"
"$ROOT/psql.sh" -f load_sample_data.sql

echo; echo "################ STEP 2: SINGLE-ROUND BENCHMARK ################"
"$ROOT/psql.sh" -f benchmark.sql

echo; echo "################ STEP 3: EXTENDED 4-ROUND INSERT BENCHMARK ################"
echo "Four rounds of 150,000 random-key inserts; watch the B+ tree slow down"
echo "while LSM insert latency stays flat (see RESULTS.txt for sample output)."
"$ROOT/psql.sh" -f benchmark_inserts.sql

echo; echo "Demo complete. Use ./psql.sh to explore, or see RESULTS.txt for recorded numbers."
