AM_CFLAGS = --std=gnu99
AM_CPPFLAGS = -I$(top_srcdir)/src -I$(top_builddir)/src

check_PROGRAMS = test_calc test_crypt test_cryptdev test_swaps
test_calc_SOURCES = test_calc.c
test_crypt_SOURCES = test_crypt.c
test_cryptdev_SOURCES = test_cryptdev.c
test_swaps_SOURCES = test_swaps.c

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
TESTS = test_calc test_crypt test_cryptdev test_swaps
else
TESTS = test_calc test_crypt test_cryptdev test_swaps test_cli.sh test_crypt_cli.sh \
         test_crypt_root_bin.sh
endif

EXTRA_DIST = fake_system.h test_cli.sh test_crypt_cli.sh test_crypt_root_bin.sh root-integration.sh \
             crypt-root.sh
