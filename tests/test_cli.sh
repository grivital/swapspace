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

# file_priority is 0 (kernel default) or 31 (one per swapfile slot, so none goes
# negative) up to the kernel's maximum of 32767.
for prio in 0 31 100 32767; do
  printf 'swappath = "%s"\nfile_priority = %s\n' "$TMP" "$prio" \
    >"$TMP/prio.conf"
  "$BIN" -i -c "$TMP/prio.conf" >/dev/null 2>&1 ||
    fail "valid file_priority $prio rejected"
done
for prio in 1 30 32768 -1; do
  printf 'swappath = "%s"\nfile_priority = %s\n' "$TMP" "$prio" \
    >"$TMP/prio.conf"
  if "$BIN" -i -c "$TMP/prio.conf" >/dev/null 2>&1; then
    fail "invalid file_priority $prio accepted"
  fi
done

# The command line takes the same range checks.
"$BIN" -i -c "$TMP/good.conf" -F 31 >/dev/null 2>&1 ||
  fail "-F 31 rejected"
if "$BIN" -i -c "$TMP/good.conf" -F 30 >/dev/null 2>&1; then
  fail "-F 30 accepted"
fi

cat >"$TMP/syntax.conf" <<EOF
this is not valid
EOF

if "$BIN" -i -c "$TMP/syntax.conf" >/dev/null 2>&1; then
  fail "configuration syntax error accepted"
fi

exit 0
