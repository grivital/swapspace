/* Root-free tests of how swaps.c treats names it finds in the swap directory.
 *
 * swaps.c is included directly, with swapon/mkswap/unlink/fopen replaced, so
 * the real activate_old_swaps(), read_proc_swaps() and alloc_swapfile() run
 * against a temporary directory and a fake /proc/swaps.  Nothing outside the
 * temporary directory is touched and no swap area is ever enabled.
 */
#include "env.h"

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static FILE *test_fopen(const char *path, const char *mode);
static int test_unlink(const char *path);

#define fopen test_fopen
#define unlink test_unlink
#include "../src/swaps.c"
#undef fopen
#undef unlink

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

// Symbols that swaps.c expects from the rest of the daemon.
char localbuf[16384];
bool quiet = true;
bool verbose = false;
time_t runclock = 0;
static int diets = 0;
void request_diet(void) { ++diets; }
void dump_memory(void) {}
void dump_state(void) {}
int runcommand(const char cmd[], const char arg[])
{
  (void)cmd; (void)arg;
  return 0;
}
int swapoff(const char *path) { (void)path; return 0; }

/* Names for which swapon() reports EBUSY, swapon() fails once with EINVAL, and
 * unlink() fails with stuck_errno.
 */
static const char *busy_name = NULL;
static const char *einval_name = NULL;
static const char *stuck_name = NULL;
static int stuck_errno = EPERM;
static char fake_proc[PATH_MAX];

int swapon(const char *path, int flags)
{
  (void)flags;
  if (busy_name && strcmp(path, busy_name) == 0) { errno = EBUSY; return -1; }
  if (einval_name && strcmp(path, einval_name) == 0)
  {
    einval_name = NULL;
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int test_unlink(const char *path)
{
  if (stuck_name && strcmp(path, stuck_name) == 0)
  {
    // ENOENT models a file that someone else removed between scan and unlink.
    if (stuck_errno == ENOENT) unlink(path);
    errno = stuck_errno;
    return -1;
  }
  return unlink(path);
}

static FILE *test_fopen(const char *path, const char *mode)
{
  if (strcmp(path, "/proc/swaps") == 0) path = fake_proc;
  return (fopen)(path, mode);
}


static char root[PATH_MAX];

static void remove_tree(const char *path)
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
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        remove_tree(sub);
      }
      closedir(d);
    }
    rmdir(path);
  }
  else unlink(path);
}

/* Cleanup runs from atexit only: an assert or sanitizer abort leaves the small
 * directory behind, and a signal handler that walks the tree is not worth the
 * risk of masking the real failure.
 */
static void cleanup(void)
{
  if (chdir("/") == 0 && root[0]) remove_tree(root);
}

static void write_proc(const char *entry)
{
  FILE *f = (fopen)(fake_proc, "w");
  if (!f) { perror("fake /proc/swaps"); exit(2); }
  fputs("Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n", f);
  if (entry) fprintf(f, "%s/%s file 4096 0 -2\n", swappath, entry);
  fclose(f);
}

/// Enter a fresh swap directory with a clean slot table
static void new_swapdir(const char *name)
{
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", root, name);
  if (mkdir(path, 0700) != 0) { perror("mkdir"); exit(2); }
  strcpy(swappath, path);
  if (!to_swapdir()) { fprintf(stderr, "to_swapdir failed\n"); exit(2); }
  memset(swapfiles, 0, sizeof(swapfiles));
  sequence_number = 0;
  busy_name = einval_name = stuck_name = NULL;
  stuck_errno = EPERM;
  write_proc(NULL);
}

static void make_file(const char *name, const char *content, off_t size)
{
  int fd = open(name, O_WRONLY|O_CREAT|O_EXCL, 0600);
  if (fd == -1 || write(fd, content, strlen(content)) < 0 ||
      ftruncate(fd, size) != 0)
  { perror(name); exit(2); }
  close(fd);
}

static void start_up(void)
{
  CHECK(read_proc_swaps());
  CHECK(activate_old_swaps());
}

/// Is name still the same kind of entry it was, with unchanged contents?
static bool untouched(const char *name, mode_t type, const char *content)
{
  struct stat st;
  if (lstat(name, &st) != 0 || (st.st_mode & S_IFMT) != type) return false;
  if (!content) return true;
  char buf[8] = "";
  int fd = open(name, O_RDONLY);
  const ssize_t n = fd == -1 ? -1 : read(fd, buf, strlen(content));
  if (fd != -1) close(fd);
  return n == (ssize_t)strlen(content) && strncmp(buf, content, n) == 0;
}

static bool exists(const char *name)
{
  struct stat st;
  return lstat(name, &st) == 0;
}


int main(void)
{
  const long long page = getpagesize();
  min_swapsize = 16*page;
  max_swapsize = 64*page;

  snprintf(root, sizeof(root), "%s/swapspace-test.XXXXXX",
      getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
  atexit(cleanup);
  for (const char *c = root; *c; ++c)
    if (isspace((unsigned char)*c))
    {
      fprintf(stderr, "SKIP: swap paths with whitespace are unsupported\n");
      return 77;
    }
  snprintf(fake_proc, sizeof(fake_proc), "%s/proc-swaps", root);

  /* A busy swapfile, a FIFO, a directory and a dangling symlink: none of them
   * may be chosen, replaced or removed by allocation.
   */
  new_swapdir("names");
  make_file("1", "BUSY", 32*page);
  mkfifo("2", 0600);
  mkdir("3", 0700);
  symlink("no-such-target", "4");
  busy_name = "1";
  start_up();
  for (int round = 0; round < 6; ++round)
  {
    const int before = diets;
    CHECK(alloc_swapfile(4*page));
    CHECK(diets == before);
  }
  for (int i = 1; i <= 4; ++i) CHECK(swapfiles[i].size == 0);
  for (int i = 5; i <= 10; ++i) CHECK(swapfiles[i].size > 0);
  CHECK(untouched("1", S_IFREG, "BUSY"));
  CHECK(untouched("2", S_IFIFO, NULL));
  CHECK(untouched("3", S_IFDIR, NULL));
  CHECK(untouched("4", S_IFLNK, NULL));

  /* The busy file shows up in /proc/swaps: it is adopted like any other, and
   * once it is gone again its slot can be reused.
   */
  busy_name = NULL;
  write_proc("1");
  CHECK(read_proc_swaps());
  CHECK(swapfiles[1].size == 4096LL*1024);
  write_proc(NULL);
  CHECK(read_proc_swaps());
  CHECK(swapfiles[1].size == 0);
  sequence_number = 0;
  CHECK(alloc_swapfile(4*page));
  CHECK(swapfiles[1].size > 0);

  // A file that cannot be removed keeps its slot, and nothing wedges on it.
  new_swapdir("stuck");
  make_file("1", "x", 1);
  stuck_name = "1";
  start_up();
  for (int round = 0; round < 3; ++round)
  {
    const int before = diets;
    CHECK(alloc_swapfile(4*page));
    CHECK(diets == before);
  }
  CHECK(swapfiles[1].size == 0);
  CHECK(untouched("1", S_IFREG, "x"));

  // A file that vanished before the scan could unlink it leaves a free slot.
  new_swapdir("gone");
  make_file("1", "x", 1);
  stuck_name = "1";
  stuck_errno = ENOENT;
  start_up();
  CHECK(alloc_swapfile(4*page));
  CHECK(swapfiles[1].size > 0);

  /* If swapon() rejects a fallocate()d file, allocation retries without it in
   * the same slot, which must not be left blocked.
   */
  new_swapdir("einval");
  einval_name = "1";
  start_up();
  const int diets_before = diets;
  CHECK(pfalloc_ok);
  CHECK(alloc_swapfile(4*page));
  CHECK(!pfalloc_ok);
  CHECK(swapfiles[1].size > 0);
  CHECK(diets == diets_before);

  /* Every slot taken: report it and request a diet, without touching the
   * sentinel slot or creating a file for it.
   */
  new_swapdir("full");
  for (int i = 0; i < MAX_SWAPFILES; ++i)
  {
    char name[8];
    snprintf(name, sizeof(name), "%d", i);
    mkdir(name, 0700);
  }
  start_up();
  const int before = diets;
  CHECK(!alloc_swapfile(4*page));
  CHECK(diets == before + 1);
  CHECK(!exists("32"));
  CHECK(swapfiles[MAX_SWAPFILES].size == 0);
  for (int i = 0; i < MAX_SWAPFILES; ++i)
  {
    char name[8];
    snprintf(name, sizeof(name), "%d", i);
    CHECK(untouched(name, S_IFDIR, NULL));
  }

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
