/*
This file is part of Swapspace.

Copyright (C) 2005,2006, Software Industry Industry Promotion Agency (SIPA)
Copyright (C) 2010, Jeroen T. Vermeulen
Written by Jeroen T. Vermeulen.

Swapspace is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Swapspace is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with swapspace; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/
#include "env.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "calc.h"
#include "log.h"
#include "memory.h"
#include "opts.h"
#include "support.h"


/// Lower bound to percentage of memory/swap space kept available
static int lower_freelimit=20;
/// Upper bound to percentage of memory/swap space kept available
static int upper_freelimit=60;

/// Configuration item: target percentage of available space after adding swap
static int freetarget=30;

/// Configuration item: what percentage of buffer space do we consider "free"?
static int buffer_elasticity=30;

// TODO: Make this adaptive based on actual cache usage, to avoid thrashing?
// TODO: Any way of detecting actual cache flexibility or minimum size?
/// Configuration item: what percentage of cache space do we consider "free"?
static int cache_elasticity=80;

/// Configuration item: PSI "some avg10" pressure threshold in percent
/** Zero disables PSI-based allocation triggers. */
static int psi_threshold=10;

// Track whether MemAvailable is present in /proc/meminfo
static bool kernel_mem_available = false;

#ifndef NO_CONFIG
char *set_freetarget(long long pct)
{
  freetarget = (int)pct;
  return NULL;
}
char *set_lower_freelimit(long long pct)
{
  lower_freelimit = (int)pct;
  return NULL;
}
char *set_upper_freelimit(long long pct)
{
  upper_freelimit = (int)pct;
  return NULL;
}
char *set_buffer_elasticity(long long pct)
{
  buffer_elasticity = (int)pct;
  return NULL;
}
char *set_cache_elasticity(long long pct)
{
  cache_elasticity = (int)pct;
  return NULL;
}
char *set_psi_threshold(long long pct)
{
  psi_threshold = (int)pct;
  return NULL;
}

bool memory_check_config(void)
{
  CHECK_CONFIG_ERR(lower_freelimit > upper_freelimit);
  CHECK_CONFIG_ERR(freetarget < lower_freelimit);
  CHECK_CONFIG_ERR(freetarget > upper_freelimit);
  return true;
}
#endif


struct meminfo_item
{
  char entry[200];
  memsize_t value;
};


/// Read and parse next line from /proc/meminfo.  Clobbers localbuf.
/** Attempt to read a line from /proc/meminfo into result.  If an error occurs,
 * result will hold errno (which may be zero, e.g. in the case of a parse error)
 * and a generic error string.  If end-of-file has been reached or a meaningless
 * line has been read, result will contain zero and the empty string.
 *
 * @return success (not EOF and not error)
 */
static bool read_meminfo_item(FILE *fp, struct meminfo_item *result)
{
  result->entry[0] = '\0';
  result->value = 0;
  if (unlikely(!fgets(localbuf, sizeof(localbuf), fp)))
  {
    if (unlikely(ferror(fp))) result->value = (memsize_t)errno;
    if (unlikely(result->value))
      strcpy(result->entry, "Error reading /proc/meminfo");
    return false;
  }

  char fact[20];
  const int x = sscanf(localbuf,
      "%199[^:]: %lld %19s",
      result->entry,
      &result->value,
      fact);

  if (unlikely(x == 2))
  {
    // Since Linux 2.6.18-mm4 or so, /proc/meminfo may contain lines without a
    // unit or "factor" attached.
    fact[0] = '\0';
  }
  else if (unlikely(x < 2))
  {
    result->value = 0;

    // In Linux 2.4, /proc/meminfo starts with a header line with leading
    // whitespace.  We accept that as a special case.
    if (likely(isspace(localbuf[0])))
    {
      result->entry[0] = '\0';
      return true;
    }

    // All other parse failures are fatal.
    snprintf(result->entry,
	  sizeof(result->entry),
	  "Parse error in /proc/meminfo: '%150s'",
	  localbuf);

    return false;
  }

  // Another special case for Linux 2.4: two extra lines summarize the various
  // pools of physical memory and swap space.  We'd fail to parse these lines
  // (which we're not interested in anyway) because there's a number where we
  // expect a scale factor.  Detect this and skip those lines.
  if (isdigit(fact[0]))
  {
    result->entry[0] = '\0';
    result->value = 0;
    return true;
  }

  // Parse scale factor at end of line.  I've never seen it be anything other
  // than "kB", but who knows what can happen...
  memsize_t scale = 0;
  const size_t factlen = strlen(fact);
  switch (factlen)
  {
  case 0:
    scale = 1;
    break;
  case 3:
    if (fact[1] != 'i') break;
    // FALL-THROUGH
  case 2:
    if (fact[factlen-1] != 'B') break;
    // FALL-THROUGH
  case 1:
    switch (tolower(fact[0]))
    {
    case 'b': scale = 1;    break;
    case 'k': scale = KILO; break;
    case 'm': scale = MEGA; break;
    case 'g': scale = GIGA; break;
    }
    break;
  }

  if (unlikely(!scale))
  {
    result->value = 0;
    snprintf(result->entry,
	sizeof(result->entry),
	"Unknown scale factor in /proc/meminfo: %s",
	fact);
    return false;
  }

  result->value *= scale;
  return true;
}


struct memstate
{
  memsize_t MemTotal,
	    MemFree,
	    MemAvailable,
	    Buffers,
	    Cached,
	    Dirty,
	    Writeback,
	    SwapCached,
	    SwapTotal,
	    SwapFree,
	    Shmem;
  /// Are MemTotal/MemFree/MemAvailable derived from a cgroup limit?
  bool cgroup_limited;
};


static void imbibe_meminfo_entry(const struct meminfo_item *inf,
    struct memstate *st)
{
  switch (inf->entry[0])
  {
  case 'B':
    if (strcmp(inf->entry, "Buffers")==0)         st->Buffers = inf->value;
    break;
  case 'C':
    if (strcmp(inf->entry, "Cached")==0)          st->Cached = inf->value;
    break;
  case 'D':
    if (strcmp(inf->entry, "Dirty")==0)           st->Dirty = inf->value;
    break;
  case 'M':
    if (strncmp(inf->entry,"Mem",3) == 0)
    {
      if (strcmp(inf->entry+3, "Total")==0)       st->MemTotal = inf->value;
      else if (strcmp(inf->entry+3, "Free")==0)   st->MemFree = inf->value;
      else if (strcmp(inf->entry+3, "Available")==0)
      {
        kernel_mem_available = true;
        st->MemAvailable = inf->value;
      }
    }
    break;
  case 'S':
    if (strncmp(inf->entry,"Swap",4) == 0)
    {
      if (strcmp(inf->entry+4, "Total")==0)       st->SwapTotal = inf->value;
      else if (strcmp(inf->entry+4, "Free")==0)   st->SwapFree = inf->value;
      else if (strcmp(inf->entry+4, "Cached")==0) st->SwapCached = inf->value;
    }
    if (strcmp(inf->entry,"Shmem")==0)            st->Shmem = inf->value;
    break;
  case 'W':
    if (strcmp(inf->entry,"Writeback")==0)        st->Writeback = inf->value;
    break;
  }
}


#ifdef FUZZING
int fuzz_meminfo_line(const uint8_t data[], size_t size)
{
  FILE *fp = fmemopen((void *)data, size, "r");
  if (unlikely(!fp)) return 0;

  struct memstate st = { 0 };
  struct meminfo_item inf;
  while (read_meminfo_item(fp, &inf)) imbibe_meminfo_entry(&inf, &st);
  fclose(fp);

  return 0;
}
#endif


/// Join a cgroup directory and a control-file name
static bool join_path(char out[], size_t size,
    const char dir[], const char name[])
{
  const int len = snprintf(out, size, "%s/%s", dir, name);
  return len >= 0 && (size_t)len < size;
}


/// Read a value from a cgroup control file
/** Returns false for missing files, "max", and cgroup v1's "unlimited"
 * sentinel, which is a page-aligned value close to LLONG_MAX.
 */
static bool read_cgroup_value(const char path[], memsize_t *value)
{
  char buf[64];
  FILE *fp = fopen(path, "r");
  if (unlikely(!fp)) return false;

  const bool ok = (fgets(buf, sizeof(buf), fp) != NULL) &&
		  parse_cgroup_limit(buf, value);
  fclose(fp);

  return ok && *value < LLONG_MAX/2;
}


/// Locate this process's memory cgroup directory
/** @param version receives 2 for cgroup v2, or 1 for v1 */
static bool cgroup_dir(char base[], size_t size, int *version)
{
  FILE *fp = fopen("/proc/self/cgroup", "r");
  if (unlikely(!fp)) return false;

  bool found = false;
  while (fgets(localbuf, sizeof(localbuf), fp))
  {
    // cgroup v2: "0::/path"
    if (strncmp(localbuf, "0::", 3) == 0)
    {
      char *path = localbuf + 3;
      path[strcspn(path, "\n")] = '\0';
      if (path[0] == '/' && path[1] == '\0') path[0] = '\0';
      const int len = snprintf(base, size, "/sys/fs/cgroup%s", path);
      if (len >= 0 && (size_t)len < size)
      {
	*version = 2;
	found = true;
      }
      break;
    }

    // cgroup v1: "N:memory:/path" (the controller list may be comma-separated)
    char *first = strchr(localbuf, ':');
    if (!first) continue;
    char *second = strchr(first + 1, ':');
    if (!second) continue;
    *second = '\0';
    if (!strstr(first + 1, "memory")) continue;

    char *path = second + 1;
    path[strcspn(path, "\n")] = '\0';
    if (path[0] == '/' && path[1] == '\0') path[0] = '\0';
    const int len = snprintf(base, size, "/sys/fs/cgroup/memory%s", path);
    if (len >= 0 && (size_t)len < size)
    {
      *version = 1;
      found = true;
    }
    break;
  }

  fclose(fp);
  return found;
}


/// Prefer cgroup limits over host-wide /proc/meminfo figures
/** Containers see the host's memory in /proc/meminfo, which makes swapspace
 * overestimate how much swap is reasonable.  When a cgroup memory limit is in
 * effect, use it as the total and derive free space from memory.current.
 */
static void apply_cgroup_limits(struct memstate *s)
{
  char dir[PATH_MAX];
  int version = 0;
  if (!cgroup_dir(dir, sizeof(dir), &version)) return;

  char path[PATH_MAX];
  memsize_t limit, current;
  const char *limitname =
    (version == 2) ? "memory.max" : "memory.limit_in_bytes";
  const char *currentname =
    (version == 2) ? "memory.current" : "memory.usage_in_bytes";

  if (!join_path(path, sizeof(path), dir, limitname) ||
      !read_cgroup_value(path, &limit))
    return;
  if (!join_path(path, sizeof(path), dir, currentname) ||
      !read_cgroup_value(path, &current))
    return;

  // Ignore limits that do not actually constrain us.
  if (limit == 0 || limit >= s->MemTotal) return;

  s->MemTotal = limit;
  s->MemFree = (current < limit) ? limit - current : 0;
  s->MemAvailable = s->MemFree;
  s->cgroup_limited = true;

  /* Host-wide cache and buffer figures do not describe this cgroup.  They are
   * not used by space_free() once cgroup_limited is set, but leaving them in
   * place would make diagnostics misleading.
   */
  s->Cached = s->Buffers = s->Dirty = s->Writeback = 0;
  s->Shmem = s->SwapCached = 0;

  // Use the cgroup's own swap accounting where available.  Otherwise keep the
  // host-wide swap figures from /proc/meminfo.
  if (version == 2)
  {
    memsize_t swaplimit, swapcurrent;
    if (join_path(path, sizeof(path), dir, "memory.swap.max") &&
	read_cgroup_value(path, &swaplimit) &&
	join_path(path, sizeof(path), dir, "memory.swap.current") &&
	read_cgroup_value(path, &swapcurrent))
    {
      s->SwapTotal = swaplimit;
      s->SwapFree = (swapcurrent < swaplimit) ? swaplimit - swapcurrent : 0;
    }
  }
  else
  {
    // cgroup v1 accounts memory and swap together in memsw.
    memsize_t memswlimit, memswcurrent;
    if (join_path(path, sizeof(path), dir, "memory.memsw.limit_in_bytes") &&
	read_cgroup_value(path, &memswlimit) &&
	join_path(path, sizeof(path), dir, "memory.memsw.usage_in_bytes") &&
	read_cgroup_value(path, &memswcurrent) &&
	memswlimit > limit)
    {
      s->SwapTotal = memswlimit - limit;
      const memsize_t swapused =
	(memswcurrent > current) ? memswcurrent - current : 0;
      s->SwapFree = (swapused < s->SwapTotal) ? s->SwapTotal - swapused : 0;
    }
  }
}


static bool read_proc_meminfo(struct memstate *s)
{
  FILE *fp = fopen("/proc/meminfo", "r");
  if (unlikely(!fp))
  {
    log_perr(LOG_ERR, "Could not open /proc/meminfo for reading", errno);
    return false;
  }

  struct meminfo_item inf;
  while (read_meminfo_item(fp, &inf)) imbibe_meminfo_entry(&inf, s);
  fclose(fp);
  if (unlikely(inf.entry[0]))
  {
    log_perr(LOG_ERR, inf.entry, (int)inf.value);
    return false;
  }

  if (unlikely(!s->MemTotal))
  {
    logm(LOG_ERR,
	"No memory detected! Perhaps /proc/meminfo is in an unexpected format");
    return false;
  }
  if (unlikely(s->MemTotal < s->MemFree+s->Buffers+s->Cached+s->SwapCached))
  {
    logm(LOG_ERR, "Memory statistics read from /proc/meminfo don't add up");
    return false;
  }

  apply_cgroup_limits(s);

  return true;
}


bool check_memory_status(void)
{
  const memsize_t init_req = memory_target();
  if (unlikely(init_req == MEMSIZE_ERROR)) return false;
#ifndef NO_CONFIG
  if (init_req && !quiet)
  {
    printf("Initial memory status: ");
    if (init_req > 0)
      logm(LOG_INFO, "would prefer %lld extra bytes", init_req);
    else
      logm(LOG_INFO, "%lld bytes to spare", -init_req);
  }
#endif
  return true;
}


/// How much buffer space can we expect the system to free up?
static inline memsize_t buffers_free(const struct memstate *st)
{
  return (st->Buffers/100) * buffer_elasticity;
}


/// How much cache space can we expect the system to free up?
static inline memsize_t cache_free(const struct memstate *st)
{
  const memsize_t cache = st->Cached - (st->Dirty + st->Writeback + st->Shmem);
  return (cache > 0) ? (cache/100)*cache_elasticity : 0;
}


static inline memsize_t space_free(const struct memstate *st)
{
  /* Estimating "free" memory is not only hard, but subjective as well.  Ideally
   * we'd want some measure of "pressure"--which we don't seem to have.
   *
   * The big question is what to do about Cached.  This is where most bytes tend
   * to go, and it grows unless and until memory requirements get really, really
   * urgent.  In other words, it doesn't behave as if it's something very
   * flexible that we could treat as essentially "free" space.
   *
   * What if we subtracted Dirty and Writeback from this number?  Writeback
   * appears to be a very rare state, but Dirty can get quite large when the
   * system is pressed for memory.  Would the remainder be more or less
   * flexible?  For some reason I get more flexibility now, even without having
   * a noticeable number of dirty pages, on a 2.6.10 kernel than I observed in
   * the past.  Has something changed here?
   *
   * I managed to push back Cached all the way to about half a percent of total
   * physical memory before the system started to thrash.  And that was with a
   * large number of applications loaded and large amounts of memory allocated
   * and committed by applications.
   *
   * Under these circumstances, best thing to do is subtract Dirty/Writeback
   * from Cached and "discount" the remainder against a new parameter ("Cache
   * Pressure") which says how much of the non-dirty part of Cache should be
   * considered in-use.
   */
  if (st->cgroup_limited || kernel_mem_available) {
    return st->MemAvailable + st->SwapFree;
  } else {
    return st->MemFree +
      st->SwapFree +
      st->SwapCached +
      buffers_free(st) +
      cache_free(st);
  }
}

static inline memsize_t space_total(const struct memstate *st)
{
  return st->MemTotal + st->SwapTotal;
}

static inline int pct_free(const struct memstate *st)
{
  // Phrased oddly to avoid integer overflows
  const memsize_t hundredth = space_total(st)/100;
  return hundredth ? (int)(space_free(st)/hundredth) : 0;
}


/// Read the "some avg10" memory pressure percentage from PSI
/** Returns false if PSI is unavailable (kernel before 4.20, or no /proc). */
static bool read_psi(int *percent)
{
  FILE *fp = fopen("/proc/pressure/memory", "r");
  if (unlikely(!fp)) return false;

  bool found = false;
  while (fgets(localbuf, sizeof(localbuf), fp))
  {
    if (parse_psi_line(localbuf, percent))
    {
      found = true;
      break;
    }
  }
  fclose(fp);

  return found;
}


memsize_t memory_target(void)
{
  /* Determining how much memory we need is a pretty difficult job.  One reason
   * is that if no swap space is available under Linux 2.6, lots of pages stay
   * marked as "cached."  So when /proc/meminfo says that a large portion of
   * physical memory is in use as cache, that doesn't mean that memory can be
   * made available for other uses!
   */

  assert(upper_freelimit > lower_freelimit);

  struct memstate st = { 0 };
  if (unlikely(!read_proc_meminfo(&st))) return MEMSIZE_ERROR;

  const int freepct = pct_free(&st);
  memsize_t request = 0;

  /* Ask the kernel whether tasks are actually stalling.  PSI is a much more
   * direct signal than our free-memory estimate, so under pressure we steer
   * towards freetarget even before lower_freelimit is reached.
   */
  int psi = 0;
  const bool pressure =
    (psi_threshold > 0) && read_psi(&psi) && (psi >= psi_threshold);

  // Policy: once we hit either freelimit, steer for "freetarget"% free space
  // TODO: Use /proc/sys/vm/min_free_kbytes when no swap yet allocated?
  if (freepct < lower_freelimit || freepct > upper_freelimit ||
      (pressure && freepct <= freetarget))
    request = ideal_swapsize(space_total(&st), space_free(&st), freetarget);

  // Freeing swap while tasks are stalling would only make things worse.
  if (pressure && request < 0) request = 0;

  return request;
}


memsize_t minimal_swapfile(void)
{
  struct memstate st = { 0 };
  if (unlikely(!read_proc_meminfo(&st))) return MEMSIZE_ERROR;

  const memsize_t total = space_total(&st);
  // Compute desired swapfile size if we're exactly at lower_freelimit
  return ideal_swapsize(total, (total/100)*lower_freelimit, freetarget);
}


static void dump_memline(const char category[],
    long long total,
    long long free,
    long long cached)
{
  logm(LOG_INFO,
      "%s: %lld total, %lld free (%lld used); %lld cached",
      category,
      total,
      free,
      (total - free),
      cached);
}

void dump_memory(void)
{
  struct memstate st = { 0 };
  if (unlikely(!read_proc_meminfo(&st))) return;
  check_memory_status();

  if (st.cgroup_limited)
    logm(LOG_INFO, "cgroup memory limit in effect");

  dump_memline("core", st.MemTotal, st.MemFree, st.Cached);
  dump_memline("swap", st.SwapTotal, st.SwapFree, st.SwapCached);
  dump_memline("total",
      space_total(&st),
      st.MemFree + st.SwapFree,
      st.Cached + st.SwapCached);

  logm(LOG_INFO,
      "bufs: %lld, dirty: %lld, writeback: %lld",
      st.Buffers,
      st.Dirty,
      st.Writeback);

  const int pf = pct_free(&st);
  logm(LOG_INFO,
      "estimate free: %lld cache, %lld bufs, %lld total (%d%%)",
      cache_free(&st),
      buffers_free(&st),
      space_free(&st),
      pf);
  logm(LOG_INFO,
      "thresholds: %d%% < %d%% < %d%%",
      lower_freelimit,
      pf,
      upper_freelimit);

  int psi = 0;
  if (read_psi(&psi))
    logm(LOG_INFO,
	"memory pressure: %d%% (some avg10, threshold %d%%)",
	psi, psi_threshold);
}
