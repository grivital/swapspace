
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

Swapfile compaction
-------------------

The kernel supports only a limited number of swap areas (at most 32, and often
24-28 depending on kernel config and other swap areas; swapspace's own limit is
`MAX_SWAPFILES`, 32), and swap devices such as zram and swap partitions use some
of them.  Compaction postpones running out of
slots by making each new swapfile larger.

It sits in `alloc_swapfile()`, which asks `compact_limits()` in `calc.c` for the
effective minimum and maximum swapfile size.  Let `active` be the number of
swapfiles in use, counted from the `swapfiles[]` array, and `n = active -
compact_threshold + 1`.  If `compact_threshold` is nonzero, `compact_factor` is
above 1 and `n >= 1`, both `min_swapsize` and `max_swapsize` are multiplied by
`compact_factor^n` (saturating, page aligned).  The maximum is then limited to
`compact_max_swapsize` (0 means no limit), but never to less than
`max_swapsize`, and the minimum never exceeds the maximum.  With `max_swapsize`
at 1 GiB and the defaults (threshold 16, factor 2, cap 16 GiB), swapfiles 1 to
16 may be 1 GiB, and swapfiles 17 to 20 may be 2, 4, 8 and 16 GiB; later files
stay at 16 GiB.  Without a cap the sizes would continue to double, so disk
headroom matters.  `choose_swapsize()` in `calc.c` then checks the free disk
space: the compacted size is used if it fits, otherwise the size from the
configured limits, otherwise the allocation fails and the daemon goes on a
diet, as it did before compaction existed.

The mode keeps no state apart from a level used to log changes.  The multiplier
is computed from the current count at allocation time, and again after a
retirement, so when swapfiles are retired the next allocation steps back down,
and below the threshold new swapfiles have their normal size.  Existing
swapfiles are never resized.  Retirement policy is unchanged.  A NOTICE is
logged when compaction starts and ends; `--verbose` also logs level changes.

Compaction is a mitigation.  It does not add slots and does not help once every
slot is in use.  The threshold counts only swapspace's own swapfiles, not zram
or other swap areas, so it should be lowered by the number of slots those take.
It relates to Tookmund/Swapspace#45 but is not the full algorithm proposed
there.

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
