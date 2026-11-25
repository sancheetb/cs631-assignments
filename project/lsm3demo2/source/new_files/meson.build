# Copyright (c) 2022-2025, PostgreSQL Global Development Group

lsm_sources = files(
  'lsm.c',
  'lsmctrl.c',
  'lsmmerge.c',
  'lsmworker.c',
  'lsmfuncs.c',
)

if host_system == 'windows'
  lsm_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'lsm',
    '--FILEDESC', 'lsm - LSM tree index AM',])
endif

lsm = shared_module('lsm',
  lsm_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += lsm
contrib_install_libs += lsm
