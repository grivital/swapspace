#!/usr/bin/env bash
# Build a swapspace package for one distro family from the current checkout.
# The same script runs in CI (inside a distro container) and on a dev machine.
#
#   packaging/build.sh <arch|deb|rpm> [outdir] [--install-deps]
#
# The package is built from `git archive HEAD`, so commit first; uncommitted
# changes outside packaging/ are not included. Any ref works, not only tags.
set -euo pipefail
shopt -s nullglob

die() { echo "build.sh: $*" >&2; exit 1; }

usage() {
  echo "usage: ${0##*/} <arch|deb|rpm> [outdir] [--install-deps]" >&2
  exit 2
}

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
[[ $(git -C "$repo" rev-parse --is-shallow-repository) == false ]] ||
  die "shallow clone gives a wrong version: run git fetch --unshallow"

target=""
outdir=""
install_deps=0
for arg in "$@"; do
  case "$arg" in
    --install-deps) install_deps=1 ;;
    -h|--help) usage ;;
    -*) die "unknown option: $arg" ;;
    *)
      if [[ -z $target ]]; then target=$arg
      elif [[ -z $outdir ]]; then outdir=$arg
      else usage
      fi
      ;;
  esac
done
case "$target" in arch|deb|rpm) ;; *) usage ;; esac
outdir=${outdir:-./packages}

as_root() {
  if (( EUID == 0 )); then "$@"; else sudo "$@"; fi
}

# Tools each target needs; with --install-deps they are installed first.
case "$target" in
  arch) tools=(git gzip makepkg autoconf automake gcc make fakeroot) ;;
  deb) tools=(git gzip dpkg-buildpackage dh autoconf automake gcc make) ;;
  rpm) tools=(git gzip rpmbuild autoconf automake gcc make) ;;
esac

install_build_deps() {
  case "$target" in
    arch)
      as_root pacman -Syu --noconfirm --needed base-devel git
      ;;
    deb)
      as_root env DEBIAN_FRONTEND=noninteractive apt-get update
      as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y \
        --no-install-recommends build-essential debhelper dpkg-dev \
        autoconf automake git ca-certificates
      ;;
    rpm)
      as_root dnf install -y rpm-build autoconf automake gcc make \
        systemd-rpm-macros git-core gzip tar
      ;;
  esac
}

in_container() {
  [[ -e /.dockerenv || -e /run/.containerenv || -n ${CI:-} ||
    -n ${GITHUB_ACTIONS:-} ]]
}

find_missing() {
  missing=()
  local t
  for t in "${tools[@]}"; do command -v "$t" >/dev/null || missing+=("$t"); done
}

find_missing
if (( install_deps && ${#missing[@]} )); then
  # arch runs a full system upgrade, so never on a developer machine.
  in_container ||
    die "--install-deps is for containers and CI only; install: ${missing[*]}"
  install_build_deps
  find_missing
fi
if (( ${#missing[@]} )); then
  die "missing build tools: ${missing[*]} (--install-deps works in containers)"
fi

# Version: upstream base from HEAD's configure.ac plus the commit position.
base=$(git -C "$repo" show HEAD:configure.ac |
  sed -n 's/^AC_INIT(\[swapspace\], *\[\([0-9][0-9.]*\)\].*/\1/p')
[[ -n $base ]] || die "cannot read the version from configure.ac"
count=$(git -C "$repo" rev-list --count HEAD)
sha=$(git -C "$repo" rev-parse --short=7 HEAD)
commit_time=$(git -C "$repo" show -s --format=%ct HEAD)
# pacman versions cannot contain hyphens; this form is also valid for rpm.
srcver="${base}+git${count}.g${sha}"

if [[ -n $(git -C "$repo" status --porcelain --untracked-files=no) ]]; then
  echo "build.sh: warning: uncommitted changes are not in the package" >&2
fi

mkdir -p "$outdir"
outdir=$(cd "$outdir" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/swapspace-pkg.XXXXXX")
builder=""
cleanup() {
  if [[ -n $builder ]]; then userdel "$builder" 2>/dev/null || true; fi
  rm -rf "$work"
}
trap cleanup EXIT

tarball="$work/swapspace-$srcver.tar.gz"
git -C "$repo" archive --prefix="swapspace-$srcver/" HEAD | gzip -n > "$tarball"

publish() {
  (( $# )) || die "no package was produced"
  cp -- "$@" "$outdir/"
  local f
  for f in "$@"; do echo "$outdir/${f##*/}"; done
}

build_arch() {
  local dir="$work/arch"
  mkdir -p "$dir/out" "$dir/home"
  cp "$here/arch/PKGBUILD" "$here/swapspace.tmpfiles" "$tarball" "$dir/"

  local run=()
  if (( EUID == 0 )); then
    # makepkg refuses to run as root.
    useradd --system --no-create-home --home-dir "$dir/home" swapspace-builder
    # Set only after useradd succeeded, so the exit trap never deletes a user
    # this run did not create.
    builder=swapspace-builder
    chmod 755 "$work"
    chown -R "$builder" "$dir"
    run=(runuser -u "$builder" --)
  fi
  (
    cd "$dir"
    "${run[@]}" env HOME="$dir/home" SWAPSPACE_PKGVER="$srcver" \
      PKGDEST="$dir/out" makepkg --noconfirm --force
  )
  publish "$dir"/out/swapspace-*.pkg.tar.*
}

build_deb() {
  local dir="$work/deb" upstream="${base}+git${count}.${sha}"
  local src="$dir/swapspace-$srcver"
  mkdir -p "$dir"
  tar -xzf "$tarball" -C "$dir"
  cp "$tarball" "$dir/swapspace_$upstream.orig.tar.gz"
  cp -a "$here/debian" "$src/debian"
  cp "$here/swapspace.tmpfiles" "$src/debian/swapspace-tmpfiles.conf"
  sed -e "s|@VERSION@|$upstream-1|" -e "s|@SHA@|$sha|" \
    -e "s|@DATE@|$(date -R -u -d "@$commit_time")|" \
    "$here/debian/changelog.tmpl" > "$src/debian/changelog"
  rm "$src/debian/changelog.tmpl"
  (cd "$src" && dpkg-buildpackage -us -uc -b)
  publish "$dir"/swapspace_*.deb
}
build_rpm() {
  local top="$work/rpm"
  mkdir -p "$top"/{SOURCES,SPECS,BUILD,RPMS,SRPMS,BUILDROOT}
  cp "$tarball" "$here/swapspace.tmpfiles" "$top/SOURCES/"
  cp "$here/rpm/swapspace.spec" "$top/SPECS/"
  rpmbuild -bb "$top/SPECS/swapspace.spec" \
    --define "_topdir $top" \
    --define "pkg_version $base" \
    --define "snap_count $count" \
    --define "snap_sha $sha" \
    --define "snap_date $(LC_ALL=C date -u -d "@$commit_time" '+%a %b %d %Y')"
  publish "$top"/RPMS/*/swapspace-*.rpm
}

"build_$target"
