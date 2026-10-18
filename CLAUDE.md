# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Swapspace is a single-threaded C daemon for Linux that dynamically creates and removes swap files in a dedicated directory based on current memory pressure. It ships as an autotools project (`configure` + `make`), a systemd unit (`swapspace.service`), a SysV/LSB init script (`swapspace.lsbinit`), and a sample config (`swapspace.conf`). It must run as root because it calls `mkswap`, `swapon`, and `swapoff`.

## Build and install

```sh
autoreconf -i          # only needed from a git clone; tarballs include configure
./configure
make                   # builds src/swapspace and the test helper src/hog
make install           # installs to /usr/local by default
```

`configure` options of note:

- `--localstatedir=...` / `--sysconfdir=...` set `VARPREFIX` / `ETCPREFIX`, which become the compiled-in default swap directory (`$localstatedir/lib/swapspace`) and config file (`$sysconfdir/swapspace.conf`). Both are injected via `AM_CFLAGS` in `src/Makefile.am`.
- `--enable-noconfig` defines `NO_CONFIG`: an ultra-lightweight build with no config file and almost no options. Most feature code is wrapped in `#ifndef NO_CONFIG`.
- `--enable-nodebug` defines `NO_DEBUG`: disables assertions and internal consistency checks.

`Makefile.static` and `src/Makefile.static` are a legacy non-autotools build path, not used by the normal workflow.

## Tests

`make check` runs the automated suite:

- `tests/test_calc` — unit tests for page rounding and `ideal_swapsize()` (`tests/test_calc.c` includes `src/calc.c` directly so it tests the exact daemon code).
- `tests/test_cli.sh` — option handling, configuration validation, and the unprivileged `-e` path in a temporary directory. It is skipped in `--enable-noconfig` builds.

`--enable-fuzzing` (clang only) builds `tests/fuzz_meminfo`, a libFuzzer harness for the `/proc/meminfo` parser, with ASan/UBSan coverage; run it with a corpus directory, e.g. `tests/fuzz_meminfo -max_total_time=60 corpus/`. CI (`.github/workflows/ci.yml`) covers gcc/clang in the default, noconfig and nodebug configurations, ASan/UBSan, a 30-second fuzz run, and `make distcheck`.

Deeper testing is still manual and requires root. `make` also builds `src/hog`, the memory-pressure helper: `src/hog <megabytes>` allocates and holds that much memory (or with no argument, reads desired MB values from stdin one per line).

The pre-release scenarios and filesystem matrix are documented in `doc/release.md`; run `make distcheck` and use `swapspace -e` (retire all swapfiles and exit) between tests.

Useful runtime flags for verification:

- `swapspace -i` — validate configuration and exit.
- `swapspace -v` — log state transitions and required-bytes changes.
- `swapspace -e` — deactivate and delete all managed swapfiles.
- `SIGUSR1` — dump state, memory stats, and swapfile table.
- `SIGUSR2` — force an immediate adjustment (enter the "diet" path).

## Architecture

Startup flow (`src/main.c`): parse config → `read_proc_swaps()` → `activate_old_swaps()` → `check_memory_status()` → install signal handlers → create pidfile → optionally daemonize → loop. Each loop iteration sleeps one second and calls `handle_requirements()`, except when a signal set the status/diet flags.

Module responsibilities:

- `src/opts.c` — configuration. A single `options[]` table drives both command-line parsing and `swapspace.conf` entries. This table **must stay sorted alphabetically by long name** (lookup is `bsearch`) and short options must be unique. Each entry declares an argument type, min/max, and a setter callback. Config is applied in three passes: command line, then config file, then command line again so CLI wins.
- `src/memory.c` — parses `/proc/meminfo`, estimates free space (uses `MemAvailable` when the kernel provides it, otherwise a heuristic over buffers/cache elasticities), and implements `memory_target()`. The return value is a **delta in bytes**: positive means "allocate this much more swap", negative means "we have this much excess". Where available it also consults PSI (`/proc/pressure/memory`, `--psi_threshold`) and replaces host-wide figures with the process's cgroup v2/v1 memory and swap limits.
- `src/state.c` — the allocation policy, a four-state finite machine: `steady`, `hungry`, `overfed`, `diet`. A cooldown timer (`cooldown`, default 600 seconds) prevents allocation/deallocation thrashing. `alloc_swapfile()`/`free_swapfile()` are only called from here. The behavior is described in `doc/technicaldetails.md` and visualized in `doc/fsm.dot` / `fsm.png`.
- `src/swaps.c` — everything about swap files: the `swapfiles[]` slot array, scanning the swap directory, parsing `/proc/swaps`, creating files (`posix_fallocate` when available, otherwise writing zeroed pages), `mkswap`/`swapon`/`swapoff`, and `retire_all()`. It also enforces the swap directory's permissions (0700 root-only) and, when the filesystem supports it, clears compression and sets `NOCOW` (needed for btrfs).
- `src/support.c` — `runcommand()` helpers plus fallback implementations of `swapon`/`swapoff` when libc lacks them.
- `src/log.c` — logging to syslog in daemon mode, stdout/stderr otherwise.

Key invariants and conventions:

- **All sizes are bytes, held in `memsize_t` (`long long`).** `MEMSIZE_ERROR` (`LLONG_MIN`) is the error sentinel. Convert units only at the edges (config input, `/proc` output).
- **`env.h` must be included first in every `.c` file**, before any system header, to enable large-file support. Use the `SEEK()` macro from it rather than calling `lseek` directly.
- **`localbuf`** is a shared 16 KB scratch buffer declared in `main.c`. Many functions are documented as "clobbers localbuf"; do not assume its contents survive a call, and avoid nested use.
- **No dynamic allocation after startup.** Swapspace must keep working when the system is out of memory, so it deliberately uses fixed arrays and the static buffer. New code should follow this constraint.
- Swap files live directly in the swap directory and are named by decimal sequence number (`0`, `1`, ...). `MAX_SWAPFILES` is 32 (`1 << 5`); slot `MAX_SWAPFILES` is a zeroed sentinel. Size class grows as files are allocated.
- An exclusive `flock()` on `.swapspace.lock` in the swap directory keeps instances from overlapping; it is held for the process lifetime and the file is never unlinked (that would defeat the lock). Because of this, a stale pidfile is safe to overwrite.
- `/proc/swaps` reports sizes in 1 KB units; the parser converts them with `KILO`.
- Swap directory operations run with the process `chdir`'d into the directory (`to_swapdir()`), so file names are relative.

## Release process

Documented in `doc/release.md`: run the manual test scenarios, update `NEWS`, commit the version bump in `configure.ac` plus `NEWS` as "Version x.xx", then sign the *previous* commit as tag `vx.xx` with the changelog in the tag message. Run `make distcheck` first.

## Documentation

- `doc/swapspace.8` — man page (options and behavior).
- `doc/technicaldetails.md` — state-machine algorithm.
- `doc/comparison.md` — design rationale (notably the low-memory constraint).
- `doc/unix-ipc-sucks.md` — why signal handlers set flags and the main loop does the work.
