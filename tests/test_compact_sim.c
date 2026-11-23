/* Root-free simulation of swapfile compaction.
 *
 * This drives compact_limits() through sequences of allocations and
 * retirements the way alloc_swapfile() and free_swapfile() do, with a model
 * disk in place of real swapfiles.  It checks the arithmetic and the policy
 * (files stay within the slot limit for a capacity target, sizes step down
 * again after retirement).  It does not touch swapon, mkswap, the filesystem
 * or the kernel; see tests/root-integration.sh for that.
 *
 * Run with -v to print the capacity reachable with each number of files.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/calc.c"

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

/// Mirror of MAX_SWAPFILES in src/swaps.h
#define SLOTS 32
/// A realistic kernel limit: 32 minus reserved entries and other swap areas
#define KSLOTS 24

struct config
{
  int threshold, factor;
  memsize_t cap, minsize, maxsize;
};

struct model
{
  memsize_t size[SLOTS];
  int active;
  memsize_t capacity;
};

/// Free disk space that never limits a file
#define NOLIMIT LLONG_MAX

/// Size of a new file for a request, as alloc_swapfile() would choose it
/** Zero if it does not fit in free_space. */
static memsize_t new_size(const struct config *c, const struct model *m,
    memsize_t request, memsize_t free_space)
{
  memsize_t cmin, cmax, omin, omax;
  compact_limits(m->active, c->threshold, c->factor, c->cap,
      c->minsize, c->maxsize, &cmin, &cmax);
  return choose_swapsize(request, c->minsize, c->maxsize, cmin, cmax,
      free_space, &omin, &omax);
}

static void add_file(struct model *m, memsize_t size)
{
  m->size[m->active++] = size;
  m->capacity += size;
}

/// Retire the newest file
static memsize_t drop_file(struct model *m)
{
  const memsize_t size = m->size[--m->active];
  m->capacity -= size;
  return size;
}

/// Allocate files for a worst-case request (always the largest allowed)
/** @return number of files needed to reach target, or SLOTS+1 if the slots ran
 * out first
 */
static int files_to_reach(const struct config *c, struct model *m,
    memsize_t target, int slots)
{
  memset(m, 0, sizeof(*m));
  while (m->capacity < target)
  {
    if (m->active >= slots) return slots + 1;
    add_file(m, new_size(c, m, c->maxsize * 64, NOLIMIT));
  }
  return m->active;
}

int main(int argc, char *argv[])
{
  const int verbose = argc > 1 && !strcmp(argv[1], "-v");
  const memsize_t page = getpagesize();
  struct model m;

  // The user's setup: 1 GiB files, compaction from the 17th file on.
  const struct config user = { 16, 2, 16*GIGA, 4*MEGA, GIGA };
  const struct config off = { 0, 2, 16*GIGA, 4*MEGA, GIGA };

  // Capacity reachable with n files, in GiB.
  memsize_t reach[SLOTS+1] = { 0 }, plain[SLOTS+1] = { 0 };
  struct model a, b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  for (int n=1; n<=SLOTS; ++n)
  {
    add_file(&a, new_size(&user, &a, user.maxsize * 64, NOLIMIT));
    add_file(&b, new_size(&off, &b, off.maxsize * 64, NOLIMIT));
    reach[n] = a.capacity;
    plain[n] = b.capacity;
    if (verbose)
      printf("files %2d: %4lld GiB with compaction, %4lld GiB without\n",
	  n, reach[n]/GIGA, plain[n]/GIGA);
  }

  // Files 1..16 are normal; the 17th is 2 GiB, then 4, 8 and 16 GiB.
  CHECK(reach[16] == 16*GIGA);
  CHECK(reach[17] == 18*GIGA);
  CHECK(reach[18] == 22*GIGA);
  CHECK(reach[19] == 30*GIGA);
  CHECK(reach[20] == 46*GIGA);
  CHECK(reach[32] == 238*GIGA);
  CHECK(plain[32] == 32*GIGA);
  for (int n=1; n<=SLOTS; ++n) CHECK(reach[n] >= plain[n]);

  // A 64 GiB target needs more files than the slots without compaction and
  // fits with it, also when the kernel leaves only KSLOTS of them.
  CHECK(files_to_reach(&off, &m, 64*GIGA, SLOTS) == SLOTS + 1);
  CHECK(files_to_reach(&off, &m, 64*GIGA, KSLOTS) == KSLOTS + 1);
  CHECK(files_to_reach(&user, &m, 64*GIGA, SLOTS) == 22);
  CHECK(files_to_reach(&user, &m, 64*GIGA, KSLOTS) == 22);
  // Targets within the normal capacity are unaffected.
  CHECK(files_to_reach(&user, &m, 16*GIGA, SLOTS) == 16);
  CHECK(files_to_reach(&off, &m, 16*GIGA, SLOTS) == 16);

  // For every target up to the compacted capacity the file count stays within
  // the slots, in steps of 1 GiB.
  for (memsize_t t=GIGA; t<=reach[SLOTS]; t+=GIGA)
    CHECK(files_to_reach(&user, &m, t, SLOTS) <= SLOTS);
  // Beyond it, compaction cannot help: it does not create slots.
  CHECK(files_to_reach(&user, &m, reach[SLOTS] + page, SLOTS) == SLOTS + 1);

  // A lower threshold, as for a machine where zram takes a few slots.
  const struct config lowthr = { 12, 2, 16*GIGA, 4*MEGA, GIGA };
  CHECK(files_to_reach(&lowthr, &m, 64*GIGA, SLOTS) == 18);

  // Small requests: every file is at least the effective minimum, so files
  // grow with the level even when little swap is needed, up to the cap.
  memset(&m, 0, sizeof(m));
  for (int i=0; i<31; ++i) add_file(&m, new_size(&user, &m, page, NOLIMIT));
  CHECK(m.size[0] == user.minsize);
  CHECK(m.size[15] == user.minsize);
  CHECK(m.size[16] == 2*user.minsize);
  CHECK(m.size[17] == 4*user.minsize);
  CHECK(m.size[22] == 128*user.minsize);
  CHECK(m.size[30] == user.cap);

  // Retirement steps the size back down, and the mode is stateless: growing
  // again after shrinking gives the same sizes as the first time.
  memset(&m, 0, sizeof(m));
  memsize_t first[SLOTS];
  for (int i=0; i<SLOTS; ++i)
  {
    first[i] = new_size(&user, &m, user.maxsize * 64, NOLIMIT);
    add_file(&m, first[i]);
  }
  for (int i=SLOTS-1; i>=15; --i)
    CHECK(drop_file(&m) == first[i]);
  CHECK(m.active == 15);
  CHECK(new_size(&user, &m, user.maxsize * 64, NOLIMIT) == user.maxsize);
  for (int i=15; i<SLOTS; ++i)
  {
    const memsize_t s = new_size(&user, &m, user.maxsize * 64, NOLIMIT);
    CHECK(s == first[i]);
    add_file(&m, s);
  }

  // Tight disk: a compacted file that does not fit falls back to the
  // configured maximum instead of failing.  At 20 files the 21st may be 16 GiB.
  memset(&m, 0, sizeof(m));
  for (int i=0; i<20; ++i) add_file(&m, new_size(&user, &m, GIGA, NOLIMIT));
  CHECK(new_size(&user, &m, 64*GIGA, 100*GIGA) == user.cap);
  CHECK(new_size(&user, &m, 64*GIGA, 3*GIGA) == user.maxsize);
  CHECK(new_size(&user, &m, 64*GIGA, user.maxsize - page) == 0);

  // Pseudo-random allocation and retirement: the size of each new file follows
  // from the current file count alone.
  unsigned long long seed = 12345;
  memset(&m, 0, sizeof(m));
  for (int step=0; step<20000; ++step)
  {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const unsigned r = (unsigned)(seed >> 33) % 100;
    if (r < 55 && m.active < SLOTS - 1)
    {
      const memsize_t size = new_size(&user, &m, user.maxsize * 64, NOLIMIT);
      // Expected size worked out by hand: doubling per file from the 17th.
      memsize_t expect = GIGA;
      for (int k=15; k<m.active && expect < 16*GIGA; ++k) expect *= 2;
      CHECK(size == expect);
      add_file(&m, size);
    }
    else if (m.active > 0) drop_file(&m);
    CHECK(m.active <= SLOTS);
  }

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
