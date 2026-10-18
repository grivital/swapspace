AM_CFLAGS = --std=gnu99
AM_CPPFLAGS = -I$(top_srcdir)/src

check_PROGRAMS = test_calc
test_calc_SOURCES = test_calc.c

if FUZZING
check_PROGRAMS += fuzz_meminfo
fuzz_meminfo_SOURCES = fuzz_meminfo.c
fuzz_meminfo_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer,address
fuzz_meminfo_LDFLAGS = -fsanitize=fuzzer,address
fuzz_meminfo_LDADD = $(top_builddir)/src/memory.$(OBJEXT) \
                     $(top_builddir)/src/calc.$(OBJEXT) \
                     $(top_builddir)/src/log.$(OBJEXT)
endif

if NO_CONFIG
TESTS = test_calc
else
TESTS = test_calc test_cli.sh
endif

EXTRA_DIST = test_cli.sh
