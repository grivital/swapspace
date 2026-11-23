#!/bin/sh
# Basic command-line and configuration integration tests.
#
# These do not require root: they only exercise parsing, validation, and the
# erase path on an empty directory.

BIN="${BIN:-../src/swapspace}"

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit 77; }

test -x "$BIN" || skip "swapspace binary not built (run 'make' first)"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/swapspace-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$BIN" -V >/dev/null 2>&1 || fail "-V failed"
"$BIN" -h >/dev/null 2>&1 || fail "-h failed"

if "$BIN" --definitely-not-an-option >/dev/null 2>&1; then
  fail "invalid option accepted"
fi

# Thresholds chosen so that minimal_swapfile() is zero: the test then does not
# depend on how much space the build filesystem happens to have.
cat >"$TMP/good.conf" <<EOF
swappath = "$TMP"
lower_freelimit = 50
freetarget = 50
upper_freelimit = 60
psi_threshold = 5
EOF

"$BIN" -i -c "$TMP/good.conf" >/dev/null 2>&1 ||
  fail "valid configuration rejected"

# Erasing an empty swap directory must succeed without root privileges.
"$BIN" -e -c "$TMP/good.conf" >/dev/null 2>&1 ||
  fail "-e on an empty swap directory failed"

cat >"$TMP/unknown.conf" <<EOF
not_an_option = 1
EOF

if "$BIN" -i -c "$TMP/unknown.conf" >/dev/null 2>&1; then
  fail "unknown configuration item accepted"
fi

cat >"$TMP/overflow.conf" <<EOF
max_swapsize = 99999999999999999999
EOF

if "$BIN" -i -c "$TMP/overflow.conf" >/dev/null 2>&1; then
  fail "out-of-range numeric option accepted"
fi

cat >"$TMP/badpsi.conf" <<EOF
swappath = "$TMP"
psi_threshold = 101
EOF

if "$BIN" -i -c "$TMP/badpsi.conf" >/dev/null 2>&1; then
  fail "out-of-range psi_threshold accepted"
fi

# Absolute free-swap band
cat >"$TMP/abs.conf" <<EOF
swappath = "$TMP"
min_free_swap = 4m
max_free_swap = 8m
EOF

"$BIN" -i -c "$TMP/abs.conf" >/dev/null 2>&1 ||
  fail "valid absolute free-swap configuration rejected"

cat >"$TMP/absbad.conf" <<EOF
swappath = "$TMP"
min_free_swap = 8m
max_free_swap = 4m
EOF

if "$BIN" -i -c "$TMP/absbad.conf" >/dev/null 2>&1; then
  fail "max_free_swap below min_free_swap accepted"
fi

# max_free_swap needs min_free_swap
cat >"$TMP/maxonly.conf" <<EOF
swappath = "$TMP"
max_free_swap = 8m
EOF

if "$BIN" -i -c "$TMP/maxonly.conf" >/dev/null 2>&1; then
  fail "max_free_swap without min_free_swap accepted"
fi

# The accepted range stops at a quarter of the largest 64-bit size.  The top of
# the range is accepted with a zero-width band (equal minimum and maximum), which
# also keeps minimal_swapfile() at zero.
TOP=2305843009213693951
printf 'swappath = "%s"\nmin_free_swap = %s\nmax_free_swap = %s\n' "$TMP" "$TOP" "$TOP" >"$TMP/range.conf"
"$BIN" -i -c "$TMP/range.conf" >/dev/null 2>&1 || fail "free swap values at the top of their range rejected"
for opt in min_free_swap max_free_swap; do
  printf 'swappath = "%s"\n%s = 2305843009213693952\n' "$TMP" "$opt" >"$TMP/range.conf"
  "$BIN" -i -c "$TMP/range.conf" 2>&1 | grep -q "'$opt': given value too large" ||
    fail "$opt beyond its range not rejected as too large"
done

cat >"$TMP/syntax.conf" <<EOF
this is not valid
EOF

if "$BIN" -i -c "$TMP/syntax.conf" >/dev/null 2>&1; then
  fail "configuration syntax error accepted"
fi

exit 0
