#!/bin/sh
# Destructive integration test for crypt mode (encrypted swapfiles).
#
# RUN ONLY ON A MACHINE WHERE TINY TEMPORARY SWAP AREAS ARE ACCEPTABLE, as root.
# This test activates real swap (a few files of 4 to 8 MB each), creates loop
# devices and dm-crypt mappings named swapspace-<n>, and removes them again.
# It refuses to run unless SWAPSPACE_CRYPT_TEST=1 is set, and refuses to start
# if a mapping named swapspace-* already exists or a swapspace process that may
# use crypt mode is running (either could belong to a real instance).  It only
# ever swaps off, closes or detaches things that it created: mappings whose
# loop device is backed by a file in its own directory, and whose names it
# recorded, plus those loop devices and the plain files there.
#
# Usage, as root from the build tree:
#   SWAPSPACE_CRYPT_TEST=1 ./tests/crypt-root.sh [parent-directory]
#
# From a desktop session with polkit, pkexec starts it as root.  It strips the
# environment: pass variables through env(1) before the script path (after it,
# BIN would be an argument and be ignored) and use absolute paths:
#   pkexec env SWAPSPACE_CRYPT_TEST=1 BIN=/abs/path/to/src/swapspace \
#     /abs/path/to/tests/crypt-root.sh /var/tmp
# The binary is BIN if set, else src/swapspace next to this script, never one
# found through the current directory.  It must be an absolute path to a regular
# executable owned by root or the invoking user (PKEXEC_UID, SUDO_UID), not
# writable by group or others, and its -h must list crypt.  Its path and
# version are printed.
#
# The parent directory (default /var/tmp) must be on a filesystem and device
# where O_DIRECT works with the loop device's block size, which swapspace takes
# from the file's STATX_DIOALIGN (typically the filesystem block size, 4096 on
# 4Kn storage).  If direct I/O is not available, the test reports that crypt
# mode refused to start there.  A throwaway directory is created inside the
# parent, which is resolved to its real path first, and removed at the end,
# including after failures.  Cleanup only touches mappings and loop devices
# whose backing file lies inside that directory.  The parent must not be on
# tmpfs or ramfs (the loop device needs direct I/O) and needs 600 MB free.
#
# Cleanup ignores INT, TERM and HUP so that it always finishes.  After SIGKILL
# of the script, or a reported cleanup problem, check by hand: "dmsetup ls"
# (swapspace-* mappings), "losetup -l" (loops backed by <parent>/swapspace-
# crypt-test.*), "pgrep -x swapspace" (stray daemons, up to 32 x 8 MB), and
# "BIN -c <parent>/swapspace-crypt-test.*/crypt.conf -e" to retire the rest.
#
# Requires: cryptsetup, dmsetup, losetup, mkswap, swapoff, dd, grep, awk, pgrep,
# tr, sed, and kernel support for loop (Linux 5.8 or later) and dm-crypt.
#
# Scenarios:
#  1. allocation: files are created, mapped and enabled; /proc/swaps lists the
#     mapper devices and never the plain files; cipher, key size, direct I/O
#     and random distinct keys are visible in the kernel's state
#  2. adoption: a restarted daemon adopts the active mappings unchanged
#  3. confidentiality: a marker written through the mapping does not appear in
#     the backing file; startup with crypt on removes a leftover mapping and
#     deletes its file
#  4. erase with crypt off: -e retires encrypted swapfiles anyway
#  5. paranoid: stopping the daemon retires everything and leaves nothing
#  6. fail closed: with a cipher the kernel does not have, no swap is enabled
#     and nothing is left behind
#  7. another instance: a second swap directory holding a file with the same
#     number does not adopt, and does not close, the first one's mapping, in
#     crypt mode or with crypt off

if [ "${SWAPSPACE_CRYPT_TEST:-0}" != "1" ]; then
  echo "Refusing to run: set SWAPSPACE_CRYPT_TEST=1 (this test activates real" >&2
  echo "swap and creates loop and dm-crypt devices)" >&2
  exit 77
fi

scriptdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 2
BIN=${BIN:-${scriptdir%/*}/src/swapspace}
case $BIN in
  /*) ;;
  *) echo "BIN must be an absolute path: $BIN" >&2; exit 2 ;;
esac
if [ ! -f "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "Not an executable regular file: $BIN (build swapspace or set BIN)" >&2
  exit 2
fi
invoker=${PKEXEC_UID:-${SUDO_UID:-$(id -u)}}
owner=$(stat -L -c %u "$BIN")
case $owner in
  0|"$invoker") ;;
  *) echo "$BIN is owned by uid $owner, not root or uid $invoker" >&2; exit 2 ;;
esac
case $(stat -L -c %A "$BIN") in
  ?????w*|????????w*)
    echo "$BIN is writable by group or others" >&2; exit 2 ;;
esac
if ! "$BIN" -h 2>&1 | grep -q -- '--crypt'; then
  echo "$BIN does not support crypt mode (rebuild without --enable-noconfig)" >&2
  exit 2
fi
echo "binary: $BIN ($("$BIN" -V 2>&1 | sed -n 1p))"

if [ "$(id -u)" -ne 0 ]; then
  echo "This test must run as root" >&2
  exit 77
fi

set -u

for tool in cryptsetup dmsetup losetup mkswap swapoff dd grep awk pgrep tr sed \
    setsid stat df; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required tool: $tool" >&2
    exit 77
  fi
done

if [ ! -e /dev/loop-control ]; then
  echo "No /dev/loop-control: loop device support is missing" >&2
  exit 77
fi

# A running swapspace that uses crypt mode owns mappings named swapspace-<n> and
# would fight over memory pressure with the daemons started below.  Crypt mode
# can come from the command line or from the configuration file; look at both.
for pid in $(pgrep -x swapspace 2>/dev/null); do
  args=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null)
  case $args in
    *crypt*|*" -E"*|*" -"[A-Za-z]*E*)
      echo "Refusing to run: swapspace (pid $pid) runs with crypt options: $args" >&2
      exit 77 ;;
  esac
  conf=$(echo "$args" | sed -n 's/.*\(-c\|--configfile\)[= ]\([^ ]*\).*/\2/p')
  for f in ${conf:-/etc/swapspace.conf /usr/local/etc/swapspace.conf}; do
    if [ -r "$f" ] && grep -Eq '^[[:space:]]*crypt([[:space:]]|=|$)' "$f"; then
      echo "Refusing to run: swapspace (pid $pid) uses crypt mode from $f" >&2
      exit 77
    fi
  done
done
unset pid args conf f

existing=$(dmsetup ls 2>/dev/null | awk '$1 ~ /^swapspace-[0-9]+$/ {print $1}')
if [ -n "$existing" ]; then
  echo "Refusing to run: mappings named swapspace-* already exist:" >&2
  echo "$existing" >&2
  exit 77
fi

parent=${1:-/var/tmp}
if [ ! -d "$parent" ]; then
  echo "Not a directory: $parent" >&2
  exit 2
fi
# Resolve symlinks and relative components: the checks below compare paths with
# what the kernel reports, and a relative or symlinked parent would make the
# "nothing left" checks pass without looking at anything.
parent=$(CDPATH= cd -- "$parent" && pwd -P) || exit 2
case $parent in
  /*) ;;
  *) echo "Cannot resolve $parent" >&2; exit 2 ;;
esac
case $parent in
  *[\"#]*) echo "A quote or # in $parent cannot go into the configuration" >&2
           exit 2 ;;
esac
case $(stat -f -c %T "$parent") in
  tmpfs|ramfs) echo "$parent is on tmpfs or ramfs; use a disk filesystem" >&2
               exit 2 ;;
esac
avail_kb=$(df -Pk "$parent" | awk 'NR == 2 {print $4}')
if [ "${avail_kb:-0}" -lt 614400 ]; then
  echo "Need 600 MB free in $parent, have ${avail_kb:-0} KB" >&2
  exit 2
fi

base=$(mktemp -d "$parent/swapspace-crypt-test.XXXXXX") || exit 1
swapdir="$base/swap"
mkdir "$swapdir" && chmod 700 "$swapdir" || exit 1
daemon_pid=""
other=""
failures=0

not() { ! "$@"; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }
skip() { echo "SKIP: $*"; }

# check DESCRIPTION COMMAND...
check() {
  desc=$1; shift
  if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi
}

# Slot numbers of all active encrypted swaps, one per line.
active_slots() {
  while read -r name rest; do
    case $name in
      /dev/mapper/swapspace-[0-9]*)
        echo "${name#/dev/mapper/swapspace-}" ;;
      /dev/dm-[0-9]*)
        node=${name#/dev/}
        if [ -r "/sys/block/$node/dm/name" ]; then
          mname=$(cat "/sys/block/$node/dm/name")
          case $mname in
            swapspace-[0-9]*) echo "${mname#swapspace-}" ;;
          esac
        fi ;;
    esac
  done </proc/swaps
}

count_slots() { active_slots | wc -l; }

mappers() { dmsetup ls 2>/dev/null | awk '$1 ~ /^swapspace-[0-9]+$/ {print $1}'; }

# Loop devices whose backing file lies under the test directory (including
# deleted files, which losetup still lists with their old path).
test_loops() {
  losetup -l -n -O NAME,BACK-FILE 2>/dev/null |
    awk -v p="$base/" 'index($2, p) == 1 {print $1}'
}

old_files() {
  for f in "$swapdir"/* ${other:+"$other"/*}; do
    case ${f##*/} in
      ''|*[!0-9]*) continue ;;
    esac
    [ -e "$f" ] && echo "$f"
  done
}

# Wait up to $1 seconds for the command in the remaining arguments to succeed.
wait_for() {
  limit=$1; shift
  while [ "$limit" -gt 0 ]; do
    if "$@" >/dev/null 2>&1; then return 0; fi
    sleep 1
    limit=$((limit - 1))
  done
  "$@" >/dev/null 2>&1
}

at_least_slots() { [ "$(count_slots)" -ge "$1" ]; }

no_slots() { [ -z "$(active_slots)" ]; }

no_numbered_files() { [ -z "$(old_files)" ]; }

no_plain_swaps() { ! grep -qF "$swapdir/" /proc/swaps; }

files_exist() {
  [ $# -gt 0 ] || return 1
  for s in "$@"; do [ -e "$swapdir/$s" ] || return 1; done
}

nothing_left() {
  [ -z "$(active_slots)" ] &&
  [ -z "$(mappers)" ] &&
  [ -z "$(test_loops)" ] &&
  no_plain_swaps
}

# Wait up to $1 seconds for the daemon to exit.
daemon_exits() {
  waited=0
  while kill -0 "$daemon_pid" 2>/dev/null && [ "$waited" -lt "$1" ]; do
    sleep 1
    waited=$((waited + 1))
  done
  ! kill -0 "$daemon_pid" 2>/dev/null
}

# The daemon runs in its own process group (see start_daemon), so a cryptsetup
# or mkswap it started, which survives SIGKILL of the daemon and can still
# create a mapping, is found by group.  Let those finish, then kill leftovers.
drain_group() {
  waited=0
  while pgrep -g "$daemon_pid" >/dev/null 2>&1 && [ "$waited" -lt 30 ]; do
    sleep 1
    waited=$((waited + 1))
  done
  if pgrep -g "$daemon_pid" >/dev/null 2>&1; then
    kill -KILL -- "-$daemon_pid" 2>/dev/null
    sleep 2
  fi
}

# Stop the daemon: SIGTERM, then SIGKILL after $1 seconds, then drain its group.
stop_daemon() {
  [ -n "$daemon_pid" ] || return 0
  note_mappers
  kill -TERM "$daemon_pid" 2>/dev/null
  if ! daemon_exits "${1:-60}"; then
    fail "daemon did not exit within ${1:-60} seconds of SIGTERM"
    kill -KILL "$daemon_pid" 2>/dev/null
  fi
  wait "$daemon_pid" 2>/dev/null
  daemon_status=$?
  drain_group
  note_mappers
  daemon_pid=""
}

# Names of the swapspace-* mappings whose loop device is backed by a file inside
# the test directory: the ones this test (through the daemon) created.
own_mappers() {
  for m in $(mappers); do
    dev=$(dmsetup deps -o devname "$m" 2>/dev/null |
      sed -n 's/.*(\(loop[0-9][0-9]*\)).*/\1/p')
    [ -n "$dev" ] || continue
    back=$(losetup -l -n -O NAME,BACK-FILE 2>/dev/null |
      awk -v d="/dev/$dev" '$1 == d {print $2}')
    case $back in
      "$base"/*) echo "$m" ;;
    esac
  done
}

# Record the names of the mappings that are ours right now, once per name.
# Cleanup acts on recorded names only.
created="$base/created-mappers"
: >"$created"
note_mappers() {
  for m in $(own_mappers); do
    grep -qx "$m" "$created" || echo "$m" >>"$created"
  done
}

# Remove everything this test may have created.  Runs on every exit path.
cleanup() {
  trap '' INT TERM HUP	# a second Ctrl-C must not stop cleanup half-way
  stop_daemon 10
  note_mappers
  for m in $(own_mappers); do
    # Never touch a mapping that was not recorded as ours.
    grep -qx "$m" "$created" || continue
    swapoff "/dev/mapper/$m" 2>/dev/null
    cryptsetup close "$m" 2>/dev/null
  done
  for l in $(test_loops); do losetup -d "$l" 2>/dev/null; done
  old_files | while IFS= read -r f; do swapoff "$f" 2>/dev/null; done
  case $base in
    "$parent"/swapspace-crypt-test.*) rm -rf "$base" ;;
  esac
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

write_conf() {
  # write_conf FILE [extra lines...]; WCDIR overrides the swap directory
  conf=$1; shift
  {
    cat <<EOF
swappath = "${WCDIR:-$swapdir}"
lower_freelimit = 99
freetarget = 99
upper_freelimit = 100
min_swapsize = 4m
max_swapsize = 8m
cooldown = 5
EOF
    for line in "$@"; do echo "$line"; done
  } >"$conf"
}

start_daemon() {
  # start_daemon CONF LOG
  setsid "$BIN" -c "$1" -v >"$2" 2>&1 &
  daemon_pid=$!
}

echo "kernel: $(uname -r); $(cryptsetup --version)"
echo "swap directory: $swapdir ($(stat -f -c %T "$parent"))"

write_conf "$base/crypt.conf" "crypt" "crypt_cipher = aes-xts-plain64" \
  "crypt_keysize = 512"
write_conf "$base/plain.conf"
write_conf "$base/paranoid.conf" "crypt" "paranoid"
write_conf "$base/badcipher.conf" "crypt" \
  "crypt_cipher = nosuchcipher-xts-plain64"

# === 1. allocation ==========================================================
echo "=== 1. allocation ==="
start_daemon "$base/crypt.conf" "$base/log1"
if wait_for 40 at_least_slots 2; then
  pass "encrypted swap appears in /proc/swaps (two slots)"
else
  fail "no two encrypted swaps after 40 seconds; log follows"
  tail -20 "$base/log1" >&2
fi
stop_daemon
check "daemon exits cleanly on SIGTERM" [ "$daemon_status" -eq 0 ]

slots=$(active_slots)
first=$(echo "$slots" | sed -n 1p)
second=$(echo "$slots" | sed -n 2p)

check "no plain swapfile is listed in /proc/swaps" no_plain_swaps
check "files exist for each slot" files_exist $slots

if [ -n "$first" ]; then
  check "dmsetup table shows the cipher" \
    sh -c "dmsetup table swapspace-$first | grep -q 'crypt aes-xts-plain64 '"
  status=$(cryptsetup status "swapspace-$first" 2>&1)
  echo "$status" | grep -Eq 'type: +PLAIN' && pass "cryptsetup status: type PLAIN" ||
    fail "cryptsetup status: type PLAIN"
  echo "$status" | grep -Eq 'cipher: +aes-xts-plain64' &&
    pass "cryptsetup status: cipher" || fail "cryptsetup status: cipher"
  echo "$status" | grep -Eq 'keysize: +512' &&
    pass "cryptsetup status: key size" || fail "cryptsetup status: key size"
  loopdev=$(test_loops | sed -n 1p)
  if [ -n "$loopdev" ]; then
    dio=$(losetup -l -n -O NAME,DIO | awk -v d="$loopdev" '$1 == d {print $2}')
    [ "$dio" = "1" ] && pass "loop device uses direct I/O" ||
      fail "loop device does not use direct I/O (DIO=$dio)"
    logsec=$(losetup -l -n -O NAME,LOG-SEC 2>/dev/null |
      awk -v d="$loopdev" '$1 == d {print $2}')
    echo "info: loop logical block size ${logsec:-unknown}, filesystem block size $(stat -f -c %S "$swapdir")"
    [ -z "$logsec" ] || [ "$logsec" -ge 512 ] &&
      pass "loop logical block size is usable" ||
      fail "loop logical block size is $logsec"
  else
    fail "no loop device backed by a file in the test directory"
  fi
fi

if [ -n "$first" ] && [ -n "$second" ]; then
  k1=$(dmsetup table --showkeys "swapspace-$first" | awk '{print $5}')
  k2=$(dmsetup table --showkeys "swapspace-$second" | awk '{print $5}')
  if echo "$k1" | grep -Eq '^[0-9a-f]{128}$' &&
     echo "$k2" | grep -Eq '^[0-9a-f]{128}$'; then
    zeros=$(printf '0%.0s' $(seq 1 128))
    [ "$k1" != "$k2" ] && pass "each file has its own key" ||
      fail "two files share a key"
    [ "$k1" != "$zeros" ] && [ "$k2" != "$zeros" ] && pass "keys are not zero" ||
      fail "a key is all zeros"
  else
    skip "key comparison: key is not shown as hex by dmsetup here"
  fi
  unset k1 k2
fi

# === 2. adoption ============================================================
echo "=== 2. adoption ==="
minor_before=""
[ -n "$first" ] && minor_before=$(dmsetup info -c --noheadings -o minor "swapspace-$first")
start_daemon "$base/crypt.conf" "$base/log2"
sleep 3
stop_daemon
check "restarted daemon detects the encrypted swapfile" \
  grep -q "Detected swapfile '$first'" "$base/log2"
check "the active mapping was not recreated" \
  sh -c "[ \"\$(dmsetup info -c --noheadings -o minor swapspace-$first)\" = '$minor_before' ]"
check "no plain swapfile is listed in /proc/swaps after restart" no_plain_swaps

# === 3. confidentiality and stale mapping ===================================
echo "=== 3. confidentiality and stale mapping ==="
if [ -n "$first" ]; then
  marker="SWAPSPACE-CRYPT-MARKER-$$-$(date +%s)"
  # A 1 MiB pattern of the marker, written at offset 1 MiB of the device.
  yes "$marker" | head -c 1048576 >"$base/pattern"
  check "marker is findable in a plain file (control)" \
    grep -aqF "$marker" "$base/pattern"

  # Make the mapping a plain block device again: writing into active swap
  # would corrupt swapped-out pages.
  if swapoff "/dev/mapper/swapspace-$first"; then
    dd if="$base/pattern" of="/dev/mapper/swapspace-$first" bs=4096 seek=256 \
      count=256 oflag=direct 2>/dev/null
    sync
    dd if="/dev/mapper/swapspace-$first" bs=4096 skip=256 count=256 \
      iflag=direct 2>/dev/null | grep -aqF "$marker" &&
      pass "marker reads back through the mapping" ||
      fail "marker did not reach the mapping"
    grep -aqF "$marker" "$swapdir/$first" &&
      fail "marker found in the backing file: not encrypted" ||
      pass "marker is absent from the backing file"
  else
    fail "could not swapoff slot $first for the marker test"
  fi

  # Startup with crypt on: the mapping of slot $first is stale (no swap on
  # it).  It must be removed and its file deleted; the rest is retired by -e.
  "$BIN" -c "$base/crypt.conf" -e -v >"$base/log3" 2>&1
  check "-e with crypt on succeeds" [ $? -eq 0 ]
  check "stale mapping removed" sh -c "! dmsetup info swapspace-$first"
  check "stale file deleted" not files_exist "$first"
  wait_for 10 nothing_left
  check "nothing left behind after -e" nothing_left
fi

# === 4. erase with crypt off ================================================
echo "=== 4. erase with crypt off ==="
start_daemon "$base/crypt.conf" "$base/log4"
wait_for 40 at_least_slots 1
stop_daemon
check "an encrypted swap is active before -e" at_least_slots 1
"$BIN" -c "$base/plain.conf" -e -v >"$base/log4e" 2>&1
check "-e with crypt off succeeds" [ $? -eq 0 ]
wait_for 10 nothing_left
check "-e retired the encrypted swapfiles and left nothing" nothing_left
check "no numbered files remain" no_numbered_files

# === 5. paranoid ============================================================
echo "=== 5. paranoid ==="
start_daemon "$base/paranoid.conf" "$base/log5"
wait_for 40 at_least_slots 1
stop_daemon
check "paranoid daemon exits with success" [ "$daemon_status" -eq 0 ]
wait_for 10 nothing_left
check "paranoid exit retired everything" nothing_left
check "no numbered files remain" no_numbered_files

# === 6. fail closed =========================================================
echo "=== 6. fail closed ==="
start_daemon "$base/badcipher.conf" "$base/log6"
sleep 8
check "daemon tried and logged the failure" \
  grep -q "Could not create encrypted mapping" "$base/log6"
check "no swap was enabled" no_slots
check "no plain swapfile was enabled" no_plain_swaps
stop_daemon
wait_for 10 nothing_left
check "nothing left behind after failed setups" nothing_left
check "no numbered files remain" no_numbered_files

# === 7. another instance ====================================================
echo "=== 7. another instance ==="
other="$base/other"
mkdir "$other" && chmod 700 "$other" || exit 1
WCDIR=$other write_conf "$base/other.conf" "crypt"
start_daemon "$base/crypt.conf" "$base/log7a"
wait_for 40 at_least_slots 1
stop_daemon
mine=$(active_slots | sed -n 1p)
if [ -n "$mine" ]; then
  # The other instance has a file with the same number, as an earlier run of
  # its own might have left.  Mapping names are global; this one is not its.
  dd if=/dev/zero of="$other/$mine" bs=1k count=64 2>/dev/null
  start_daemon "$base/other.conf" "$base/log7b"
  sleep 6
  stop_daemon
  "$BIN" -c "$base/other.conf" -e -v >"$base/log7c" 2>&1
  check "other instance did not adopt the mapping" \
    not grep -q "Detected swapfile '$mine'" "$base/log7b" "$base/log7c"
  check "the first instance's mapping is still active" \
    dmsetup info "swapspace-$mine"
  check "the first instance's swap is still in /proc/swaps" \
    at_least_slots 1
  check "the first instance's file is still there" [ -e "$swapdir/$mine" ]
  check "the other instance's own file is still there" [ -e "$other/$mine" ]

  # A plain (crypt off) instance on a directory with a file of that number must
  # not close the mapping either: at startup it clears leftovers only if they
  # sit on its own files.
  WCDIR=$other write_conf "$base/otherplain.conf"
  dd if=/dev/zero of="$other/$mine" bs=1k count=64 2>/dev/null
  start_daemon "$base/otherplain.conf" "$base/log7e"
  sleep 6
  stop_daemon
  "$BIN" -c "$base/otherplain.conf" -e -v >"$base/log7f" 2>&1
  check "a plain instance left the first instance's mapping alone" \
    dmsetup info "swapspace-$mine"
  check "the first instance's swap is still active after the plain run" \
    at_least_slots 1
else
  fail "no encrypted swap to test with"
fi
"$BIN" -c "$base/crypt.conf" -e -v >"$base/log7d" 2>&1
wait_for 10 nothing_left
check "nothing left behind after scenario 7" nothing_left

echo
if [ "$failures" -eq 0 ]; then
  echo "ALL PASSED"
  exit 0
fi
echo "$failures check(s) failed; logs are in $base until this script exits" >&2
exit 1
