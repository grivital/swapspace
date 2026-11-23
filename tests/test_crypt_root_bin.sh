#!/bin/sh
# Checks how tests/crypt-root.sh picks and vets the swapspace binary.  The
# script stops at its root check, after printing the binary, so this needs no
# privileges and never gets to the destructive part (it is skipped as root).

skip() { echo "SKIP: $*" >&2; exit 77; }
fail() { echo "FAIL: $*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || skip "running as root"
SCRIPT=${srcdir:-.}/crypt-root.sh
[ -f "$SCRIPT" ] || skip "no crypt-root.sh"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/swapspace-bin-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
TMP=$(cd "$TMP" && pwd -P)

# A stand-in binary: -h lists the crypt option unless NOCRYPT, -V a version.
stub() {
  mkdir -p "$(dirname "$1")"
  cat >"$1" <<EOF2
#!/bin/sh
case \$1 in
  -h) echo "  -E, --crypt  Encrypt"; $2 ;;
  -V) echo "swapspace stub $3" ;;
esac
EOF2
  chmod 755 "$1"
}

# The script under test sits in one tree; the current directory is another one
# whose ../src/swapspace must be ignored.
mkdir -p "$TMP/real/tests" "$TMP/other/tests"
cp "$SCRIPT" "$TMP/real/tests/crypt-root.sh"
stub "$TMP/real/src/swapspace" : real
stub "$TMP/other/src/swapspace" : other

run() {
  # run [VAR=value...]: from the other tree, with the guard variable set
  (cd "$TMP/other/tests" &&
   env SWAPSPACE_CRYPT_TEST=1 "$@" sh "$TMP/real/tests/crypt-root.sh" /tmp) 2>&1
}

out=$(run) || true
echo "$out" | grep -q "binary: $TMP/real/src/swapspace (swapspace stub real)" ||
  fail "did not use the binary next to the script: $out"
echo "$out" | grep -q "$TMP/other" && fail "used a binary from the current directory"

out=$(run BIN="$TMP/other/src/swapspace") || true
echo "$out" | grep -q "binary: $TMP/other/src/swapspace " ||
  fail "ignored an explicit BIN: $out"

out=$(run BIN=other/src/swapspace) && fail "accepted a relative BIN"
echo "$out" | grep -q "absolute" || fail "relative BIN: wrong message: $out"

out=$(run BIN="$TMP/real/tests") && fail "accepted a directory as BIN"

stub "$TMP/nocrypt/src/swapspace" ":" x
sed 's/--crypt//' "$TMP/nocrypt/src/swapspace" >"$TMP/nocrypt/plain"
chmod 755 "$TMP/nocrypt/plain"
out=$(run BIN="$TMP/nocrypt/plain") && fail "accepted a binary without crypt"
echo "$out" | grep -q "does not support crypt" || fail "no-crypt message: $out"

cp "$TMP/real/src/swapspace" "$TMP/real/src/gw"
chmod 775 "$TMP/real/src/gw"
out=$(run BIN="$TMP/real/src/gw") && fail "accepted a group-writable binary"
chmod 757 "$TMP/real/src/gw"
out=$(run BIN="$TMP/real/src/gw") && fail "accepted a world-writable binary"

out=$(run PKEXEC_UID=$(($(id -u) + 1))) && fail "accepted a binary of another user"
echo "$out" | grep -q "owned by uid" || fail "owner message: $out"

echo "PASS: crypt-root.sh binary selection"
