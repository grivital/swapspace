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

  // Lowest free swapfile slot.
  {
    memsize_t slots[4] = { 0, 0, 0, 0 };
    CHECK(lowest_free_slot(slots, 4) == 0);
    slots[0] = 1;
    CHECK(lowest_free_slot(slots, 4) == 1);
    slots[1] = slots[3] = 1;
    CHECK(lowest_free_slot(slots, 4) == 2);
    slots[2] = 1;
    CHECK(lowest_free_slot(slots, 4) == -1);
    slots[0] = 0;
    CHECK(lowest_free_slot(slots, 4) == 0);
    // Only the first n entries count: the slot at index n is a sentinel.
    slots[3] = 0;
    CHECK(lowest_free_slot(slots, 3) == 0);
    slots[0] = 1;
    CHECK(lowest_free_slot(slots, 3) == -1);
    CHECK(lowest_free_slot(slots, 0) == -1);
  }

  // File priority validation: zero, or enough headroom for nslots slots.
  CHECK(valid_file_priority(0, 32));
  CHECK(!valid_file_priority(1, 32));
  CHECK(!valid_file_priority(30, 32));
  CHECK(valid_file_priority(31, 32));
  CHECK(valid_file_priority(100, 32));
  CHECK(valid_file_priority(32767, 32));
  CHECK(!valid_file_priority(32768, 32));
  CHECK(!valid_file_priority(-1, 32));
  CHECK(valid_file_priority(1, 2));
  CHECK(!valid_file_priority(1, 3));

  // Default keeps the kernel's own choice for every slot.
  CHECK(swapon_flags(0, 0) == 0);
  CHECK(swapon_flags(0, 31) == 0);
  // Slot n is n below slot 0; the lowest slot never goes negative.
  CHECK(swapon_flags(31, 0) == (SWAP_FLAG_PREFER | 31));
  CHECK(swapon_flags(31, 31) == SWAP_FLAG_PREFER);
  CHECK(swapon_flags(100, 0) == (SWAP_FLAG_PREFER | 100));
  CHECK(swapon_flags(100, 5) == (SWAP_FLAG_PREFER | 95));
  CHECK(swapon_flags(32767, 0) == (SWAP_FLAG_PREFER | 32767));
  CHECK(swapon_flags(32767, 31) == (SWAP_FLAG_PREFER | 32736));
  CHECK((swapon_flags(32767, 0) & SWAP_FLAG_PRIO_MASK) == 32767);

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
