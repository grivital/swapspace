#!/usr/bin/env bash
# Install a built swapspace package with the native tool, check what it put on
# the system, remove it and check what it left behind. The service is never
# started: containers have no systemd and swapspace must not run.
#
#   packaging/smoke.sh <arch|deb|rpm> <package-file>
#
# This installs and removes packages on the machine it runs on, so it only runs
# in a container unless SWAPSPACE_SMOKE_ALLOW_HOST=1 is set.
set -euo pipefail

die() { echo "smoke.sh: $*" >&2; exit 1; }

(( $# == 2 )) || die "usage: ${0##*/} <arch|deb|rpm> <package-file>"
target=$1
[[ -f $2 ]] || die "no such package file: $2"
pkg=$(realpath "$2")
case "$target" in arch|deb|rpm) ;; *) die "unknown target: $target" ;; esac
(( EUID == 0 )) || die "must run as root"
if [[ ! -e /.dockerenv && ! -e /run/.containerenv &&
      ${SWAPSPACE_SMOKE_ALLOW_HOST:-} != 1 ]]; then
  die "refusing to install packages outside a container"
fi

unit=/usr/lib/systemd/system/swapspace.service
tmpfiles=/usr/lib/tmpfiles.d/swapspace.conf
conf=/etc/swapspace.conf
statedir=/var/lib/swapspace

pm_install() {
  case "$target" in
    arch) pacman -U --noconfirm "$pkg" ;;
    deb)
      apt-get update
      DEBIAN_FRONTEND=noninteractive apt-get install -y "$pkg"
      ;;
    rpm) dnf install -y "$pkg" ;;
  esac
}

pm_remove() {
  case "$target" in
    arch) pacman -R --noconfirm swapspace ;;
    deb) DEBIAN_FRONTEND=noninteractive apt-get remove -y swapspace ;;
    rpm) dnf remove -y swapspace ;;
  esac
}

pm_files() {
  case "$target" in
    arch) pacman -Qlq swapspace ;;
    deb) dpkg -L swapspace ;;
    rpm) rpm -ql swapspace ;;
  esac
}

# Contents of the package file itself: container images often exclude docs
# and man pages at install time, so those are checked here, not on disk.
pkg_files() {
  case "$target" in
    arch) pacman -Qlpq "$pkg" ;;
    deb) dpkg-deb -c "$pkg" | sed 's/ -> .*//' | awk '{print $NF}' ;;
    rpm) rpm -qlp "$pkg" ;;
  esac
}

# Is the config file registered with the package manager as user-editable?
conf_is_marked() {
  local out
  case "$target" in
    arch)
      out=$(pacman -Qii swapspace)
      sed -n '/^Backup Files/,$p' <<<"$out" | grep -q "$conf"
      ;;
    deb)
      out=$(dpkg-query -W -f='${Conffiles}\n' swapspace)
      grep -q "$conf" <<<"$out"
      ;;
    rpm)
      local flags
      flags=$(rpm -q --qf '[%{FILEFLAGS:fflags} %{FILENAMES}\n]' swapspace |
        sed -n "s| $conf\$||p")
      [[ $flags == *c* && $flags == *n* ]]
      ;;
  esac
}

fail=0
check() {
  local desc=$1
  shift
  if "$@" >/dev/null 2>&1; then
    echo "PASS: $desc"
  else
    echo "FAIL: $desc"
    fail=1
  fi
}

exec_path() { sed -n 's/^ExecStart=\([^ ]*\).*/\1/p' "$unit"; }
exec_is_executable() { local p; p=$(exec_path); [[ -n $p && -x $p ]]; }
inspect_config() {
  local p
  p=$(exec_path)
  "$p" -i -c "$conf"
}
state_dir_is_private() { [[ $(stat -c %a "$statedir") == 700 ]]; }
tmpfiles_has_state_dir() {
  grep -Eq "^d +$statedir +0?700 +root +root " "$tmpfiles"
}
# Capture first: grep -q closing the pipe early would trip pipefail.
ships() {
  local f
  f=$(pkg_files)
  grep -Eq "$1" <<<"$f"
}
ships_man_page() { ships '/usr/share/man/man8/swapspace\.8(\.[a-z0-9]+)?$'; }
ships_news() { ships '/NEWS(\.gz)?$'; }
ships_license() {
  ships '(licenses/swapspace/COPYING|doc/swapspace/copyright)$'
}
# An enable link shipped in the payload would enable it on install.
payload_has_no_enable_link() {
  ! ships '\.(wants|requires)/swapspace\.service'
}
# A symlink named swapspace.service is an enable (or mask) link.
not_enabled() {
  [[ -z $(find /etc/systemd /usr/lib/systemd /lib/systemd \
    -name swapspace.service -type l 2>/dev/null) ]]
}

pm_install
check "unit file exists ($unit)" test -f "$unit"
check "ExecStart target exists and is executable" exec_is_executable
check "swapspace -i accepts $conf" inspect_config
check "$conf is marked as a config file" conf_is_marked
check "tmpfiles entry creates $statedir mode 0700" tmpfiles_has_state_dir
check "$statedir ships with mode 0700" state_dir_is_private
check "package ships the man page" ships_man_page
check "package ships NEWS" ships_news
check "package ships the license" ships_license
check "package ships no enable link" payload_has_no_enable_link
check "service is not enabled" not_enabled

files=$(pm_files)
pm_remove

leftovers=()
while IFS= read -r f; do
  [[ $f == "$conf" ]] && continue
  if [[ -L $f || ( -e $f && ! -d $f ) ]]; then leftovers+=("$f"); fi
done <<<"$files"
if (( ${#leftovers[@]} )); then
  echo "leftover files: ${leftovers[*]}"
fi
check "no package files left after removal" test "${#leftovers[@]}" -eq 0
check "$statedir removed" test ! -e "$statedir"

if (( fail )); then echo "smoke: FAIL"; exit 1; fi
echo "smoke: PASS"
