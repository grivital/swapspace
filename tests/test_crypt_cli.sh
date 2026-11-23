#!/bin/sh
# Command-line and configuration tests for the crypt options.
#
# These do not require root: crypt mode is only validated here (-i), never
# started.

BIN="${BIN:-../src/swapspace}"

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit 77; }

test -x "$BIN" || skip "swapspace binary not built (run 'make' first)"

# Startup in crypt mode looks for mappings named swapspace-<n> on this machine;
# a real one (a running crypt instance) would make the results depend on it.
for m in /dev/mapper/swapspace-*; do
  test -e "$m" && skip "a mapping named swapspace-<n> exists on this machine"
done

TMP=$(mktemp -d "${TMPDIR:-/tmp}/swapspace-crypt-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

cat >"$TMP/base.conf" <<EOF2
swappath = "$TMP"
lower_freelimit = 50
freetarget = 50
upper_freelimit = 60
psi_threshold = 5
EOF2

ok() {
  desc=$1; shift
  "$BIN" -i -c "$TMP/base.conf" "$@" >/dev/null 2>&1 || fail "rejected: $desc"
}
bad() {
  desc=$1; shift
  if "$BIN" -i -c "$TMP/base.conf" "$@" >/dev/null 2>&1; then
    fail "accepted: $desc"
  fi
}

# The new options are documented in the usage text.
"$BIN" -h 2>&1 | grep -q -- '--crypt_cipher' || fail "help lacks crypt_cipher"
"$BIN" -h 2>&1 | grep -q -- '-E,' || fail "help lacks -E"

# Defaults and the short and long forms.
ok "no crypt options"
ok "-E" -E
ok "--crypt" --crypt
ok "-E -X cipher -K size" -E -X aes-xts-plain64 -K 512
ok "long forms" --crypt --crypt_cipher=aes-xts-plain64 --crypt_keysize=256
ok "aes-cbc-essiv" -E -X aes-cbc-essiv:sha256 -K 256
ok "serpent 1024" -E -X serpent-cbc-plain64 -K 1024

# Key size range and cipher rules.
bad "keysize 64" -E -K 64
bad "keysize 1032" -E -K 1032
bad "keysize not a number" -E -K abc
bad "xts keysize 128" -E -K 128
bad "xts keysize 384" -E -K 384
bad "aes-cbc keysize 512" -E -X aes-cbc-essiv:sha256 -K 512
ok "bare aes keysize 256" -E -X aes -K 256
bad "bare aes keysize 512" -E -X aes -K 512
bad "keysize not multiple of 8" -E -X serpent-cbc-plain64 -K 130

# Cipher whitelist: nothing that a shell would interpret.
bad "cipher with semicolon" -E "--crypt_cipher=aes-xts-plain64;touch $TMP/pwned"
bad "cipher with space" -E "--crypt_cipher=aes xts"
bad "cipher with substitution" -E '--crypt_cipher=$(id)'
bad "cipher with backtick" -E '--crypt_cipher=`id`'
bad "cipher with quote" -E "--crypt_cipher=aes'"
bad "cipher leading dash" -E --crypt_cipher=-aes-xts-plain64
bad "empty cipher" -E --crypt_cipher=
bad "cipher too long" -E "--crypt_cipher=$(printf 'a%.0s' $(seq 1 64))"
bad "cipher with pipe (validated even when crypt is off)" "--crypt_cipher=a|b"
test ! -e "$TMP/pwned" || fail "cipher text was executed"

# Ciphers that protect nothing.
bad "null cipher" -E --crypt_cipher=cipher_null-ecb -K 128
bad "null cipher, other mode" -E --crypt_cipher=cipher_null-cbc-plain64 -K 128
bad "ecb mode" -E --crypt_cipher=aes-ecb -K 256

# The same checks apply to the configuration file.
printf 'crypt\ncrypt_cipher = "aes-xts-plain64"\ncrypt_keysize = 512\n' >"$TMP/c1.conf"
cat "$TMP/base.conf" "$TMP/c1.conf" >"$TMP/c1full.conf"
"$BIN" -i -c "$TMP/c1full.conf" >/dev/null 2>&1 || fail "valid crypt config rejected"

printf 'crypt_cipher = "aes;id"\n' >"$TMP/c2.conf"
cat "$TMP/base.conf" "$TMP/c2.conf" >"$TMP/c2full.conf"
if "$BIN" -i -c "$TMP/c2full.conf" >/dev/null 2>&1; then
  fail "config file cipher with semicolon accepted"
fi

printf 'crypt = 1\n' >"$TMP/c3.conf"
cat "$TMP/base.conf" "$TMP/c3.conf" >"$TMP/c3full.conf"
if "$BIN" -i -c "$TMP/c3full.conf" >/dev/null 2>&1; then
  fail "crypt flag with an argument accepted"
fi

# Startup in crypt mode never reuses an inactive old file, because its key is
# gone or it may be plain.  It deletes it, and does not touch other names.
# (This runs unprivileged: nothing is activated.)
mkdir "$TMP/old"
chmod 700 "$TMP/old"
printf 'swappath = "%s"\nlower_freelimit = 50\nfreetarget = 50\nupper_freelimit = 60\npsi_threshold = 5\n' "$TMP/old" >"$TMP/old.conf"
dd if=/dev/zero of="$TMP/old/0" bs=1k count=64 2>/dev/null
dd if=/dev/zero of="$TMP/old/7" bs=1k count=64 2>/dev/null
echo keep >"$TMP/old/notes"
"$BIN" -e -E -c "$TMP/old.conf" >"$TMP/old.log" 2>&1 || fail "-e -E on old files failed"
test ! -e "$TMP/old/0" || fail "old swapfile 0 kept in crypt mode"
test ! -e "$TMP/old/7" || fail "old swapfile 7 kept in crypt mode"
test -e "$TMP/old/notes" || fail "unrelated file deleted"
grep -q "crypt mode cannot reuse" "$TMP/old.log" || fail "no log line for deleted old file"

# With --paranoid the old file is overwritten before it is deleted.  A second
# name for the same data lets the test look at it afterwards.
yes M | head -c 65536 >"$TMP/old/2"
ln "$TMP/old/2" "$TMP/old/data-of-2"
"$BIN" -e -E --paranoid -c "$TMP/old.conf" >"$TMP/old2.log" 2>&1 ||
  fail "-e -E --paranoid on an old file failed"
test ! -e "$TMP/old/2" || fail "old swapfile 2 kept in crypt mode"
test "$(wc -c <"$TMP/old/data-of-2")" -eq 65536 || fail "wiped file changed size"
test "$(tr -d '\0' <"$TMP/old/data-of-2" | wc -c)" -eq 0 ||
  fail "old swapfile not wiped under --paranoid"
# Control: without --paranoid the contents are left as they were.
yes M | head -c 65536 >"$TMP/old/2"
ln -f "$TMP/old/2" "$TMP/old/data-of-2"
"$BIN" -e -E -c "$TMP/old.conf" >"$TMP/old3.log" 2>&1 || fail "-e -E failed"
test "$(tr -d '\0' <"$TMP/old/data-of-2" | wc -c)" -gt 0 ||
  fail "control: file was wiped without --paranoid"

# Crypt off keeps the erase path working unprivileged on an empty directory.
"$BIN" -e -c "$TMP/base.conf" >/dev/null 2>&1 || fail "-e failed with crypt off"

exit 0
