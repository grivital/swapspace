#!/bin/sh
# Destructive integration test against real swapfiles.
#
# RUN ONLY IN A DISPOSABLE VM.  This script creates and activates real
# swapfiles on the filesystems you name, then retires them.  Set
# SWAPSPACE_VM_TEST=1 to confirm that you are inside such a VM.
#
# Usage (from the build tree):
#   SWAPSPACE_VM_TEST=1 ./tests/root-integration.sh /mnt/ext4 /mnt/xfs ...
#
# Each argument must be a directory on a mounted test filesystem.  A swap
# subdirectory is created inside it and removed afterwards.

set -eu

if [ "${SWAPSPACE_VM_TEST:-0}" != "1" ]; then
  echo "Refusing to run: set SWAPSPACE_VM_TEST=1 inside a disposable VM" >&2
  echo "(this test creates real swapfiles)" >&2
  exit 77
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "This test creates real swapfiles and must run as root" >&2
  exit 77
fi

if [ "$#" -eq 0 ]; then
  echo "Usage: SWAPSPACE_VM_TEST=1 $0 <directory-on-test-fs>..." >&2
  exit 2
fi

scriptdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN="${BIN:-$(pwd)/../src/swapspace}"
if [ ! -x "$BIN" ] && [ -x "$scriptdir/../src/swapspace" ]; then
  BIN="$scriptdir/../src/swapspace"
fi
if [ ! -x "$BIN" ]; then
  echo "Cannot find $BIN; build swapspace first or set BIN" >&2
  exit 1
fi

fail=0
for parent in "$@"; do
  if [ ! -d "$parent" ]; then
    echo "SKIP $parent: not a directory" >&2
    continue
  fi

  fstype=$(stat -f -c %T "$parent" 2>/dev/null || echo unknown)
  dir=$(mktemp -d "$parent/swapspace-test.XXXXXX")
  conf="$dir/conf"
  log="$dir/log"

  cat >"$conf" <<EOF
swappath = "$dir"
lower_freelimit = 99
freetarget = 99
upper_freelimit = 100
min_swapsize = 4m
max_swapsize = 16m
cooldown = 5
paranoid
EOF

  echo "=== $parent ($fstype) ==="

  # Show what is active while swapspace is running.
  ( sleep 3; grep -F "$dir" /proc/swaps || true ) &
  checker=$!

  # The daemon retires everything on SIGTERM because of --paranoid.
  timeout -s TERM 8 "$BIN" -c "$conf" -v >"$log" 2>&1 || true
  wait "$checker" || true

  if grep -q "Allocating swapfile" "$log" &&
     grep -q "Retiring swapfile" "$log" &&
     ! grep -qF "$dir" /proc/swaps; then
    echo "PASS $fstype: allocated and retired real swapfiles"
  else
    echo "FAIL $fstype: see $log" >&2
    tail -20 "$log" >&2
    fail=1
    # Best-effort cleanup of anything that is still active.
    for f in "$dir"/*; do
      case "$(basename "$f")" in
        ''|*[!0-9]*) continue ;;
      esac
      swapoff "$f" 2>/dev/null || true
    done
  fi

  rm -rf "$dir"
done

exit "$fail"
