/* Unit tests for the pure sizing arithmetic in src/calc.c.
 *
 * calc.c is included directly so that the tests exercise exactly the code that
 * goes into the daemon without exposing an otherwise internal API.
 */
#include <stdio.h>
#include <sys/param.h>
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


int main(void)
{
  const memsize_t page = getpagesize();

  // Page rounding.
  CHECK(trunc_to_page(0) == 0);
  CHECK(trunc_to_page(page-1) == 0);
  CHECK(trunc_to_page(page) == page);
  CHECK(trunc_to_page(page+1) == page);
  CHECK(ext_to_page(0) == 0);
  CHECK(ext_to_page(1) == page);
  CHECK(ext_to_page(page) == page);
  CHECK(ext_to_page(page+1) == 2*page);
  CHECK(ext_to_page(2*page-1) == 2*page);
  CHECK(ext_to_page(2*page) == 2*page);
  CHECK(ext_to_page(2*page+1) == 3*page);

  // At the target, no adjustment is needed.
  CHECK(ideal_swapsize(100000, 30000, 30) == 0);
  CHECK(ideal_swapsize(100000, 50000, 50) == 0);

  // Below the target: a positive adjustment, close to the requested target.
  const memsize_t add = ideal_swapsize(100000, 20000, 30);
  CHECK(add == 14200);
  CHECK(20000+add > 0);
  CHECK((20000+add)*100/(100000+add) >= 29);

  // Above the target: a negative adjustment.
  CHECK(ideal_swapsize(100000, 60000, 30) < 0);

  // Targets that are not the default work the same way.
  CHECK(ideal_swapsize(100000, 40000, 50) == 20000);

  // PSI line parsing.
  int psi = -1;
  CHECK(parse_psi_line(
      "some avg10=1.23 avg60=0.00 avg300=0.00 total=0", &psi));
  CHECK(psi == 1);
  CHECK(parse_psi_line(
      "some avg10=12.99 avg60=0.00 avg300=0.00 total=0", &psi));
  CHECK(psi == 12);
  CHECK(!parse_psi_line(
      "full avg10=99.00 avg60=0.00 avg300=0.00 total=0", &psi));
  CHECK(!parse_psi_line("garbage", &psi));
  CHECK(!parse_psi_line("some avg10=-1.0", &psi));
  CHECK(parse_psi_line("some avg10=150.0", &psi));
  CHECK(psi == 100);

  // cgroup limit parsing.
  memsize_t cg = 0;
  CHECK(!parse_cgroup_limit("max\n", &cg));
  CHECK(parse_cgroup_limit("123456\n", &cg) && cg == 123456);
  CHECK(parse_cgroup_limit("  4096  \n", &cg) && cg == 4096);
  CHECK(!parse_cgroup_limit("-1\n", &cg));
  CHECK(!parse_cgroup_limit("12x\n", &cg));
  CHECK(!parse_cgroup_limit("", &cg));
  // cgroup v1's "unlimited" sentinel is still a valid number; memory.c filters
  // values near LLONG_MAX.
  CHECK(parse_cgroup_limit("9223372036854771712\n", &cg));

  // Compaction level.
  CHECK(compact_level(0, 16, 2) == 0);
  CHECK(compact_level(15, 16, 2) == 0);
  CHECK(compact_level(16, 16, 2) == 1);
  CHECK(compact_level(17, 16, 2) == 2);
  CHECK(compact_level(22, 16, 2) == 7);
  CHECK(compact_level(1, 1, 2) == 1);
  CHECK(compact_level(0, 1, 2) == 0);
  CHECK(compact_level(31, 0, 2) == 0);
  CHECK(compact_level(22, 16, 1) == 0);

  // Compaction limits.
  const memsize_t mn = 4*MEGA, mx = GIGA, nocap = 0;
  memsize_t omin = -1, omax = -1;

  // Below the threshold: unchanged.
  CHECK(!compact_limits(15, 16, 2, nocap, mn, mx, &omin, &omax));
  CHECK(omin == mn && omax == mx);
  CHECK(!compact_limits(0, 16, 2, nocap, mn, mx, &omin, &omax));
  CHECK(omin == mn && omax == mx);

  // The progression doubles with every swapfile at or above the threshold.
  for (int a=16; a<=22; ++a)
  {
    CHECK(compact_limits(a, 16, 2, nocap, mn, mx, &omin, &omax));
    CHECK(omin == (mn << (a-15)) && omax == (mx << (a-15)));
  }
  // A factor of 3 triples instead.
  CHECK(compact_limits(16, 16, 3, nocap, mn, mx, &omin, &omax));
  CHECK(omin == 3*mn && omax == 3*mx);
  CHECK(compact_limits(18, 16, 3, nocap, mn, mx, &omin, &omax));
  CHECK(omin == 27*mn && omax == 27*mx);

  // Retirement: the same counts in descending order step back down, and
  // dropping below the threshold restores the normal size.
  for (int a=22; a>=16; --a)
  {
    CHECK(compact_limits(a, 16, 2, nocap, mn, mx, &omin, &omax));
    CHECK(omax == (mx << (a-15)));
  }
  CHECK(!compact_limits(15, 16, 2, nocap, mn, mx, &omin, &omax));
  CHECK(omin == mn && omax == mx);

  // The smallest threshold applies from the first swapfile on.
  CHECK(compact_limits(1, 1, 3, nocap, mn, mx, &omin, &omax));
  CHECK(omin == 3*mn && omax == 3*mx);
  CHECK(!compact_limits(0, 1, 3, nocap, mn, mx, &omin, &omax));
  CHECK(omin == mn && omax == mx);
  // Factor 1 has no effect, even above the threshold.
  CHECK(!compact_limits(21, 16, 1, 16*GIGA, mn, mx, &omin, &omax));
  CHECK(omin == mn && omax == mx);
  // Threshold 0 disables the mode, however many swapfiles there are.
  CHECK(!compact_limits(0, 0, 2, nocap, mn, mx, &omin, &omax));
  CHECK(omin == mn && omax == mx);
  CHECK(!compact_limits(31, 0, 16, 16*GIGA, mn, mx, &omin, &omax));
  CHECK(omin == mn && omax == mx);

  // The cap: 1 GiB doubled reaches 16 GiB at the fifth compacted swapfile
  // and stays there.
  const memsize_t cap16 = 16*GIGA;
  for (int a=16; a<=22; ++a)
  {
    CHECK(compact_limits(a, 16, 2, cap16, mn, mx, &omin, &omax));
    CHECK(omax == (a < 20 ? mx << (a-15) : cap16));
  }
  // The minimum keeps growing by its own progression.
  CHECK(compact_limits(22, 16, 2, cap16, mn, mx, &omin, &omax));
  CHECK(omin == (mn << 7) && omax == cap16);
  CHECK(compact_limits(16, 16, 2, cap16, mn, mx, &omin, &omax));
  CHECK(omin == 2*mn && omax == 2*mx);
  // A cap that is not page aligned is truncated; one below the configured
  // maximum never shrinks swapfiles.
  CHECK(compact_limits(17, 16, 2, 3*GIGA + 5, mn, mx, &omin, &omax));
  CHECK(omax == 3*GIGA);
  CHECK(compact_limits(17, 16, 2, mx/2, mn, mx, &omin, &omax));
  CHECK(omin == 4*mn && omax == mx);
  CHECK(compact_limits(17, 16, 2, 1, mn, mx, &omin, &omax));
  CHECK(omax == mx);

  // The minimum is bounded by the effective maximum.
  CHECK(compact_limits(16, 16, 2, 12*MEGA, 4*MEGA, 8*MEGA, &omin, &omax));
  CHECK(omin == 8*MEGA && omax == 12*MEGA);
  CHECK(compact_limits(17, 16, 2, 10*MEGA, 4*MEGA, 8*MEGA, &omin, &omax));
  CHECK(omin == 10*MEGA && omax == 10*MEGA);

  // Saturation: no overflow, result stays page aligned and monotonic.  Sizes
  // are page aligned, as they are in the daemon.
  const memsize_t cap = trunc_to_page(LLONG_MAX);
  CHECK(compact_limits(16, 16, 2, nocap, mn, cap, &omin, &omax));
  CHECK(omin == 2*mn && omax == cap);
  CHECK(compact_limits(22, 16, 16, nocap, cap, cap, &omin, &omax));
  CHECK(omin == cap && omax == cap);
  // 16^7 times 2 TiB overflows 63 bits on the way up.
  CHECK(compact_limits(22, 16, 16, nocap, mn, 2*TERA, &omin, &omax));
  CHECK(omax == cap);
  // Largest size that still fits, and one page beyond it.
  const memsize_t fits = trunc_to_page(LLONG_MAX / 2);
  CHECK(compact_limits(16, 16, 2, nocap, fits, fits, &omin, &omax));
  CHECK(omin == 2*fits && omax == 2*fits);
  CHECK(compact_limits(16, 16, 2, nocap, fits+page, fits+page, &omin, &omax));
  CHECK(omin == cap && omax == cap);
  // Two levels of doubling from a size that fits only once.
  CHECK(compact_limits(17, 16, 2, nocap, fits, fits, &omin, &omax));
  CHECK(omin == cap && omax == cap);

  // Alignment and ordering over a sweep of sizes, counts, factors and caps.
  const memsize_t sizes[] = {
    page, 2*page, 10*page, 4*MEGA, GIGA, 2*TERA,
    fits-page, fits, fits+page, cap-page, cap
  };
  const memsize_t caps[] = { 0, 1, 10*page, GIGA, 16*GIGA, cap };
  const int nsizes = sizeof(sizes)/sizeof(*sizes);
  const int ncaps = sizeof(caps)/sizeof(*caps);
  for (int a=0; a<=32; ++a)
    for (int f=1; f<=16; ++f)
      for (int c=0; c<ncaps; ++c)
	for (int i=0; i<nsizes; ++i)
	  for (int j=i; j<nsizes; ++j)
	  {
	    const bool r = compact_limits(a, 16, f, caps[c], sizes[i], sizes[j],
		&omin, &omax);
	    CHECK(r == (a >= 16 && f > 1));
	    CHECK(omin == trunc_to_page(omin));
	    CHECK(omax == trunc_to_page(omax));
	    CHECK(omin >= sizes[i] && omax >= sizes[j]);
	    CHECK(omin <= omax);
	    if (!r) CHECK(omin == sizes[i] && omax == sizes[j]);
	    else if (caps[c]) CHECK(omax <= MAX(sizes[j], trunc_to_page(caps[c])));
	  }

  // Choosing a size against the free disk space.  Level 2 of the default
  // compaction (16 active files): limits 16 MiB .. 4 GiB instead of 4 MiB ..
  // 1 GiB.
  memsize_t cmin, cmax, lmin = -1, lmax = -1;
  CHECK(compact_limits(17, 16, 2, 16*GIGA, mn, mx, &cmin, &cmax));
  CHECK(cmin == 16*MEGA && cmax == 4*GIGA);

  // The compacted size fits: it is used, with the compacted limits.
  memsize_t got = choose_swapsize(3*GIGA, mn, mx, cmin, cmax, 8*GIGA,
      &lmin, &lmax);
  CHECK(got == 3*GIGA && lmin == cmin && lmax == cmax);
  // A request above the compacted maximum is capped to it.
  got = choose_swapsize(7*GIGA, mn, mx, cmin, cmax, 8*GIGA, &lmin, &lmax);
  CHECK(got == 4*GIGA && lmin == cmin && lmax == cmax);

  // It does not fit but the configured size does: fall back to the base
  // limits, so make_swapfile() cannot raise the size above the free space.
  got = choose_swapsize(3*GIGA + GIGA/2, mn, mx, cmin, cmax, 3*GIGA,
      &lmin, &lmax);
  CHECK(got == mx && lmin == mn && lmax == mx);

  // Neither fits.
  lmin = lmax = -1;
  got = choose_swapsize(3*GIGA, mn, mx, cmin, cmax, GIGA - page, &lmin, &lmax);
  CHECK(got == 0 && lmin == -1 && lmax == -1);
  // A small request is raised to the minimum, which may not fit either.
  CHECK(choose_swapsize(page, mn, mx, 2*mn, cmax, mn - page, &lmin, &lmax)
      == 0);

  // Without compaction (equal limits) the result is the plain clamp.
  got = choose_swapsize(3*GIGA, mn, mx, mn, mx, 8*GIGA, &lmin, &lmax);
  CHECK(got == mx && lmin == mn && lmax == mx);
  got = choose_swapsize(64*MEGA, mn, mx, mn, mx, 8*GIGA, &lmin, &lmax);
  CHECK(got == 64*MEGA && lmin == mn && lmax == mx);

  // A tiny request is raised to the compacted minimum, or to the base
  // minimum on the fallback.
  got = choose_swapsize(page, mn, mx, cmin, cmax, 8*GIGA, &lmin, &lmax);
  CHECK(got == cmin && lmin == cmin);
  got = choose_swapsize(page, mn, mx, cmin, cmax, cmin - page, &lmin, &lmax);
  CHECK(got == mn && lmin == mn && lmax == mx);

  // Boundary: a size exactly equal to the free space fits, one page more does
  // not.
  got = choose_swapsize(3*GIGA, mn, mx, cmin, cmax, 3*GIGA, &lmin, &lmax);
  CHECK(got == 3*GIGA && lmax == cmax);
  got = choose_swapsize(3*GIGA, mn, mx, cmin, cmax, 3*GIGA - page,
      &lmin, &lmax);
  CHECK(got == mx && lmax == mx);
  got = choose_swapsize(3*GIGA, mn, mx, mn, mx, mx, &lmin, &lmax);
  CHECK(got == mx);
  CHECK(choose_swapsize(3*GIGA, mn, mx, mn, mx, mx - page, &lmin, &lmax) == 0);

  // The fallback has the same boundary: base size == free fits, one page less
  // does not.
  got = choose_swapsize(3*GIGA, mn, mx, cmin, cmax, mx, &lmin, &lmax);
  CHECK(got == mx && lmin == mn && lmax == mx);
  CHECK(choose_swapsize(3*GIGA, mn, mx, cmin, cmax, mx - page,
      &lmin, &lmax) == 0);

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
