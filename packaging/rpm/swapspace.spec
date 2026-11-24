# Built by packaging/build.sh, which passes pkg_version, snap_count, snap_sha
# and snap_date and prepares the source tarball from git.
%{!?pkg_version:%{error:run packaging/build.sh rpm instead of rpmbuild}}
%global srcver %{pkg_version}+git%{snap_count}.g%{snap_sha}
%global debug_package %{nil}

Name:           swapspace
Version:        %{pkg_version}^git%{snap_count}.g%{snap_sha}
Release:        1%{?dist}
Summary:        Dynamic swap space manager
License:        GPL-2.0-or-later
URL:            https://github.com/Tookmund/swapspace
Source0:        %{name}-%{srcver}.tar.gz
Source1:        swapspace.tmpfiles

BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  systemd-rpm-macros
%{?systemd_requires}

%description
Swapspace watches memory usage and creates swap files in a dedicated
directory when the system needs more virtual memory, then removes them again
once the pressure is gone. The systemd unit is installed but neither enabled
nor started.

%prep
%autosetup -n %{name}-%{srcver}

%build
autoreconf -fi
%configure
%make_build

%install
%make_install
# "make install" skips the unit and the man page, and upstream's unit points
# at /usr/local/sbin.
sed 's|/usr/local/sbin/swapspace|%{_sbindir}/swapspace|' swapspace.service |
    install -Dpm644 /dev/stdin %{buildroot}%{_unitdir}/swapspace.service
install -Dpm644 %{SOURCE1} %{buildroot}%{_tmpfilesdir}/swapspace.conf
install -Dpm644 doc/swapspace.8 %{buildroot}%{_mandir}/man8/swapspace.8

%check
make check

%post
%systemd_post swapspace.service

%preun
%systemd_preun swapspace.service

%postun
%systemd_postun_with_restart swapspace.service

%files
%license COPYING
%doc README README.md NEWS ChangeLog AUTHORS
%config(noreplace) %{_sysconfdir}/swapspace.conf
%{_sbindir}/swapspace
%{_unitdir}/swapspace.service
%{_tmpfilesdir}/swapspace.conf
%{_mandir}/man8/swapspace.8*
%dir %attr(0700,root,root) %{_localstatedir}/lib/swapspace

%changelog
* %{snap_date} Cristian Magherusan-Stanciu <95209+cristim@users.noreply.github.com> - %{version}-%{release}
- Snapshot build of commit %{snap_sha}
