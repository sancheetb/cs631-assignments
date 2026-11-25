#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$ROOT/pg/lib:${LD_LIBRARY_PATH:-}"
PORT="${PORT:-5444}"
exec "$ROOT/pg/bin/psql" -h "$ROOT" -p "$PORT" -d postgres "$@"
