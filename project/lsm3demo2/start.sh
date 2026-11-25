#!/usr/bin/env bash
# One-command launcher: initialize (once) and start the bundled PostgreSQL
# with the lsm module preloaded (needed for the background merge worker).
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PGBIN="$ROOT/pg/bin"
PGDATA="$ROOT/data"
SOCK="$ROOT"
PORT="${PORT:-5444}"

export LD_LIBRARY_PATH="$ROOT/pg/lib:${LD_LIBRARY_PATH:-}"

if [ ! -f "$PGDATA/PG_VERSION" ]; then
    echo "[start] Initializing a fresh database cluster ..."
    rm -rf "$PGDATA"
    "$PGBIN/initdb" -D "$PGDATA" -A trust --no-instructions
    # The lsm AM's bgworker is registered from the module's _PG_init(), which
    # only runs when the library is in shared_preload_libraries.
    echo "shared_preload_libraries = 'lsm'" >> "$PGDATA/postgresql.conf"
    # Small shared_buffers keeps the large base B-tree cold so the benchmark
    # exercises the real LSM write path (hot top tree vs random B-tree I/O).
    echo "shared_buffers = 4MB" >> "$PGDATA/postgresql.conf"
fi

if "$PGBIN/pg_ctl" -D "$PGDATA" status >/dev/null 2>&1; then
    echo "[start] Server already running on port $PORT."
else
    "$PGBIN/pg_ctl" -D "$PGDATA" -l "$ROOT/server.log" \
        -o "-p $PORT -k $SOCK" -w start
    echo "[start] Server started on port $PORT."
fi
echo
echo "Connect: ./psql.sh    Demo: ./run_demo.sh"
