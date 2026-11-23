
Technical details
=================

Algorithm
---------

Choices of allocation and deallocation are driven by a "finite state machine"
consisting of four states: steady, hungry, overfed, and diet.  The program will
move between these states as circumstances dictate, and apply different policies
depending on the current state.  This is how swapspace manages its tradeoffs
between the desire to free up unused swap space on the one hand, and on the
other, the desire to minimize deallocation and re-allocation (or "thrashing") of
swap files.

![finite state machine](fsm.png)

For those interested, here is a quick description of these states and their
associated policies:

Steady - normal operation; no additional swap space is needed, nor do we have
more than is needed.  However, the program can go into the hungry or overfed
states depending on memory usage.

Hungry - more swap space was recently allocated.  The program is willing to
allocate even more if needed, but to avoid thrashing, it's not going to drop
swap files.  If the system does not need more swap files for a certain period of
time, the program reverts to the steady state.

Overfed - significantly more virtual memory is available than is needed.  If
this situation persists for a certain timeout period, the program deallocates
one of its swap files and returns to the steady state.  If there's still too
much swap space, the program may come straight back to this state.

Diet - a recent allocation attempt has run into resource limits, e.g. because
the filesystem used for swap files was full.  No more swapspace can be allocated
but excess files can be freed up very rapidly.  Afer timeout, reverts to steady.

State information can be queried by sending the program the SIGUSR1 signal (see
"man 7 signal" to get the number for your architecture) which will cause it to
log debug information to the system's daemon log and/or standard output, as
appropriate.  The program can also be made to log state transitions by starting
it with the -v or --verbose option.

External signals
----------------

Two further inputs refine the policy when the kernel provides them.  Pressure
Stall Information (`/proc/pressure/memory`, Linux 4.20 and later) tells us
whether tasks are actually stalling.  When the "some avg10" value exceeds
`psi_threshold` (10% by default, 0 disables), swapspace steers towards
`freetarget` even before `lower_freelimit` is reached, and it does not
deallocate swap while pressure persists.

In containers, `/proc/meminfo` describes the host rather than the container.
When the process's cgroup exposes a numeric memory limit (v2 `memory.max`, or
v1 `memory.limit_in_bytes`), that limit replaces `MemTotal`, free space is
derived from the cgroup's current usage, and the cgroup's own swap accounting
is used where available.  The percentage thresholds then apply to the
cgroup's budget instead of the host's.

Encrypted swapfiles
-------------------

The optional `crypt` mode keeps the swap data in the swapfiles encrypted with a
random key that lives only in the kernel.  Without it, none of this applies and
the code paths are the plain ones described above.

Setup of swapfile N (`alloc_swapfile()` and `crypt_enable()`):

1. The file is created with `NOCOW` through the directory attribute.  On btrfs
   the new file must really have `FS_NOCOW_FL` (checked with `FS_IOC_GETFLAGS`
   on the open file); if it does not, crypt mode refuses to create it and logs
   why, because a copy-on-write file with data checksums gets its direct writes
   bounced through freshly allocated pages, which is exactly what must not
   happen while swapping out.  Even with `NOCOW`, btrfs may still bounce direct
   writes when the data profile is DUP or RAID1 (mirrored data needs a stable
   copy); that is not checked, so use the `single` data profile for the swap
   directory's filesystem, or another filesystem.  The file is not
   preallocated: `posix_fallocate()` leaves unwritten extents, and the first
   direct write to each one needs a metadata update (allocation, extent
   conversion, a journal transaction) on the swap-out path.  Instead the file
   is filled with zeros and flushed (`fsync` also forces delayed allocation), so
   only allocated blocks are overwritten later.
2. `crypt_clear_stale(N, file)` removes a leftover mapping `swapspace-N` if
   there is one and it is ours, that is, its loop device is backed by our file N
   (same device and inode; N must itself be a regular file with one link, not a
   symlink or a second name for another instance's file).  If it cannot be
   removed, the slot is skipped for now.  Plain mode runs the same check before
   it creates file N, so it never unlinks the file under a mapping left by an
   earlier crypt run.  A mapping of that name that is not backed by our file (another
   instance's) is never touched: the slot number is skipped and the next free
   one is used.  A dm device without its `/dev/mapper` node counts as existing
   (the dm names in sysfs are consulted), so it is removed too instead of making
   `cryptsetup open` fail forever.
3. The file is attached to a loop device through `LOOP_CTL_GET_FREE` and one
   `LOOP_CONFIGURE` call with `LO_FLAGS_AUTOCLEAR | LO_FLAGS_DIRECT_IO` and a
   block size taken from the open file's `STATX_DIOALIGN` (else its preferred
   I/O size, capped at the page size), rounded up to a power of two between 512
   and the page size.  The
   block size matters: a loop device starts with 512-byte blocks, and since
   current mainline `loop.c` (`lo_can_use_dio()`) refuses direct I/O when the loop's
   logical block size is below the backing file's direct I/O alignment, so on 4Kn
   storage a later `LOOP_SET_DIRECT_IO` fails with `EINVAL`.  The kernel also
   drops the flag silently, so the result is read back with
   `LOOP_GET_STATUS64`; if direct I/O is not in effect, setup fails with a
   message that says so.  Direct I/O keeps swap writes out of the backing file's
   page cache, the part of loop-backed swap that is most likely to deadlock when
   memory is short; it does not by itself make the write allocation-free (see
   the btrfs note in step 1).  A logical block size below the page size is fine
   for swap: `mm/swapfile.c` rejects only zoned devices, and kernels that called
   `set_blocksize(PAGE_SIZE)` needed the block size to be at most the page size,
   which the cap guarantees (this matters with 16 KiB pages).  Mainline allows
   loop block sizes up to 64 KiB, but swap does its I/O in pages and cannot use
   a device with larger logical blocks, so nothing above the page size is
   requested.  If a filesystem reports a direct I/O alignment above the page
   size, setup is refused before the loop device is configured: with
   `block_size` 0 and direct I/O requested, mainline `loop.c` would take the
   alignment as the logical block size and keep direct I/O, giving a block size
   above the page size.  No shell is involved.
4. `cryptsetup open --type plain --cipher C --key-size BITS --key-file
   /dev/urandom --keyfile-size BYTES --batch-mode /dev/loopM swapspace-N`.  With
   a key file and no `--hash`, plain mode uses exactly `--key-size` bits of the
   file as the key (no hashing) and ignores `--keyfile-size` (see
   `action_open_plain()` in cryptsetup's `src/cryptsetup.c`); the flag is kept
   so the command states what is meant.  The cipher string is whitelisted
   (`[A-Za-z0-9:_.-]`, alphanumeric first) and single-quoted; the only other
   substituted values are integers.
5. `mkswap` and `swapon` on `/dev/mapper/swapspace-N`.

The setup fails closed.  A failure in any step undoes the earlier ones in
reverse order: the mapping is removed with `cryptsetup close`, our reference on
the loop device is dropped (autoclear detaches it), and the file is deleted.
Unlike the plain path, a failing `mkswap` is an error.  Nothing is ever
activated from the plain file.  If the mapping cannot be removed (`CRYPT_HELD`),
it still holds the file through its loop device, so the file is kept and the
leftover is cleaned when the slot is next used or at the next startup.

Retirement (`crypt_retire()`): `swapoff` the mapper device, `cryptsetup close
swapspace-N`, then unlink the file (wiping it first under `paranoid`).
Autoclear detaches the loop device when the mapping goes away; detachment may be
deferred slightly by the kernel.  If `cryptsetup close` fails, the file is kept
because the loop device still holds it; the leftover is cleaned at the next
startup or when the slot is next used.

Bookkeeping.  `/proc/swaps` lists the mapper device (as
`/dev/mapper/swapspace-N` or `/dev/dm-X`, with type `partition`) instead of the
file.  `read_proc_swaps()` maps such an entry to slot N by name, or via
`/sys/block/dm-X/dm/name`.  Mapping names are global, so the name is not proof
of ownership: the entry is adopted only if
`/sys/block/dm-X/slaves/loopY/loop/backing_file` is our file N (compared by
device and inode number with the file in the swap directory).  Another instance's
mapping, or one whose file is gone, is left alone and logged once.  The identity
(dm device, loop device, backing file) is recorded when the slot is first
seen and re-checked on every `/proc/swaps` read, without scanning directories
or allocating: if the mapping behind the name changes, the slot is forgotten
and the entry ignored, so it is never retired by mistake.  Sizes and usage come
from `/proc/swaps` as before.  Each slot remembers whether it is
encrypted, so retirement does not depend on the current setting.

Startup.  A file N that is not active cannot be reused with crypt on: its key
is gone, or it was a plain file holding readable data.  It is deleted (after an
overwrite with zeros under `paranoid`, as in retirement) and a new file is
created when needed.  This was chosen over re-keying the same file (fresh
`mkswap` over old ciphertext or plaintext) because only a newly created file is
known to hold nothing but ciphertext.  A leftover mapping of ours with no active
swap is removed first; if that fails the file is left alone, as for `EBUSY` in
the plain path.  A file that is in use as swap but not listed under our
directory's path (a bind mount or symlink to the same directory) is detected by
comparing device and inode with every `/proc/swaps` entry, and is left alone;
unlike the plain path, crypt mode cannot learn this from `swapon` failing, since
it must never `swapon` a plain file.  Mappings named `swapspace-N` that are not
backed by our file N are not touched at startup in any mode, including with
crypt off; in crypt mode the old file N is then also left in place.  Crypt mode
also leaves numbered entries that are not regular files (symlinks, FIFOs,
directories) alone, as plain mode does, instead of deleting them.

Memory pressure.  This mode is less robust than plain swapfiles under extreme
memory pressure.  `loop.c` allocates its workqueue with
`WQ_UNBOUND | WQ_FREEZABLE` and without `WQ_MEM_RECLAIM`: there is no rescuer
thread, so if no worker can be started for lack of memory, the writes that would
free memory wait for it (deadlock), and because the workqueue is freezable,
swap-out to the device stalls during suspend until resume.  Direct I/O and the
full zero fill remove the page cache and block allocation from the path (on
btrfs only with `NOCOW` and a single data profile), not these properties.  zram or a non-loop path is safer when that matters.

For testing, all system calls of crypt mode (loop attach and close, command
lines, mapping lookup, `mkswap`, `swapon`, `swapoff`) go through the
`struct swap_ops` table (`swapops`).  `tests/fake_system.h` replaces it with an
in-memory system, so the unit tests can fail every step and check the exact
order, the absence of plain `swapon`, and the release of everything acquired,
without root.

Assumptions that need a root run to confirm (`tests/crypt-root.sh`): that
`LOOP_CONFIGURE` with the direct I/O flag and the computed block size is
honoured for files on the supported filesystems and devices, including 4Kn
storage; the exact cryptsetup options above on the installed version; that
`swapon` on a dm device shows up in `/proc/swaps` as described; that autoclear
detaches the loop device after `cryptsetup close`; and that a second
instance, in crypt mode or with crypt off, neither adopts nor closes another's
mapping.
