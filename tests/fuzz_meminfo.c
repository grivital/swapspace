/* libFuzzer harness for the /proc/meminfo line parser.
 *
 * Build with: ./configure CC=clang --enable-fuzzing && make && make check
 * Run with:   tests/fuzz_meminfo -max_total_time=60 corpus/
 */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "../src/main.h"
#include "../src/memory.h"

/* Normally provided by main.c, which is not linked into this harness. */
char localbuf[16384];
time_t runclock = 0;
bool quiet = false;
bool verbose = false;


int LLVMFuzzerTestOneInput(const uint8_t data[], size_t size)
{
  return fuzz_meminfo_line(data, size);
}
