/* Unit tests for the pure sizing arithmetic in src/calc.c.
 *
 * calc.c is included directly so that the tests exercise exactly the code that
 * goes into the daemon without exposing an otherwise internal API.
 */
#include <stdio.h>
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

  // ---- Choosing the swapfile to retire: pick_retirable ----------------------
  {
    // Emptiest file wins; equal usage and size: the higher index.
    const memsize_t s1[4] = { 0, 1000, 1000, 1000 };
    const memsize_t u1[4] = { 0,  500,  100,  100 };
    CHECK(pick_retirable(s1, u1, 4, 1000) == 3);

    // Equal usage: the larger file wins.
    const memsize_t s2[3] = { 0, 4000, 1000 };
    const memsize_t u2[3] = { 0,  100,  100 };
    CHECK(pick_retirable(s2, u2, 3, 4000) == 1);

    // Usage beats size: a small empty file is preferred over a big used one.
    const memsize_t s3[2] = { 2000, 1000 };
    const memsize_t u3[2] = {  500,   10 };
    CHECK(pick_retirable(s3, u3, 2, 2000) == 1);

    // Files bigger than the allowance are never chosen.
    const memsize_t s4[2] = { 5000, 1000 };
    const memsize_t u4[2] = {    0,  900 };
    CHECK(pick_retirable(s4, u4, 2, 1000) == 1);
    CHECK(pick_retirable(s4, u4, 2, 500) == -1);

    // Unused slots (size zero) are skipped; nothing retirable gives -1.
    const memsize_t s5[3] = { 0, 0, 0 };
    const memsize_t u5[3] = { 0, 0, 0 };
    CHECK(pick_retirable(s5, u5, 3, 1000000) == -1);
    CHECK(pick_retirable(s5, u5, 0, 1000000) == -1);

    // A single file is chosen when it fits.
    const memsize_t s6[1] = { 1024 };
    const memsize_t u6[1] = { 1024 };
    CHECK(pick_retirable(s6, u6, 1, 1024) == 0);
    CHECK(pick_retirable(s6, u6, 1, 1023) == -1);
  }

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

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
