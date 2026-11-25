#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$ROOT/pg/lib:${LD_LIBRARY_PATH:-}"
"$ROOT/pg/bin/pg_ctl" -D "$ROOT/data" -m fast stop
