/* Tests for crypt mode in swaps.c that need no privileges.
 *
 * This includes the daemon's own swaps.c, so the real allocation, adoption,
 * retirement and startup code runs, against the in-memory fake system of
 * tests/fake_system.h (no loop devices, no cryptsetup, no real swapon), a fake
 * sysfs tree and a fake /proc/swaps, all inside a temporary directory.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Compiled-in default for the swap directory; the test overrides it. */
#ifndef VARPREFIX
#define VARPREFIX "/var"
#endif

#include "../src/calc.c"
#include "../src/crypt.c"
#include "../src/support.c"
#include "../src/cryptdev.c"
#include "fake_system.h"

#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <linux/fs.h>
#include <linux/magic.h>

/* Count the calls that decide how a swapfile is populated. */
static int fallocate_calls = 0;
static int fsync_calls = 0;

static int counting_fallocate(int fd, off_t off, off_t len)
{
  ++fallocate_calls;
  return posix_fallocate(fd, off, len);
}

static int counting_fsync(int fd)
{
  ++fsync_calls;
  return fsync(fd);
}

/* The filesystem of the swap directory is made to look like btrfs, with or
 * without NOCOW on new files, so that the crypt-mode refusal can be tested.
 */
static bool fake_btrfs = false;
static bool fake_nocow = true;

static int fake_fstatfs(int fd, struct statfs *buf)
{
  const int r = fstatfs(fd, buf);
  if (r == 0 && fake_btrfs) buf->f_type = BTRFS_SUPER_MAGIC;
  return r;
}

static int fake_ioctl(int fd, unsigned long request, ...)
{
  va_list ap;
  va_start(ap, request);
  void *const arg = va_arg(ap, void *);
  va_end(ap);
  if (fake_btrfs && request == FS_IOC_GETFLAGS)
  {
    *(int *)arg = fake_nocow ? FS_NOCOW_FL : 0;
    return 0;
  }
  return ioctl(fd, request, arg);
}

#define posix_fallocate counting_fallocate
#define fsync counting_fsync
#define fstatfs fake_fstatfs
#define ioctl fake_ioctl

#include "../src/swaps.c"

#undef posix_fallocate
#undef fsync
#undef fstatfs
#undef ioctl

/* Stand-ins for the globals and functions of the rest of the daemon. */
char localbuf[16384];
time_t runclock = 0;
bool verbose = false;
bool quiet = false;
static int diets = 0;

void request_diet(void) { ++diets; }
void dump_memory(void) { }
void dump_state(void) { }

static char logtext[8192];

void logm(int priority, const char fmt[], ...)
{
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (strlen(logtext) + strlen(line) + 2 < sizeof(logtext))
  {
    strcat(logtext, line);
    strcat(logtext, "\n");
  }
  if (getenv("TEST_SWAPS_VERBOSE")) fprintf(stderr, "%s\n", line);
}

void log_perr(int priority, const char msg[], int err)
{
  logm(priority, "%s: errno %d", msg, err);
}

void log_perr_str(int priority, const char msg[], const char arg[], int err)
{
  logm(priority, "%s '%s': errno %d", msg, arg, err);
}

void log_discrep(int priority, const char what[], int seqno, memsize_t a,
    memsize_t b)
{
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


/* --paranoid does not exist in the unconfigurable build. */
#ifndef NO_CONFIG
#define SET_PARANOID(value) (paranoid = (value))
#else
#define SET_PARANOID(value) ((void)0)
#endif

static char sysroot[1024];
static char procpath[1024];

static bool exists(const char name[])
{
  struct stat st;
  return lstat(name, &st) == 0;
}

static void remove_numbered_files(void)
{
  DIR *d = opendir(".");
  if (!d) return;
  for (struct dirent *e = readdir(d); e; e = readdir(d))
    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
      unlink(e->d_name);
  closedir(d);
}

/// Back to a known state, in the swap directory of the scratch area
static void reset(bool crypt)
{
  remove_numbered_files();
  memset(swapfiles, 0, sizeof(swapfiles));
  sequence_number = MAX_SWAPFILES - 1;	// so that the next slot used is 0
  foreign_logged = 0;
  pfalloc_ok = true;
  fake_btrfs = false;
  fake_nocow = true;
  fake_sysfs_clear();
  proc_swaps_read_ok = false;
  diets = 0;
  fallocate_calls = fsync_calls = 0;
  logtext[0] = '\0';
  fake_reset();
  swapops = &fake_ops;
  crypt_enabled = crypt;
  SET_PARANOID(false);
  proc_swaps_path = procpath;
  fake_write("Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n", "proc_swaps");
}

static void set_proc_swaps(const char body[])
{
  char text[2048];
  snprintf(text, sizeof(text),
      "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n%s", body);
  fake_write(text, "proc_swaps");
}

static void make_file(const char name[], size_t bytes, char fill)
{
  FILE *fp = fopen(name, "w");
  if (!fp) { perror(name); exit(2); }
  for (size_t i = 0; i < bytes; ++i) fputc(fill, fp);
  if (fclose(fp) != 0) { perror(name); exit(2); }
}

static memsize_t page(void) { return getpagesize(); }

/// Absolute path of swapfile n in the swap directory
static const char *swapfile_path(int n)
{
  return fake_path("swap/%d", n);
}

/// A mapping swapspace-<n> exists, on a loop device backed by the given path
static void add_mapping(int n, const char backing[])
{
  fake_sysfs_mapping(20 + n, n, 30 + n, backing);
  fake.mapped |= 1u << n;
}

static bool all_bytes(const char name[], char value)
{
  FILE *fp = fopen(name, "r");
  if (!fp) return false;
  int c;
  bool ok = true;
  while ((c = fgetc(fp)) != EOF) if (c != (unsigned char)value) ok = false;
  fclose(fp);
  return ok;
}


static void test_alloc_crypt(void)
{
  // Success: zeros are written and flushed, not preallocated; only the mapper
  // device is ever formatted and enabled.
  reset(true);
  CHECK(alloc_swapfile(8 * page()));
  CHECK_LOG("attach;open;loopclose;mkswap;swapon");
  CHECK(fake.plain_calls == 0);
  CHECK(swapfiles[0].size >= 8 * page() && swapfiles[0].crypt);
  CHECK(exists("0"));
  CHECK(fallocate_calls == 0);
  CHECK(fsync_calls >= 1);
  CHECK(all_bytes("0", '\0'));
  CHECK(fake.mapped == 1u && fake.swapped == 1u && fake.loops_open == 0);

  // The same with crypt off preallocates and never touches the mapper.
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  CHECK_LOG("plain-mkswap;plain-swapon");
  CHECK(!swapfiles[0].crypt);
#ifdef HAVE_PFALLOCATE
  CHECK(fallocate_calls >= 1);
#endif
  CHECK(fake.mapped == 0 && fake.plain_swapped == 1);

  /* After a switch from crypt to plain, a leftover mapping of ours that cannot
   * be removed still holds file 0: plain mode must not unlink it, and uses the
   * next slot instead.  A removable one is closed first, as in crypt mode.
   */
  reset(false);
  make_file("0", 4 * page(), 'M');
  add_mapping(0, swapfile_path(0));
  fake.fail_close = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK(all_bytes("0", 'M') && fake.mapped == 1u && diets >= 1);
  CHECK(alloc_swapfile(8 * page()) && exists("1") && swapfiles[1].size);
  CHECK(!swapfiles[0].size && all_bytes("0", 'M'));
  reset(false);
  make_file("0", 4 * page(), 'M');
  add_mapping(0, swapfile_path(0));
  CHECK(alloc_swapfile(8 * page()) && fake.mapped == 0);
  CHECK_LOG("close;plain-mkswap;plain-swapon");

  // Each failure point: no swap is left, no plain swapon ever happens, the
  // file is deleted, and the daemon asks for a diet.
  struct { const char *name; bool *knob; const char *log; } cases[] =
  {
    { "attach", &fake.fail_attach, "attach" },
    { "open", &fake.fail_open, "attach;open;loopclose" },
    { "mkswap", &fake.fail_mkswap, "attach;open;loopclose;mkswap;close" },
    { "swapon", &fake.fail_swapon,
      "attach;open;loopclose;mkswap;swapon;close" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
  {
    reset(true);
    *cases[i].knob = true;
    CHECK(!alloc_swapfile(8 * page()));
    if (strcmp(fake.log, cases[i].log) != 0)
    {
      fprintf(stderr, "FAIL %s failure: log '%s', expected '%s'\n",
	  cases[i].name, fake.log, cases[i].log);
      ++failures;
    }
    CHECK(fake.plain_calls == 0 && fake.plain_swapped == 0);
    CHECK(fake.mapped == 0 && fake.swapped == 0 && fake.loops_open == 0);
    CHECK(!exists("0"));
    CHECK(swapfiles[0].size == 0 && !swapfiles[0].crypt);
    CHECK(diets >= 1);
  }

  // cryptsetup failing half-way leaves a mapping that must be removed.
  reset(true);
  fake.fail_open = fake.open_leaves_mapping = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("attach;open;close;loopclose");
  CHECK(fake.mapped == 0 && !exists("0") && fake.plain_calls == 0);

  // A mapping that cannot be removed still holds the file: keep the file.
  reset(true);
  fake.fail_swapon = fake.fail_close = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK(exists("0"));
  CHECK(fake.mapped == 1u && fake.plain_calls == 0);
  CHECK(swapfiles[0].size == 0);

  reset(true);
  fake.fail_open = fake.open_leaves_mapping = fake.fail_close = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK(exists("0"));
  CHECK(fake.mapped == 1u && fake.plain_calls == 0);

  // The next attempt on that slot removes the leftover first, then works.
  add_mapping(0, swapfile_path(0));
  fake.fail_open = fake.open_leaves_mapping = fake.fail_close = false;
  fake.log[0] = '\0';
  CHECK(alloc_swapfile(8 * page()));
  CHECK_LOG("close;attach;open;loopclose;mkswap;swapon");
  CHECK(fake.mapped == 1u && fake.swapped == 1u);
  CHECK(swapfiles[0].crypt);

  // A leftover mapping of ours that cannot be removed blocks the slot: nothing
  // is created, and nothing is enabled.
  reset(true);
  make_file("0", 4 * page(), '\0');
  add_mapping(0, swapfile_path(0));
  fake.fail_close = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("close");
  CHECK(exists("0") && fake.plain_calls == 0 && diets >= 1);
  CHECK(fake.mapped == 1u);

  // A mapping of that name that is not ours (another instance's, on a file
  // elsewhere) is never closed: its slot number is skipped.
  reset(true);
  fake_mkdirs("elsewhere");
  fake_write("data", "elsewhere/0");
  add_mapping(0, fake_path("elsewhere/0"));
  CHECK(alloc_swapfile(8 * page()));
  CHECK_LOG("attach;open;loopclose;mkswap;swapon");
  CHECK(fake.mapped == 3u && fake.swapped == 2u);	// theirs untouched, ours is 1
  CHECK(swapfiles[1].crypt && swapfiles[0].size == 0 && !exists("0"));
  CHECK(exists("1") && diets == 0);
  CHECK(strstr(logtext, "Ignoring mapping swapspace-0") != NULL);

  // ... also when it is the only slot left: nothing is created.
  reset(true);
  add_mapping(0, fake_path("elsewhere/0"));
  for (int i = 1; i < MAX_SWAPFILES; ++i) swapfiles[i].size = 1;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("");
  CHECK(fake.mapped == 1u && !exists("0") && diets >= 1);
  memset(swapfiles, 0, sizeof(swapfiles));

  // On btrfs a file without NOCOW would bounce direct writes through freshly
  // allocated pages: crypt mode refuses to create it.  Plain mode, and a
  // filesystem that does keep NOCOW, are not affected.
  reset(true);
  fake_btrfs = true;
  fake_nocow = false;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("");
  CHECK(!exists("0") && swapfiles[0].size == 0 && diets >= 1);
  CHECK(strstr(logtext, "Refusing to create encrypted swapfile") != NULL);

  reset(true);
  fake_btrfs = true;
  CHECK(alloc_swapfile(8 * page()));
  CHECK(swapfiles[0].crypt);

  reset(false);
  fake_btrfs = true;
  fake_nocow = false;
  CHECK(alloc_swapfile(8 * page()));
  CHECK(!swapfiles[0].crypt);

  // Crypt mode never retries the plain way: its swapon is on the mapper device.
  reset(true);
  fake.fail_swapon = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("attach;open;loopclose;mkswap;swapon;close");
  CHECK(pfalloc_ok);
}


/// Retirement of plain swapfiles, which shares remove_swapfile() with startup
static void test_retire_plain(void)
{
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  fake.log[0] = '\0';
  CHECK(retire_all());
  CHECK_LOG("plain-swapoff");
  CHECK(!exists("0") && swapfiles[0].size == 0 && fake.plain_swapped == 0);

  // swapoff failing keeps the file and the slot.
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  fake.fail_swapoff = true;
  CHECK(!retire_all());
  CHECK(exists("0") && swapfiles[0].size != 0);

#ifndef NO_CONFIG
  // Paranoid retirement overwrites the file before it is gone.
  reset(false);
  paranoid = true;
  CHECK(alloc_swapfile(8 * page()));
  CHECK(link("0", "keep") == 0);
  {
    FILE *fp = fopen("0", "r+");
    CHECK(fp != NULL);
    if (fp) { for (int i = 0; i < 4096; ++i) fputc('S', fp); fclose(fp); }
  }
  CHECK(!all_bytes("keep", '\0'));	// control: the data is there
  CHECK(retire_all());
  CHECK(!exists("0") && exists("keep"));
  CHECK(all_bytes("keep", '\0'));

  /* A file that cannot be unlinked (the kernel refuses that for active swap)
   * holds live data, so it must not be wiped either.  Unlinking is made to fail
   * by taking write permission off the directory, which root ignores.
   */
  if (geteuid() != 0)
  {
    reset(false);
    paranoid = true;
    make_file("3", 4 * page(), 'M');
    CHECK(chmod(".", 0555) == 0);
    remove_swapfile("3", MEMSIZE_ERROR);
    CHECK(chmod(".", 0755) == 0);
    CHECK(exists("3") && all_bytes("3", 'M'));
  }
#endif
}


/// The retry after a plain swapon() failing with EINVAL (no posix_fallocate)
static void test_plain_einval(void)
{
  const long long wanted = trunc_to_page(8 * page()) + 2 * page();

  // Exactly one retry, asking for the same size again: the request is reduced
  // by the overhead that alloc_swapfile() adds back.
  reset(false);
  fake.fail_swapon = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("plain-mkswap;plain-swapon;plain-mkswap;plain-swapon");
  CHECK(fake.plain_sizes_n == 2);
  CHECK(fake.plain_sizes[0] == wanted && fake.plain_sizes[1] == wanted);
  CHECK(!pfalloc_ok);
  CHECK(!exists("0") && swapfiles[0].size == 0);

  // Once quick creation is off, there is no further retry.
  reset(false);
  pfalloc_ok = false;
  fake.fail_swapon = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("plain-mkswap;plain-swapon");

  // Other errors are not retried, and leave quick creation on.
  const int others[] = { ENOSPC, EBUSY, ENOMEM, EPERM };
  for (size_t i = 0; i < sizeof(others) / sizeof(*others); ++i)
  {
    reset(false);
    fake.fail_swapon = true;
    fake.swapon_errno = others[i];
    CHECK(!alloc_swapfile(8 * page()));
    CHECK_LOG("plain-mkswap;plain-swapon");
    CHECK(pfalloc_ok && diets >= 1);
  }
}


/// dump_stats keeps its table unless some slot is encrypted
static void test_dump_stats(void)
{
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  logtext[0] = '\0';
  dump_stats();
  CHECK(strstr(logtext,
	"file            size            used         created  seen\n") != NULL);
  CHECK(strstr(logtext, "crypt") == NULL);
  // One row, ending in the "seen" flag.
  const char *row = strstr(logtext, "   0");
  CHECK(row != NULL && strstr(row, "  0\n") != NULL);
}


static void test_dump_stats_crypt(void)
{
  reset(true);
  CHECK(alloc_swapfile(8 * page()));
  logtext[0] = '\0';
  dump_stats();
  CHECK(strstr(logtext, "seen  crypt\n") != NULL);
  CHECK(strstr(logtext, "      1\n") != NULL);
}


static void test_retire(void)
{
  // An encrypted slot is retired through the mapper, whatever the current
  // setting says.
  for (int mode = 0; mode < 2; ++mode)
  {
    reset(true);
    CHECK(alloc_swapfile(8 * page()));
    crypt_enabled = mode;	// retirement must follow the slot, not the setting
    fake.log[0] = '\0';
    CHECK(retire_all());
    CHECK_LOG("swapoff;close");
    CHECK(!exists("0"));
    CHECK(swapfiles[0].size == 0 && !swapfiles[0].crypt);
    CHECK(fake.mapped == 0 && fake.swapped == 0 && fake.plain_calls == 0);
  }

  // A plain slot is retired by file name, even with crypt on.
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  crypt_enabled = true;
  fake.log[0] = '\0';
  CHECK(retire_all());
  CHECK_LOG("plain-swapoff");
  CHECK(!exists("0") && fake.plain_swapped == 0);

  // swapoff failing keeps everything: the file, the mapping and the slot.
  reset(true);
  CHECK(alloc_swapfile(8 * page()));
  fake.log[0] = '\0';
  fake.fail_swapoff = true;
  CHECK(!retire_all());
  CHECK_LOG("swapoff");
  CHECK(exists("0") && fake.mapped == 1u && swapfiles[0].size != 0);

  // The mapping failing to close after swapoff keeps the file too.
  reset(true);
  CHECK(alloc_swapfile(8 * page()));
  fake.log[0] = '\0';
  fake.fail_close = true;
  CHECK(!retire_all());
  CHECK_LOG("swapoff;close");
  CHECK(exists("0") && swapfiles[0].size != 0);

#ifndef NO_CONFIG
  // Paranoid retirement overwrites the file before it is gone.
  reset(true);
  paranoid = true;
  CHECK(alloc_swapfile(8 * page()));
  CHECK(link("0", "keep") == 0);
  memset(localbuf, 'S', 16);
  {
    // Put recognizable data in the file, as swapped-out pages would be.
    FILE *fp = fopen("0", "r+");
    CHECK(fp != NULL);
    if (fp) { for (int i = 0; i < 4096; ++i) fputc('S', fp); fclose(fp); }
  }
  CHECK(retire_all());
  CHECK(!exists("0") && exists("keep"));
  CHECK(all_bytes("keep", '\0'));
#endif
}


/// The fake sysfs of the adoption tests: dm-3 is our swapspace-5, dm-4 is a
/// swapspace-6 on another directory's file, dm-9 is not ours by name
static void adoption_sysfs(void)
{
  static const char *const loopB[] = { "loop3", NULL };
  fake_mkdirs("elsewhere");
  fake_write("data", "elsewhere/5");
  fake_sysfs_mapping(3, 5, 2, swapfile_path(5));
  fake_sysfs_mapping(4, 6, 3, fake_path("elsewhere/5"));
  fake_sysfs_dm(9, "cryptroot", loopB);
}

static void test_adoption(void)
{
  char body[1024];

  reset(true);
  adoption_sysfs();
  make_file("5", 4 * page(), '\0');

  // Our mapping, listed as a dm node: adopted, marked encrypted.
  set_proc_swaps("/dev/dm-3\t\t\t\tpartition\t4096\t\t8\t\t-2\n");
  CHECK(read_proc_swaps());
  CHECK(swapfiles[5].size == 4096 * KILO && swapfiles[5].crypt);
  CHECK(swapfiles[5].used == 8 * KILO);

  // Already tracked: re-checked on every read against the recorded identity.
  // While the mapping is the same it stays; once the loop device behind the
  // name backs something else, the slot is forgotten and reported once.
  CHECK(read_proc_swaps() && swapfiles[5].size != 0);
  CHECK(swapfiles[5].traced);
  fake_sysfs_loop("loop2", fake_path("elsewhere/5"));
  CHECK(read_proc_swaps());
  CHECK(swapfiles[5].size == 0 && !swapfiles[5].crypt);
  CHECK(read_proc_swaps() && swapfiles[5].size == 0);
  {
    const char *first = strstr(logtext, "Ignoring mapping swapspace-5");
    CHECK(first != NULL);
    CHECK(first && strstr(first + 1, "Ignoring mapping swapspace-5") == NULL);
  }

  // The same, listed by its mapper name.
  reset(true);
  adoption_sysfs();
  make_file("5", 4 * page(), '\0');
  set_proc_swaps("/dev/mapper/swapspace-5\tpartition\t4096\t\t0\t\t-2\n");
  CHECK(read_proc_swaps());
  CHECK(swapfiles[5].size == 4096 * KILO && swapfiles[5].crypt);

  // A slot we just created is traced when it first shows up in /proc/swaps.
  reset(true);
  CHECK(alloc_swapfile(8 * page()));
  CHECK(!swapfiles[0].traced);
  fake_sysfs_mapping(3, 0, 2, swapfile_path(0));
  set_proc_swaps("/dev/mapper/swapspace-0\tpartition\t4096\t\t0\t\t-2\n");
  CHECK(read_proc_swaps());
  CHECK(swapfiles[0].crypt && swapfiles[0].traced && swapfiles[0].size == 4096 * KILO);

  // Not adopted: no file of that number here (this also covers the mapping
  // that belongs to nothing of ours).
  reset(true);
  adoption_sysfs();
  set_proc_swaps("/dev/dm-3\t\t\t\tpartition\t4096\t\t0\t\t-2\n");
  CHECK(read_proc_swaps());
  CHECK(swapfiles[5].size == 0);

  // Not adopted: the mapping sits on another directory's file, although a
  // file of that number exists here too.  Reported once.
  reset(true);
  adoption_sysfs();
  make_file("6", 4 * page(), '\0');
  set_proc_swaps("/dev/dm-4\t\t\t\tpartition\t4096\t\t0\t\t-2\n");
  CHECK(read_proc_swaps());
  CHECK(read_proc_swaps());
  CHECK(swapfiles[6].size == 0 && !swapfiles[6].crypt);
  {
    const char *first = strstr(logtext, "Ignoring");
    CHECK(first != NULL);
    CHECK(first && strstr(first + 1, "Ignoring") == NULL);
  }
  // Such a mapping is never retired by us: retirement works on tracked slots.
  CHECK(retire_all());
  CHECK(fake.log[0] == '\0' && exists("6"));

  // Not ours: a dm device of another name, and a plain partition.
  reset(true);
  adoption_sysfs();
  make_file("5", 4 * page(), '\0');
  snprintf(body, sizeof(body),
      "/dev/dm-9\t\t\t\tpartition\t4096\t\t0\t\t-2\n"
      "/dev/sda2\t\t\t\tpartition\t4096\t\t0\t\t-3\n");
  set_proc_swaps(body);
  CHECK(read_proc_swaps());
  for (int i = 0; i < MAX_SWAPFILES; ++i) CHECK(swapfiles[i].size == 0);

  // A plain swapfile in our directory is adopted as plain, with crypt on too.
  reset(true);
  make_file("2", 4 * page(), '\0');
  snprintf(body, sizeof(body), "%s/2\t\tfile\t\t4096\t\t0\t\t-2\n", swappath);
  set_proc_swaps(body);
  CHECK(read_proc_swaps());
  CHECK(swapfiles[2].size == 4096 * KILO && !swapfiles[2].crypt);

  // A tracked encrypted slot that disappears from the list is forgotten.
  reset(true);
  adoption_sysfs();
  make_file("5", 4 * page(), '\0');
  set_proc_swaps("/dev/dm-3\t\t\t\tpartition\t4096\t\t0\t\t-2\n");
  CHECK(read_proc_swaps() && swapfiles[5].size != 0);
  set_proc_swaps("");
  CHECK(read_proc_swaps() && swapfiles[5].size == 0);
}


/// Startup with crypt off: old files are reused as before
static void test_startup_plain(void)
{
  const size_t size = 4 * (size_t)page();

  // An old file is reused as plain swap.
  reset(false);
  make_file("3", size, '\0');
  CHECK(activate_old_swaps());
  CHECK_LOG("plain-mkswap;plain-swapon");
  CHECK(exists("3"));

  // A file that cannot be enabled because it is busy (EBUSY) is kept; any
  // other error deletes it.
  reset(false);
  make_file("3", size, '\0');
  fake.fail_swapon = true;
  fake.swapon_errno = EBUSY;
  CHECK(activate_old_swaps());
  CHECK(exists("3") && swapfiles[3].size == 0);

  reset(false);
  make_file("3", size, '\0');
  fake.fail_swapon = true;
  fake.swapon_errno = ENOSPC;
  CHECK(activate_old_swaps());
  CHECK(!exists("3"));

  // Names that are not swapfile numbers are never touched.
  reset(false);
  make_file("notes", size, 'N');
  CHECK(activate_old_swaps());
  CHECK(exists("notes"));
}


static void test_startup(void)
{
  const size_t size = 4 * (size_t)page();

#ifndef NO_CONFIG
  // Crypt on: an inactive old file is deleted, wiped first under --paranoid.
  reset(true);
  paranoid = true;
  make_file("3", size, 'M');
  CHECK(link("3", "keep") == 0);
  CHECK(activate_old_swaps());
  CHECK(!exists("3") && exists("keep"));
  CHECK(all_bytes("keep", '\0'));
  CHECK(fake.log[0] == '\0');

  // Control: without --paranoid the data is not touched, so the check above
  // really detects a wipe.
  reset(true);
  make_file("3", size, 'M');
  CHECK(link("3", "keep") == 0);
  CHECK(activate_old_swaps());
  CHECK(!exists("3") && all_bytes("keep", 'M'));
#endif

  // A stale mapping of ours goes first.
  reset(true);
  make_file("3", size, 'M');
  add_mapping(3, swapfile_path(3));
  CHECK(activate_old_swaps());
  CHECK_LOG("close");
  CHECK(!exists("3") && fake.mapped == 0);

  // If it cannot be removed, the file it holds is left alone.
  reset(true);
  make_file("3", size, 'M');
  add_mapping(3, swapfile_path(3));
  fake.fail_close = true;
  CHECK(activate_old_swaps());
  CHECK(exists("3") && fake.mapped == 1u << 3);

  /* A mapping of that name that sits on another directory's file is not ours,
   * in either mode: it is not closed, and its slot is left as it is in crypt
   * mode.  With crypt off the file is a plain swapfile, which a mapping name
   * does not affect.
   */
  fake_mkdirs("elsewhere");
  fake_write("data", "elsewhere/3");
  reset(true);
  make_file("3", size, 'M');
  add_mapping(3, fake_path("elsewhere/3"));
  CHECK(activate_old_swaps());
  CHECK_LOG("");
  CHECK(exists("3") && fake.mapped == 1u << 3);
  CHECK(strstr(logtext, "Ignoring mapping swapspace-3") != NULL);

  reset(false);
  make_file("3", size, '\0');
  add_mapping(3, fake_path("elsewhere/3"));
  CHECK(activate_old_swaps());
  CHECK_LOG("plain-mkswap;plain-swapon");
  CHECK(exists("3") && fake.mapped == 1u << 3);

  // With crypt off a leftover mapping of ours is still removed first.
  reset(false);
  make_file("3", size, '\0');
  add_mapping(3, swapfile_path(3));
  CHECK(activate_old_swaps());
  CHECK_LOG("close;plain-mkswap;plain-swapon");
  CHECK(fake.mapped == 0);

  // A symlink or a FIFO is never deleted, closed or adopted, in either mode.
  for (int crypt = 0; crypt < 2; ++crypt)
  {
    reset(crypt);
    make_file("target", size, 'T');
    CHECK(symlink("target", "3") == 0 && mkfifo("4", 0600) == 0);
    add_mapping(3, fake_path("swap/target"));
    CHECK(activate_old_swaps() && exists("3") && exists("4"));
    CHECK(all_bytes("target", 'T') && fake.mapped == 1u << 3 && !fake.log[0]);
    set_proc_swaps("/dev/mapper/swapspace-3\tpartition\t4096\t\t0\t\t-2\n");
    CHECK(read_proc_swaps() && swapfiles[3].size == 0);
  }

  // Crypt on: a file that is in use as swap but is listed under another path
  // to the same directory must not be deleted.
  if (symlink(fake_path("swap"), fake_path("alias")) != 0 && errno != EEXIST)
    perror("symlink");
  reset(true);
  make_file("3", size, 'M');
  {
    char body[1024];
    snprintf(body, sizeof(body), "%s/3\t\tfile\t\t4096\t\t0\t\t-2\n",
	fake_path("alias"));
    set_proc_swaps(body);
  }
  CHECK(activate_old_swaps());
  CHECK(exists("3") && all_bytes("3", 'M') && fake.log[0] == '\0');
  CHECK(strstr(logtext, "in use as swap") != NULL);
  // Control: the same file, not listed, is deleted.
  set_proc_swaps("");
  CHECK(activate_old_swaps());
  CHECK(!exists("3"));

  // Names that are not swapfile numbers are never touched.
  reset(true);
  SET_PARANOID(true);
  make_file("notes", size, 'N');
  make_file("33", size, 'N');
  CHECK(activate_old_swaps());
  CHECK(exists("notes") && exists("33") && all_bytes("notes", 'N'));

}


int main(void)
{
  fake_dir_create("swapspace-swaps");
  fake_mkdirs("swap");
  snprintf(sysroot, sizeof(sysroot), "%s/sys", fake_dir);
  snprintf(procpath, sizeof(procpath), "%s/proc_swaps", fake_dir);
  crypt_sysfs_root = sysroot;

  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 2; }
  if (chdir(fake_path("swap")) != 0) { perror("chdir"); return 2; }
  snprintf(swappath, sizeof(swappath), "%s", fake_path("swap"));
  swappath_len = strlen(swappath);

  min_swapsize = 4 * page();
  max_swapsize = 256 * page();

  test_alloc_crypt();
  test_retire_plain();
  test_plain_einval();
  test_dump_stats();
  test_dump_stats_crypt();
  test_retire();
  test_adoption();
  test_startup_plain();
  test_startup();

  if (chdir(cwd) != 0) perror("chdir back");
  fake_dir_remove();

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
