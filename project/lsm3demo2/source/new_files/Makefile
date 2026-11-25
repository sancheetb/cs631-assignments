# contrib/lsm/Makefile

MODULE_big = lsm
OBJS = \
	lsm.o \
	lsmctrl.o \
	lsmmerge.o \
	lsmworker.o \
	lsmfuncs.o

PGFILEDESC = "lsm - LSM tree index access method with background merge worker"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/lsm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
