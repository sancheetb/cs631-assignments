# LSM Trees in PostgreSQL — Complete Package (source + compiled demo)

This package contains **both** the complete PostgreSQL 18.6 source tree with
the LSM-tree index access method already applied, and a ready-to-run compiled
demo bundle. Someone with no PostgreSQL installation can build PostgreSQL from
the source included here.

## Contents

```
postgresql_lsm3_demo_6/
├── README.md          this file
├── postgresql/        COMPLETE PostgreSQL 18.6 source tree, including:
│   ├── configure, Makefile, src/, contrib/, doc/, ...   (all stock sources)
│   ├── contrib/lsm/   the new LSM module source (lsm.c, lsmctrl.c, ...)
│   ├── src/include/access/lsm3.h   new shared header
│   ├── edited files: src/include/catalog/pg_am.dat, pg_proc.dat,
│   │   src/backend/commands/indexcmds.c, contrib/Makefile, contrib/meson.build
│   ├── explanation.md     file-by-file / function-by-function explanation
│   ├── project_report.md  architecture, benchmarks and results
│   └── README_LSM.md      original project README
└── lsm3demo2/         pre-compiled, ready-to-run bundle (no build needed)
    ├── start.sh, stop.sh, psql.sh, run_demo.sh
    ├── sample_data.csv (500,000 rows), benchmark.sql, benchmark_inserts.sql
    ├── README.md, RESULTS.txt, explanation.md, project_report.md
    ├── pg/            compiled PostgreSQL binaries + lsm.so + pageinspect
    └── source/        standalone copies of the new + edited source files
```

## Option A — Run the pre-compiled demo (fastest)

```bash
unzip postgresql_lsm3_demo_6.zip -d ~/
cd ~/postgresql_lsm3_demo_6/lsm3demo2
./run_demo.sh
```

`run_demo.sh` starts the server (with `shared_preload_libraries='lsm'` and
`shared_buffers=4MB`), loads the sample data, runs the single-round benchmark,
then the extended 4-round insert benchmark. See `README.md` there for details.

## Option B — Build PostgreSQL from the included source

The source tree in `postgresql/` is complete; no other download is needed.
It already contains the LSM changes (the catalog entries, opclass aliasing,
contrib build wiring, the new module, and the header).

```bash
cd ~/postgresql_lsm3_demo_6/postgresql
./configure --prefix=$HOME/postgres-built --enable-debug --without-icu
make -j4
make install
# Contrib modules are built separately (standard PostgreSQL behaviour):
make -C contrib
make -C contrib install
# or build only the LSM module:
make -C contrib/lsm && make -C contrib/lsm install
```

Because `pg_am.dat` / `pg_proc.dat` are bootstrap catalogs, a fresh `initdb`
is required, and the server must preload the module so the `lsm3merge`
background worker is registered:

```bash
export PGBIN=$HOME/postgres-built/bin
$PGBIN/initdb -D ~/pgdata -A trust
echo "shared_preload_libraries = 'lsm'" >> ~/pgdata/postgresql.conf
$PGBIN/pg_ctl -D ~/pgdata -l logfile -o "-p 5444 -k /tmp" start
$PGBIN/psql -h /tmp -p 5444 -d postgres \
  -c "SELECT amname FROM pg_am WHERE amname='lsm';"
```

If you extracted the project files into your own PostgreSQL source instead,
see `lsm3demo2/source/README.md` for the file-to-path mapping.

## Documentation

- `explanation.md` — every new and edited file, every data structure and
  function, and every SQL function with usage examples.
- `project_report.md` — architecture, benchmark methodology, results and
  analysis (in the report format used for the project submission).
- `RESULTS.txt` — recorded benchmark output.
