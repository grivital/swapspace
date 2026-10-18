AM_CFLAGS = --std=gnu99
AM_CPPFLAGS = -I$(top_srcdir)/src

check_PROGRAMS = test_calc
test_calc_SOURCES = test_calc.c

if NO_CONFIG
TESTS = test_calc
else
TESTS = test_calc test_cli.sh
endif

EXTRA_DIST = test_cli.sh
