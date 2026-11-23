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

# Compaction options: accepted range, rejected values, bare keywords.
compact_conf() {
  printf 'swappath = "%s"\n%s\n' "$TMP" "$1" >"$TMP/compact.conf"
  "$BIN" -i -c "$TMP/compact.conf" >/dev/null 2>&1
}

for v in 0 1 25 31; do
  compact_conf "compact_threshold = $v" ||
    fail "compact_threshold = $v rejected"
done
for v in -1 32 100 abc 1k; do
  if compact_conf "compact_threshold = $v"; then
    fail "compact_threshold = $v accepted"
  fi
done
for v in 1 2 16; do
  compact_conf "compact_factor = $v" || fail "compact_factor = $v rejected"
done
for v in 0 -1 17 abc 2k; do
  if compact_conf "compact_factor = $v"; then
    fail "compact_factor = $v accepted"
  fi
done

# The size cap takes the same unit suffixes as the other size options.
for v in 0 16g 512m 1t 8192000; do
  compact_conf "compact_max_swapsize = $v" ||
    fail "compact_max_swapsize = $v rejected"
done
for v in -1 abc 4x 99999999999999999999 100; do
  if compact_conf "compact_max_swapsize = $v"; then
    fail "compact_max_swapsize = $v accepted"
  fi
done

# Bare keywords need an argument.
for k in compact_threshold compact_factor compact_max_swapsize; do
  if compact_conf "$k"; then fail "bare $k accepted"; fi
done

# Threshold 0 (disabled) combines with any valid factor and cap.
compact_conf "compact_threshold = 0
compact_factor = 16
compact_max_swapsize = 0" || fail "disabled compaction with options rejected"

# Command line: long and short forms, and the same range rules.
"$BIN" -i -c "$TMP/good.conf" --compact_threshold=30 --compact_factor=4 \
  --compact_max_swapsize=8g >/dev/null 2>&1 ||
  fail "compaction long options rejected"
"$BIN" -i -c "$TMP/good.conf" -k 30 -K 4 -Z 8g >/dev/null 2>&1 ||
  fail "compaction short options rejected"
if "$BIN" -i -c "$TMP/good.conf" --compact_threshold=32 >/dev/null 2>&1; then
  fail "command-line compact_threshold=32 accepted"
fi
if "$BIN" -i -c "$TMP/good.conf" --compact_factor=0 >/dev/null 2>&1; then
  fail "command-line compact_factor=0 accepted"
fi
if "$BIN" -i -c "$TMP/good.conf" --compact_threshold >/dev/null 2>&1; then
  fail "bare --compact_threshold accepted"
fi
"$BIN" -h 2>&1 | grep -q -e --compact_threshold ||
  fail "-h does not list compact_threshold"

cat >"$TMP/syntax.conf" <<EOF
this is not valid
EOF

if "$BIN" -i -c "$TMP/syntax.conf" >/dev/null 2>&1; then
  fail "configuration syntax error accepted"
fi

exit 0
