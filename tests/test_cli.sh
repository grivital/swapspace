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
IDLE_PID=
REL_PID=
FAIL_PID=
trap 'kill $IDLE_PID $REL_PID $FAIL_PID 2>/dev/null; rm -rf "$TMP"' EXIT HUP INT TERM

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

cat >"$TMP/syntax.conf" <<EOF
this is not valid
EOF

if "$BIN" -i -c "$TMP/syntax.conf" >/dev/null 2>&1; then
  fail "configuration syntax error accepted"
fi

# SIGHUP reloads the configuration: the process id stays the same and the new
# process reads the configuration again, so it takes the lock in the swap directory
# that the rewritten file names.  Any swapfile the daemon tries to make is a few KiB
# and swapon fails without root, which the daemon survives.
mkdir "$TMP/dirA" "$TMP/dirB" || exit 1
chmod 700 "$TMP/dirA" "$TMP/dirB"
idle_conf() {
  cat >"$1" <<CONF
swappath = "$2"
lower_freelimit = 50
freetarget = 50
upper_freelimit = 100
min_swapsize = 262144
max_swapsize = 262144
psi_threshold = 0
CONF
}

idle_conf "$TMP/idle.conf" "$TMP/dirA"
"$BIN" -c "$TMP/idle.conf" >"$TMP/idle.out" 2>&1 &
IDLE_PID=$!
sleep 2
kill -0 "$IDLE_PID" 2>/dev/null || { cat "$TMP/idle.out" >&2; fail "daemon did not start"; }
test -e "$TMP/dirA/.swapspace.lock" || fail "daemon did not lock the first swap directory"
test ! -e "$TMP/dirB/.swapspace.lock" || fail "second swap directory locked before the reload"
idle_conf "$TMP/idle.conf" "$TMP/dirB"
kill -HUP "$IDLE_PID"
sleep 3
kill -0 "$IDLE_PID" 2>/dev/null || { cat "$TMP/idle.out" >&2; fail "daemon exited on SIGHUP instead of reloading"; }
grep -q "Reloading configuration" "$TMP/idle.out" || { cat "$TMP/idle.out" >&2; fail "no reload logged after SIGHUP"; }
test -e "$TMP/dirB/.swapspace.lock" || { cat "$TMP/idle.out" >&2; fail "reload did not re-read the configuration"; }
test "$(readlink "/proc/$IDLE_PID/cwd")" = "$TMP/dirB" || fail "reloaded daemon is not in the new swap directory"

# SIGHUPs arriving while the new process starts must not kill it: it inherits the
# blocked signal until its handlers are installed.
n=0
while [ $n -lt 600 ]; do
  kill -HUP "$IDLE_PID" 2>/dev/null || break
  sleep 0.005
  n=$((n + 1))
done
sleep 2
kill -0 "$IDLE_PID" 2>/dev/null || { cat "$TMP/idle.out" >&2; fail "daemon killed by SIGHUPs sent during a reload"; }
kill -TERM "$IDLE_PID"
wait "$IDLE_PID" 2>/dev/null

# A reload must work when the configuration file was given as a relative path
BINDIR=$(cd "$(dirname "$BIN")" && pwd)
idle_conf "$TMP/rel.conf" "$TMP/dirA"
rm -f "$TMP/dirB/.swapspace.lock"
(cd "$TMP" && exec "$BINDIR/$(basename "$BIN")" -c rel.conf) >"$TMP/rel.out" 2>&1 &
REL_PID=$!
sleep 2
kill -0 "$REL_PID" 2>/dev/null || { cat "$TMP/rel.out" >&2; fail "daemon did not start with a relative configuration path"; }
idle_conf "$TMP/rel.conf" "$TMP/dirB"
kill -HUP "$REL_PID"
sleep 3
kill -0 "$REL_PID" 2>/dev/null || { cat "$TMP/rel.out" >&2; fail "daemon died on reload with a relative configuration path"; }
grep -q "Reloading configuration" "$TMP/rel.out" || fail "no reload logged for the relative path run"
test -e "$TMP/dirB/.swapspace.lock" || { cat "$TMP/rel.out" >&2; fail "relative-path reload did not re-read the configuration"; }
kill -TERM "$REL_PID"
wait "$REL_PID" 2>/dev/null

# If the re-execute fails the daemon must carry on in its swap directory, not in the
# directory it was started from.  Taking the execute permission off the running
# binary makes the exec fail.
mkdir "$TMP/bin" "$TMP/start" || exit 1
cp "$BIN" "$TMP/bin/swapspace" || exit 1
idle_conf "$TMP/fail.conf" "$TMP/dirA"
(cd "$TMP/start" && exec "$TMP/bin/swapspace" -c "$TMP/fail.conf") >"$TMP/fail.out" 2>&1 &
FAIL_PID=$!
sleep 2
kill -0 "$FAIL_PID" 2>/dev/null || { cat "$TMP/fail.out" >&2; fail "daemon did not start from a copy of the binary"; }
chmod 000 "$TMP/bin/swapspace"
kill -HUP "$FAIL_PID"
sleep 3
kill -0 "$FAIL_PID" 2>/dev/null || { cat "$TMP/fail.out" >&2; fail "daemon died when the re-execute failed"; }
grep -q "Could not re-execute" "$TMP/fail.out" || { cat "$TMP/fail.out" >&2; fail "failed re-execute not logged"; }
test "$(readlink "/proc/$FAIL_PID/cwd")" = "$TMP/dirA" || fail "daemon left its swap directory after a failed re-execute"
kill -TERM "$FAIL_PID"
wait "$FAIL_PID" 2>/dev/null

exit 0
