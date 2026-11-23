/* A fake system for crypt-mode tests that run without root.
 *
 * It provides a replacement for the swap_ops table that keeps all state in
 * memory (loop devices, mappings, active swap) and can fail at any step, plus
 * helpers to build a fake sysfs tree.  Include after src/cryptdev.c.
 */
#ifndef SWAPSPACE_TEST_FAKE_SYSTEM_H
#define SWAPSPACE_TEST_FAKE_SYSTEM_H

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#define FAKE_FD 42
#define FAKE_LOOPNO 7

struct fake_world
{
  /// Events in order, separated by ';': attach, open, close, loopclose, mkswap,
  /// swapon, swapoff.  Operations on anything but a mapper device get a
  /// "plain-" prefix.
  char log[1024];
  /// Descriptors handed out by loop_attach and not yet closed
  int loops_open;
  /// Bit n: a dm mapping swapspace-n exists
  unsigned mapped;
  /// Bit n: swap is active on /dev/mapper/swapspace-n
  unsigned swapped;
  /// Number of mkswap, swapon or swapoff calls on something that is not a
  /// /dev/mapper/swapspace-<n> device
  int plain_calls;
  /// Number of plain files with swap active (plain mode only)
  int plain_swapped;
  /// Sizes of the plain files at each swapon, in order (plain mode only)
  long long plain_sizes[8];
  int plain_sizes_n;

  bool fail_attach;
  bool fail_open;
  /// cryptsetup open fails but leaves a mapping behind
  bool open_leaves_mapping;
  bool fail_mkswap;
  bool fail_swapon;
  /// errno of a failing swapon; EINVAL if left 0
  int swapon_errno;
  bool fail_swapoff;
  bool fail_close;
};

static struct fake_world fake;

static void fake_reset(void)
{
  memset(&fake, 0, sizeof(fake));
}

static void fake_event(const char name[])
{
  if (fake.log[0]) strncat(fake.log, ";", sizeof(fake.log) - strlen(fake.log) - 1);
  strncat(fake.log, name, sizeof(fake.log) - strlen(fake.log) - 1);
}

/// Slot number if dev is /dev/mapper/swapspace-<n>, else -1
static int fake_slot(const char dev[])
{
  int n;
  return crypt_parse_mapper_path(dev, 32, &n) ? n : -1;
}

static bool fake_loop_attach(const char file[], int *loopno, int *loopfd)
{
  fake_event("attach");
  if (fake.fail_attach) return false;
  ++fake.loops_open;
  *loopno = FAKE_LOOPNO;
  *loopfd = FAKE_FD;
  return true;
}

static void fake_loop_close(int loopfd)
{
  fake_event("loopclose");
  if (loopfd == FAKE_FD) --fake.loops_open;
}

static int fake_run(const char line[])
{
  int loop, slot;
  const char *p;
  if (strncmp(line, "cryptsetup open ", 16) == 0 &&
      (p = strstr(line, "/dev/loop")) &&
      sscanf(p, "/dev/loop%d swapspace-%d", &loop, &slot) == 2)
  {
    fake_event("open");
    if (loop != FAKE_LOOPNO || slot < 0 || slot >= 32) return 2;
    if (fake.mapped & (1u << slot)) return 3;
    if (fake.fail_open)
    {
      if (fake.open_leaves_mapping) fake.mapped |= 1u << slot;
      return 1;
    }
    fake.mapped |= 1u << slot;
    return 0;
  }
  if (sscanf(line, "cryptsetup close swapspace-%d", &slot) == 1)
  {
    fake_event("close");
    if (fake.fail_close) return 1;
    if (slot < 0 || slot >= 32 || !(fake.mapped & (1u << slot))) return 4;
    if (fake.swapped & (1u << slot)) return 5;	// busy
    fake.mapped &= ~(1u << slot);
    return 0;
  }
  fake_event("unknown-command");
  return 127;
}

static bool fake_mapping_exists(int seqno)
{
  return seqno >= 0 && seqno < 32 && (fake.mapped & (1u << seqno));
}

static int fake_mkswap(const char dev[])
{
  const bool mapper = fake_slot(dev) >= 0;
  if (!mapper) ++fake.plain_calls;
  fake_event(mapper ? "mkswap" : "plain-mkswap");
  return fake.fail_mkswap ? 1 : 0;
}

static int fake_swapon(const char dev[])
{
  const int slot = fake_slot(dev);
  if (slot < 0) ++fake.plain_calls;
  fake_event(slot >= 0 ? "swapon" : "plain-swapon");
  if (slot < 0 && fake.plain_sizes_n < 8)
  {
    struct stat st;
    fake.plain_sizes[fake.plain_sizes_n++] = stat(dev, &st) == 0 ? st.st_size : -1;
  }
  if (fake.fail_swapon)
  {
    errno = fake.swapon_errno ? fake.swapon_errno : EINVAL;
    return -1;
  }
  if (slot >= 0) fake.swapped |= 1u << slot; else ++fake.plain_swapped;
  return 0;
}

static int fake_swapoff(const char dev[])
{
  const int slot = fake_slot(dev);
  if (slot < 0) ++fake.plain_calls;
  fake_event(slot >= 0 ? "swapoff" : "plain-swapoff");
  if (fake.fail_swapoff) { errno = EBUSY; return -1; }
  if (slot >= 0) fake.swapped &= ~(1u << slot); else --fake.plain_swapped;
  return 0;
}

static const struct swap_ops fake_ops =
{
  .loop_attach = fake_loop_attach,
  .loop_close = fake_loop_close,
  .run = fake_run,
  .mapping_exists = fake_mapping_exists,
  .mkswap = fake_mkswap,
  .swapon = fake_swapon,
  .swapoff = fake_swapoff,
};


/* Fake sysfs and scratch directory helpers. */

static char fake_dir[256];

/// Create a scratch directory under $TMPDIR
static void fake_dir_create(const char prefix[])
{
  snprintf(fake_dir, sizeof(fake_dir), "%s/%s.XXXXXX",
      getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", prefix);
  if (!mkdtemp(fake_dir)) { perror("mkdtemp"); exit(2); }
}

/// Absolute path of name inside the scratch directory
/** The result stays valid for the next 15 calls (a ring of static buffers). */
static const char *fake_path(const char fmt[], ...)
{
  static char out[16][1024];
  static int which;
  which = (which + 1) % 16;
  char rel[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(rel, sizeof(rel), fmt, ap);
  va_end(ap);
  snprintf(out[which], sizeof(out[which]), "%s/%s", fake_dir, rel);
  return out[which];
}

/// mkdir -p for a path inside the scratch directory
static void fake_mkdirs(const char fmt[], ...)
{
  char rel[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(rel, sizeof(rel), fmt, ap);
  va_end(ap);
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", fake_dir, rel);
  for (char *p = path + strlen(fake_dir) + 1; *p; ++p)
  {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(path, 0755) != 0 && errno != EEXIST) { perror(path); exit(2); }
    *p = '/';
  }
  if (mkdir(path, 0755) != 0 && errno != EEXIST) { perror(path); exit(2); }
}

/// Write text to a file inside the scratch directory (parents must exist)
static void fake_write(const char text[], const char fmt[], ...)
{
  char rel[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(rel, sizeof(rel), fmt, ap);
  va_end(ap);
  FILE *fp = fopen(fake_path("%s", rel), "w");
  if (!fp || fputs(text, fp) == EOF || fclose(fp) != 0)
  {
    perror(rel);
    exit(2);
  }
}

/// Add a dm device to the fake sysfs: dm-<dmno> named name, on the given slaves
static void fake_sysfs_dm(int dmno, const char name[], const char *const slaves[])
{
  char text[128];
  snprintf(text, sizeof(text), "%s\n", name);
  fake_mkdirs("sys/block/dm-%d/dm", dmno);
  fake_write(text, "sys/block/dm-%d/dm/name", dmno);
  fake_mkdirs("sys/block/dm-%d/slaves", dmno);
  for (int i = 0; slaves && slaves[i]; ++i)
    fake_mkdirs("sys/block/dm-%d/slaves/%s", dmno, slaves[i]);
}

/// Add a loop device with the given backing file text to the fake sysfs
static void fake_sysfs_loop(const char node[], const char backing[])
{
  char text[1100];
  snprintf(text, sizeof(text), "%s\n", backing);
  fake_mkdirs("sys/block/%s/loop", node);
  fake_write(text, "sys/block/%s/loop/backing_file", node);
}

/// A mapping swapspace-<slot> as sysfs shows it: dm-<dmno> on loop<loopno>,
/// which is backed by the given path
static void fake_sysfs_mapping(int dmno, int slot, int loopno, const char backing[])
{
  char name[32], loop[32];
  snprintf(name, sizeof(name), "swapspace-%d", slot);
  snprintf(loop, sizeof(loop), "loop%d", loopno);
  const char *const slaves[] = { loop, NULL };
  fake_sysfs_dm(dmno, name, slaves);
  fake_sysfs_loop(loop, backing);
}

static void fake_rmtree(const char path[])
{
  struct stat st;
  if (lstat(path, &st) != 0) return;
  if (S_ISDIR(st.st_mode))
  {
    DIR *d = opendir(path);
    if (d)
    {
      for (struct dirent *e = readdir(d); e; e = readdir(d))
      {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
          continue;
        char sub[1280];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        fake_rmtree(sub);
      }
      closedir(d);
    }
    if (rmdir(path) != 0) fprintf(stderr, "could not remove %s\n", path);
  }
  else if (unlink(path) != 0)
  {
    fprintf(stderr, "could not remove %s\n", path);
  }
}

/// Forget every fake sysfs entry
static void fake_sysfs_clear(void)
{
  fake_rmtree(fake_path("sys"));
}

/// Contents of a text file (up to 4 KB), or "" if it cannot be read
static inline const char *fake_text(const char path[])
{
  static char text[4096];
  FILE *fp = fopen(path, "r");
  size_t n = 0;
  if (fp) { n = fread(text, 1, sizeof(text) - 1, fp); fclose(fp); }
  text[n] = '\0';
  return text;
}

/// Remove the whole scratch directory
static void fake_dir_remove(void)
{
  if (fake_dir[0] && !strstr(fake_dir, "..")) fake_rmtree(fake_dir);
}

#endif
