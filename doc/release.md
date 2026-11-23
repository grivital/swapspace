# Swapspace release process
1. TEST! See below.
2. Update version number in NEWS with new features and changes.
3. Commit just the NEWS file and the configure.ac version bump as "Version x.xx"
4. Tag the previous commit as "vx.xx" and sign it using: `git tag -s vx.xx`
	Include the changelog entries in the tag message

## Swapspace test scenarios

This list contains all the tests that should be run before a release
of Swapspace, to avoid situations like the 1.15-1.16 release snafu.

Make sure to bump the version number and `make distcheck`,
then move that tarball to the test machine.

### Scenarios
1. `hog` should be used to trigger swapspace to eventually create a swapfile.

Eventually, I would like to test more scenarios with more complexity,
but this would all need to be automated first.

Make sure to run `swapspace -e` between tests!

### Filesystems
Use `blkid` to check which partition is which.

1. btrfs (first thing to test, since it requires more complex support)
	1. Set compression flag set on the swapfile directory
	2. Unset NOCOW flag on the swapfile directory
2. ext4
3. xfs
4. f2fs

### Automated root test

`tests/root-integration.sh` runs the create/activate/retire cycle on real
filesystems.  It is destructive and refuses to run unless
`SWAPSPACE_VM_TEST=1` is set, so use it only inside a disposable VM:

    SWAPSPACE_VM_TEST=1 ./tests/root-integration.sh /mnt/ext4 /mnt/xfs ...

Pass one directory per mounted test filesystem.

### Crypt mode root test

`tests/crypt-root.sh` covers encrypted swapfiles (loop device and dm-crypt).  It
activates real swap and creates mappings named `swapspace-N`, so it refuses to
run unless it is root and `SWAPSPACE_CRYPT_TEST=1` is set:

    SWAPSPACE_CRYPT_TEST=1 ./tests/crypt-root.sh [directory-on-test-fs]

From a desktop session, `pkexec` starts it as root.  It strips the environment,
so pass the variables through `env`, before the script path, and use absolute
paths:

    pkexec env SWAPSPACE_CRYPT_TEST=1 BIN=/abs/path/to/src/swapspace /abs/path/to/tests/crypt-root.sh /var/tmp

`BIN` is the binary under test, an absolute path to a regular executable owned
by root or the invoking user and not writable by group or others, which must
support crypt mode.  Without it the script uses `src/swapspace` next to itself,
never one found through the current directory.  The directory must not be on
tmpfs and needs 600 MB free.  Cleanup cannot be interrupted; the script header
says what to check by hand after a SIGKILL.

The script refuses to start while a `swapspace-*` mapping exists or a swapspace
process with crypt options (command line or configuration file) is running, and
only swaps off, closes or detaches what it created itself.

The directory (default `/var/tmp`) must be on a filesystem and device where
direct I/O works with the loop device's block size (swapspace takes the block
size from the file's `STATX_DIOALIGN`, so 4Kn storage is covered).  It also
checks that a second instance, in crypt mode or with crypt off, neither adopts
nor closes the first one's mapping.  Run it
with the exact command above after any change to `src/cryptdev.c` or
`src/swaps.c`; the unprivileged tests (`make check`) fake the loop and
dm-crypt steps and cannot prove that the kernel accepts them.
