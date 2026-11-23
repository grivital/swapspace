/* Tests for the system-facing crypt code that need no privileges.
 *
 * Two kinds of fakes are used.  The swap_ops table is replaced by an in-memory
 * system (tests/fake_system.h) that can fail at every step, so the fail-closed
 * ordering and the teardown are checked for each failure point.  Separately,
 * cryptsetup is replaced by a script on PATH to check the command lines
 * through the real table.  The loop device ioctls need root and are covered by
 * tests/crypt-root.sh instead.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../src/crypt.c"
#include "../src/support.c"
#include "../src/cryptdev.c"
#include "fake_system.h"

/* Stand-ins for the globals and logging that main.c and log.c provide. */
char localbuf[16384];
bool verbose = false;
bool quiet = true;

void logm(int priority, const char fmt[], ...)
{
  if (!getenv("TEST_CRYPTDEV_VERBOSE")) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

void log_perr(int priority, const char msg[], int err)
{
  logm(priority, "%s: errno %d", msg, err);
}

void log_perr_str(int priority, const char msg[], const char arg[], int err)
{
  logm(priority, "%s '%s': errno %d", msg, arg, err);
}


static int failures = 0;

#define CHECK(expr) \
  do \
  { \
    if (!(expr)) \
    { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
      ++failures; \
    } \
  } while (0)

/// The event log must be exactly this
#define CHECK_LOG(expected) \
  do \
  { \
    if (strcmp(fake.log, (expected)) != 0) \
    { \
      fprintf(stderr, "FAIL %s:%d: log is '%s', expected '%s'\n", __FILE__, \
	  __LINE__, fake.log, (expected)); \
      ++failures; \
    } \
  } while (0)

/// After a failure that was undone: nothing acquired is left, nothing enabled
#define CHECK_CLEAN() \
  do \
  { \
    CHECK(fake.loops_open == 0); \
    CHECK(fake.mapped == 0); \
    CHECK(fake.swapped == 0); \
    CHECK(fake.plain_calls == 0); \
  } while (0)


static const struct swap_ops *real_ops_table;

static void use_fake(void)
{
  fake_reset();
  swapops = &fake_ops;
}


/// crypt_enable() against the fake system, for each failure point
static void test_enable(void)
{
  const char *const file = "5";
  use_fake();

  // Success: the whole chain, in this order, on the mapper device only.
  CHECK(crypt_enable(5, file) == CRYPT_OK);
  CHECK_LOG("attach;open;loopclose;mkswap;swapon");
  CHECK(fake.mapped == 1u << 5 && fake.swapped == 1u << 5);
  CHECK(fake.loops_open == 0 && fake.plain_calls == 0);

  // The slot is taken now: a second attempt must not touch anything.
  use_fake();
  fake.mapped = 1u << 5;
  CHECK(crypt_enable(5, file) == CRYPT_FAILED);
  CHECK_LOG("");

  // Loop device cannot be attached: nothing else may run.
  use_fake();
  fake.fail_attach = true;
  CHECK(crypt_enable(5, file) == CRYPT_FAILED);
  CHECK_LOG("attach");
  CHECK_CLEAN();

  // Invalid key size for the cipher: the loop device is released again.
  use_fake();
  {
    const int saved = crypt_keysize;
    crypt_keysize = 128;	// not valid for xts
    CHECK(crypt_enable(5, file) == CRYPT_FAILED);
    crypt_keysize = saved;
  }
  CHECK_LOG("attach;loopclose");
  CHECK_CLEAN();

  // cryptsetup open fails without creating a mapping: loop released, no close.
  use_fake();
  fake.fail_open = true;
  CHECK(crypt_enable(5, file) == CRYPT_FAILED);
  CHECK_LOG("attach;open;loopclose");
  CHECK_CLEAN();

  // cryptsetup open fails half-way and leaves a mapping: it is removed before
  // the loop descriptor is released.
  use_fake();
  fake.fail_open = fake.open_leaves_mapping = true;
  CHECK(crypt_enable(5, file) == CRYPT_FAILED);
  CHECK_LOG("attach;open;close;loopclose");
  CHECK_CLEAN();

  // ... and if that mapping cannot be removed, the caller is told it still
  // holds the file.
  use_fake();
  fake.fail_open = fake.open_leaves_mapping = fake.fail_close = true;
  CHECK(crypt_enable(5, file) == CRYPT_HELD);
  CHECK_LOG("attach;open;close;loopclose");
  CHECK(fake.mapped == 1u << 5 && fake.loops_open == 0 && fake.swapped == 0);
  CHECK(fake.plain_calls == 0);

  // mkswap fails: swapon is never attempted, the mapping is removed.
  use_fake();
  fake.fail_mkswap = true;
  CHECK(crypt_enable(5, file) == CRYPT_FAILED);
  CHECK_LOG("attach;open;loopclose;mkswap;close");
  CHECK_CLEAN();

  // swapon fails: the mapping is removed, and no plain swapon follows.
  use_fake();
  fake.fail_swapon = true;
  CHECK(crypt_enable(5, file) == CRYPT_FAILED);
  CHECK_LOG("attach;open;loopclose;mkswap;swapon;close");
  CHECK_CLEAN();

  // Failed teardown after mkswap or swapon is reported as HELD.
  use_fake();
  fake.fail_mkswap = fake.fail_close = true;
  CHECK(crypt_enable(5, file) == CRYPT_HELD);
  CHECK_LOG("attach;open;loopclose;mkswap;close");
  CHECK(fake.mapped == 1u << 5 && fake.swapped == 0 && fake.plain_calls == 0);

  use_fake();
  fake.fail_swapon = fake.fail_close = true;
  CHECK(crypt_enable(5, file) == CRYPT_HELD);
  CHECK_LOG("attach;open;loopclose;mkswap;swapon;close");
  CHECK(fake.mapped == 1u << 5 && fake.swapped == 0 && fake.plain_calls == 0);

  // Slot numbers up to the limit work and are the ones passed on.
  use_fake();
  CHECK(crypt_enable(31, "31") == CRYPT_OK);
  CHECK(fake.mapped == 1u << 31 && fake.swapped == 1u << 31);
  CHECK(crypt_enable(-1, "x") == CRYPT_FAILED);
}


/// crypt_retire(), crypt_unmap() and crypt_clear_stale()
static void test_teardown(void)
{
  use_fake();
  CHECK(crypt_enable(2, "2") == CRYPT_OK);

  // swapoff first, then the mapping.
  fake.log[0] = '\0';
  CHECK(crypt_retire(2));
  CHECK_LOG("swapoff;close");
  CHECK(fake.mapped == 0 && fake.swapped == 0);

  // If swapoff fails the mapping must stay: it is still in use.
  use_fake();
  CHECK(crypt_enable(2, "2") == CRYPT_OK);
  fake.log[0] = '\0';
  fake.fail_swapoff = true;
  CHECK(!crypt_retire(2));
  CHECK_LOG("swapoff");
  CHECK(fake.mapped == 1u << 2 && fake.swapped == 1u << 2);

  // swapoff works but the mapping cannot be removed: reported.
  use_fake();
  CHECK(crypt_enable(2, "2") == CRYPT_OK);
  fake.log[0] = '\0';
  fake.fail_close = true;
  CHECK(!crypt_retire(2));
  CHECK_LOG("swapoff;close");

  CHECK(!crypt_retire(-1));

  // Mapping names are global: only a mapping on our file is removed.  The fake
  // sysfs says whose it is.
  static char sys[1024];
  snprintf(sys, sizeof(sys), "%s", fake_path("sys"));
  crypt_sysfs_root = sys;
  fake_mkdirs("swap");
  fake_write("data", "swap/3");
  fake_write("data", "swap/4");

  // No mapping means nothing to do.
  use_fake();
  fake_sysfs_clear();
  CHECK(crypt_clear_stale(3, "swap/3") == CRYPT_SLOT_FREE);
  CHECK_LOG("");

  // A stale mapping of ours is removed.
  use_fake();
  fake_sysfs_mapping(3, 3, 8, fake_path("swap/3"));
  fake.mapped = 1u << 3;
  CHECK(crypt_clear_stale(3, "swap/3") == CRYPT_SLOT_FREE);
  CHECK_LOG("close");
  CHECK(fake.mapped == 0);

  // One that cannot be removed must be reported as such.
  use_fake();
  fake.mapped = 1u << 3;
  fake.fail_close = true;
  CHECK(crypt_clear_stale(3, "swap/3") == CRYPT_SLOT_HELD);
  CHECK_LOG("close");
  CHECK(fake.mapped == 1u << 3);

  // One that is busy (active swap) cannot be removed either.
  use_fake();
  fake.mapped = fake.swapped = 1u << 3;
  CHECK(crypt_clear_stale(3, "swap/3") == CRYPT_SLOT_HELD);

  // A mapping on another file (another instance's) is left alone: no close,
  // whatever else is true of it.
  use_fake();
  fake.mapped = 1u << 3;
  CHECK(crypt_clear_stale(3, "swap/4") == CRYPT_SLOT_FOREIGN);
  CHECK_LOG("");
  CHECK(fake.mapped == 1u << 3);

  // So is one whose ownership cannot be established: no sysfs entry at all, or
  // our file not existing.
  use_fake();
  fake_sysfs_clear();
  fake.mapped = 1u << 3;
  CHECK(crypt_clear_stale(3, "swap/3") == CRYPT_SLOT_FOREIGN);
  fake_sysfs_mapping(3, 3, 8, fake_path("swap/3"));
  CHECK(crypt_clear_stale(3, "swap/missing") == CRYPT_SLOT_FOREIGN);
  CHECK_LOG("");
  CHECK(fake.mapped == 1u << 3);
  fake_sysfs_clear();
  crypt_sysfs_root = "/sys";

  // crypt_mapping_exists goes through the table.
  use_fake();
  CHECK(!crypt_mapping_exists(4));
  fake.mapped = 1u << 4;
  CHECK(crypt_mapping_exists(4));
}


/// The real table: cryptsetup is a script on PATH that logs its arguments
static void test_real_table(const char logpath[])
{
  swapops = real_ops_table;

  // runcommandline reports the exit status, and runs the line as given.
  CHECK(runcommandline("exit 0") == 0);
  CHECK(runcommandline("exit 3") == 3);
  CHECK(runcommandline("kill -9 $$") == -1);

  // Removing a mapping runs "cryptsetup close swapspace-N".
  setenv("FAKE_RC", "0", 1);
  unlink(logpath);
  CHECK(crypt_unmap(4));
  CHECK(strcmp(fake_text(logpath), "close swapspace-4\n") == 0);

  setenv("FAKE_RC", "5", 1);
  unlink(logpath);
  CHECK(!crypt_unmap(4));
  CHECK(strcmp(fake_text(logpath), "close swapspace-4\n") == 0);
  CHECK(!crypt_unmap(-1));

  // A file that does not exist fails before any loop or cryptsetup step, and
  // the file argument cannot inject anything.  The real loop device code is
  // only reachable with root (see crypt-root.sh).
  setenv("FAKE_RC", "1", 1);
  unlink(logpath);
  CHECK(crypt_map_file("no/such/file; touch pwned", 27) == CRYPT_FAILED);
  CHECK(strstr(fake_text(logpath), "open") == NULL);
}


/// What the loop device is asked for, from the hints a filesystem gives
#ifdef LOOP_CONFIGURE
static void test_loop_request(void)
{
  struct loop_config cfg;

  // Alignment 4096 (4Kn storage) must give 4096, 512 must give 512: a request
  // that always said 512 would be refused by the kernel on 4Kn devices.
  memset(&cfg, 0xff, sizeof(cfg));
  CHECK(loop_request(&cfg, 17, 4096, 512, 4096));
  CHECK(cfg.fd == 17 && cfg.block_size == 4096);
  CHECK(cfg.info.lo_flags & LO_FLAGS_AUTOCLEAR);
  CHECK(cfg.info.lo_flags & LO_FLAGS_DIRECT_IO);
  CHECK(!(cfg.info.lo_flags & LO_FLAGS_READ_ONLY));
  CHECK(cfg.info.lo_offset == 0 && cfg.info.lo_sizelimit == 0);

  CHECK(loop_request(&cfg, 17, 512, 4096, 4096));
  CHECK(cfg.block_size == 512);
  loop_request(&cfg, 17, 0, 4096, 4096);	// only the preferred size known
  CHECK(cfg.block_size == 4096);
  loop_request(&cfg, 17, 0, 0, 4096);
  CHECK(cfg.block_size == 512);
  loop_request(&cfg, 17, 4096, 0, 16384);	// 16 KiB pages (Asahi)
  CHECK(cfg.block_size == 4096);
  loop_request(&cfg, 17, 0, 1048576, 4096);	// a large preferred size is capped
  CHECK(cfg.block_size == 4096);
  /* An alignment above the page size cannot be met.  Passing block_size 0 on
   * would make the kernel use the alignment as the block size and keep direct
   * I/O, so the request itself is refused.
   */
  CHECK(!loop_request(&cfg, 17, 8192, 0, 4096));
  CHECK(!loop_request(&cfg, 17, 65536, 4096, 16384));

  struct loop_info64 info;
  memset(&info, 0, sizeof(info));
  CHECK(!loop_dio_active(&info));
  info.lo_flags = LO_FLAGS_AUTOCLEAR;
  CHECK(!loop_dio_active(&info));
  info.lo_flags = LO_FLAGS_AUTOCLEAR | LO_FLAGS_DIRECT_IO;
  CHECK(loop_dio_active(&info));

  // The hints of a real open file are sane, whatever the filesystem says.
  fake_write("x", "hints-probe");
  const int fd = open(fake_path("hints-probe"), O_RDONLY);
  unsigned long dio = 99, blk = 99;
  CHECK(fd != -1);
  io_hints(fd, &dio, &blk);
  close(fd);
  CHECK(dio == 0 || (dio & (dio - 1)) == 0);
  CHECK(blk > 0 && blk != 99);
  io_hints(-1, &dio, &blk);
  CHECK(dio == 0 && blk == 0);
}
#else
static void test_loop_request(void) { }
#endif


/// Fake sysfs: finding mappings, the slot of a /proc/swaps entry, and whether
/// the mapping is backed by our file
static void test_sysfs(void)
{
  swapops = real_ops_table;
  static char sys[1024];
  snprintf(sys, sizeof(sys), "%s", fake_path("sys"));
  crypt_sysfs_root = sys;

  fake_mkdirs("swap");
  fake_mkdirs("other");
  fake_write("data", "swap/5");
  fake_write("data", "other/5");
  fake_write("data", "swap/6");
  if (symlink(fake_path("swap"), fake_path("link")) != 0) perror("symlink");

  // dm-3 is swapspace-5 on loop2, which is backed by swap/5
  static const char *const loop2[] = { "loop2", NULL };
  static const char *const loop3[] = { "loop3", NULL };
  static const char *const two[] = { "loop4", "loop5", NULL };
  static const char *const disk[] = { "sda", NULL };
  static const char *const none[] = { NULL };
  fake_sysfs_dm(3, "swapspace-5", loop2);
  fake_sysfs_loop("loop2", fake_path("swap/5"));
  // dm-4 is someone else's
  fake_sysfs_dm(4, "cryptroot", disk);
  // dm-6 .. dm-10: mapping names of our form, each broken in a different way
  fake_sysfs_dm(6, "swapspace-7", loop3);		// backed by another dir
  fake_sysfs_loop("loop3", fake_path("other/5"));
  fake_sysfs_dm(7, "swapspace-8", two);			// two slaves
  fake_sysfs_loop("loop4", fake_path("swap/5"));
  fake_sysfs_loop("loop5", fake_path("swap/5"));
  fake_sysfs_dm(8, "swapspace-9", disk);		// not on a loop device
  fake_sysfs_dm(9, "swapspace-10", none);		// no slaves at all
  static const char *const loop6[] = { "loop6", NULL };
  fake_sysfs_dm(10, "swapspace-11", loop6);		// deleted backing file
  {
    char text[1100];
    snprintf(text, sizeof(text), "%s (deleted)", fake_path("swap/5"));
    fake_sysfs_loop("loop6", text);
  }
  static const char *const loop8[] = { "loop8", NULL };
  fake_sysfs_dm(11, "swapspace-12", loop8);		// backed via a symlink
  fake_sysfs_loop("loop8", fake_path("link/6"));
  static const char *const loop9[] = { "loop9", NULL };
  fake_sysfs_dm(12, "swapspace-13", loop9);		// no backing_file at all
  fake_mkdirs("sys/block/loop9/loop");

  int n = -1, dmno = -1;

  // Finding a dm device by its name.
  CHECK(crypt_find_dm(5, &dmno) && dmno == 3);
  CHECK(crypt_find_dm(7, &dmno) && dmno == 6);
  CHECK(!crypt_find_dm(6, &dmno));
  CHECK(!crypt_find_dm(0, &dmno));

  // A dm device without its /dev/mapper node still counts as existing.
  // (Slot 5 has no node in the test environment.)
  CHECK(crypt_mapping_exists(5));
  CHECK(!crypt_mapping_exists(30));

  // /proc/swaps names as the daemon sees them.
  CHECK(crypt_swaps_slot("/dev/dm-3", 32, &n) && n == 5);
  CHECK(!crypt_swaps_slot("/dev/dm-4", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/dm-99", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/dm-3", 5, &n));	// slot beyond the limit

  // Backing check, "/dev/dm-X" form.
  CHECK(crypt_mapping_backed_by("/dev/dm-3", 5, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-3", 5, "other/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-3", 5, "swap/6"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-3", 5, "swap/missing"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-6", 7, "swap/5"));
  CHECK(crypt_mapping_backed_by("/dev/dm-6", 7, "other/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-7", 8, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-8", 9, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-9", 10, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-10", 11, "swap/5"));
  CHECK(crypt_mapping_backed_by("/dev/dm-11", 12, "swap/6"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-12", 13, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-77", 5, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/sda1", 5, "swap/5"));

  // "/dev/mapper/swapspace-N" form: the dm device is found by name.
  CHECK(crypt_mapping_backed_by("/dev/mapper/swapspace-5", 5, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-5", 5, "other/5"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-6", 6, "swap/6"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-5", 6, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/other-5", 5, "swap/5"));

  // The traced identity, and the cheap re-check against it.
  struct crypt_mapping m;
  CHECK(crypt_mapping_trace("/dev/dm-3", 5, &m) && m.dmno == 3 && m.loopno == 2);
  CHECK(crypt_mapping_is(&m, "swap/5") && !crypt_mapping_is(&m, "other/5"));
  CHECK(!crypt_mapping_is(&m, "swap/missing"));
  CHECK(!crypt_mapping_trace("/dev/dm-3", 6, &m));	// name is not swapspace-6
  CHECK(crypt_mapping_trace("/dev/mapper/swapspace-5", 5, &m) && m.dmno == 3);
  CHECK(crypt_mapping_unchanged(&m, 5));
  CHECK(!crypt_mapping_unchanged(&m, 6));

  /* Only the entry itself counts: a symlink or a second hard link to a file
   * that a mapping sits on, and a non-regular file, are never "ours".
   */
  CHECK(symlink("5", fake_path("swap/alias")) == 0);
  CHECK(link(fake_path("swap/6"), fake_path("swap/6b")) == 0);
  CHECK(mkfifo(fake_path("swap/fifo"), 0600) == 0);
  CHECK(crypt_mapping_is(&m, "swap/5") && !crypt_mapping_is(&m, "swap/alias"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-12", 12, "swap/6") &&
      !crypt_mapping_backed_by("/dev/mapper/swapspace-12", 12, "swap/6b"));
  static const char *const loop10[] = { "loop10", NULL };
  fake_sysfs_dm(13, "swapspace-14", loop10);
  fake_sysfs_loop("loop10", fake_path("swap/fifo"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-13", 14, "swap/fifo"));

  // The loop device now backs another file: no longer the mapping we traced.
  fake_sysfs_loop("loop2", fake_path("other/5"));
  CHECK(!crypt_mapping_unchanged(&m, 5));
  fake_sysfs_loop("loop2", fake_path("swap/5"));
  CHECK(crypt_mapping_unchanged(&m, 5));

  // The dm device was renamed or sits on a different loop device.
  fake_write("swapspace-9\n", "sys/block/dm-3/dm/name");
  CHECK(!crypt_mapping_unchanged(&m, 5));
  fake_write("swapspace-5\n", "sys/block/dm-3/dm/name");
  CHECK(crypt_mapping_unchanged(&m, 5));
  CHECK(rmdir(fake_path("sys/block/dm-3/slaves/loop2")) == 0);
  CHECK(!crypt_mapping_unchanged(&m, 5));
  fake_mkdirs("sys/block/dm-3/slaves/loop2");
  CHECK(crypt_mapping_unchanged(&m, 5));

  crypt_sysfs_root = "/sys";
}


/// Reading mapper names from sysfs-style files
static void test_names(void)
{
  int n = -1;
  CHECK(crypt_swaps_slot("/dev/mapper/swapspace-3", 32, &n) && n == 3);
  CHECK(!crypt_swaps_slot("/dev/mapper/other-3", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/sda2", 32, &n));
  CHECK(!crypt_swaps_slot("/var/lib/swapspace/3", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/dm-4294967295999", 32, &n));

  fake_write("swapspace-12\n", "dmname");
  const char *const file = fake_path("dmname");
  CHECK(crypt_read_mapper_name(file, 32, &n) && n == 12);
  fake_write("swapspace-12", "dmname");
  CHECK(crypt_read_mapper_name(file, 32, &n) && n == 12);
  fake_write("swapspace-12\nx\n", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  fake_write("swapspace-40\n", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  fake_write("cryptroot\n", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  fake_write("", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  CHECK(!crypt_read_mapper_name("/nonexistent/dm/name", 32, &n));
}


int main(void)
{
  fake_dir_create("swapspace-cryptdev");
  real_ops_table = swapops;

  // Fake cryptsetup first on PATH; it logs its arguments and exits $FAKE_RC.
  fake_write("#!/bin/sh\necho \"$@\" >> \"$FAKE_LOG\"\nexit \"${FAKE_RC:-0}\"\n",
      "cryptsetup");
  chmod(fake_path("cryptsetup"), 0755);
  const char *const oldpath = getenv("PATH");
  char path[PATH_MAX + 512];
  snprintf(path, sizeof(path), "%s:%s", fake_dir,
      oldpath ? oldpath : "/usr/bin:/bin");
  setenv("PATH", path, 1);
  const char *const logpath = fake_path("log");
  setenv("FAKE_LOG", logpath, 1);

  // Failed attempts to run the scripts must not leave stray files in the
  // working directory.
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 2; }
  if (chdir(fake_dir) != 0) { perror("chdir"); return 2; }

  test_real_table(logpath);
  test_enable();
  test_teardown();
  test_loop_request();
  test_sysfs();
  test_names();

  if (chdir(cwd) != 0) perror("chdir back");
  fake_dir_remove();

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
