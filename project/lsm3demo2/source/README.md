# Source code for the LSM index access method

This directory contains the source code used to build the `lsm.so` module that
is pre-compiled in `../pg/lib/lsm.so`. It is provided so the implementation can
be inspected, rebuilt, and modified.

## Layout

```
source/
├── new_files/                 files created by this project
│   ├── lsm3.h                 install as src/include/access/lsm3.h
│   ├── lsm.c                  AM handler, build/insert/scan/vacuum
│   ├── lsmctrl.c              control page, shared memory, child-tree creation
│   ├── lsmmerge.c             swap + background-worker drain
│   ├── lsmworker.c            _PG_init, shmem hooks, bgworker main
│   ├── lsmfuncs.c             SQL functions lsm_flush/compact/status
│   ├── Makefile               contrib/lsm/Makefile
│   └── meson.build            contrib/lsm/meson.build
└── edited_original_files/     files changed in the stock PostgreSQL 18.6 tree
    ├── pg_am.dat              adds the lsm AM (src/include/catalog/)
    ├── pg_proc.dat            adds handler + 3 SQL functions
    ├── indexcmds.c            aliases LSM opclass lookups to btree
    ├── Makefile               contrib/Makefile (adds SUBDIRS entry for lsm)
    └── meson.build            contrib/meson.build (adds subdir('lsm'))
```

## Rebuilding

1. Apply the new files into a PostgreSQL 18.6 source tree:
   - `new_files/lsm3.h` → `src/include/access/lsm3.h`
   - the rest of `new_files/` → `contrib/lsm/`
2. Overlay the five `edited_original_files/` onto their paths shown above.
3. Configure and build:

```bash
cd postgresql
./configure --prefix=$HOME/postgres-dir --enable-debug --without-icu
make -j4 && make install
cd contrib/lsm && make install
```

4. Because `pg_am.dat` / `pg_proc.dat` are bootstrap catalogs, create the
   cluster with the new `initdb` (the bundle's `start.sh` does this), and run
   the server with `shared_preload_libraries = 'lsm'` so the `lsm3merge`
   background worker is registered.

For a full function-by-function explanation of every file and every SQL
command, see `../explanation.md`. For the architecture and benchmark results,
see `../project_report.md`.
