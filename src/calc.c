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

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calc.h"


memsize_t trunc_to_page(memsize_t n)
{
  return n & ~((memsize_t)getpagesize()-1);
}


memsize_t ext_to_page(memsize_t n)
{
  return trunc_to_page(n + getpagesize()-1);
}


memsize_t ideal_swapsize(memsize_t totalspace, memsize_t freespace,
    int freetarget)
{
  /* This is phrased rather oddly, and a fairly high rounding error is
   * introduced.
   *
   * The reason is to avoid overflows in the computation: we effectively compute
   * at a granularity of 100 bytes to avoid overflows, then multiply by 100
   * again to get back to an approximation of our original number.
   *
   * This might conceivably be a problem if freetarget is ridiculously close to
   * either freespace threshold.
   *
   * We try to find the ideal allocation size x>0 such that if we add x free
   * bytes (i.e. we add it to both freespace and totalspace), we end up at
   * exactly our freetarget.  In mathematical terms, i.e. without caring about
   * rounding or overflows:
   *
   * 	(freespace+x)/(totalspace+x) = freetarget/100			<=>
   * 	freespace+x = freetarget*(totalspace+x)/100			<=>
   * 	100*(freespace+x)/freetarget = totalspace+x			<=>
   * 	100*freespace/freetarget+100*x/freetarget = totalspace+x	<=>
   * 	100*freespace/freetarget-totalspace = x - 100*x/freetarget	<=>
   * 	x-100*x/freetarget = 100*freespace/freetarget - totalspace	<=>
   * 	x*(1-100/freetarget) = 100*freespace/freetarget - totalspace	<=>
   *
   * 	x = (100*freespace/freetarget - totalspace) / (1 - 100/freetarget)
   * 	  = (100*freespace - freetarget*totalspace) / (freetarget - 100)
   *
   * This latter expression we convert to an overflow-free form by dividing both
   * freespace and totalspace by some constant p, and multiplying by this same p
   * afterwards:
   *
   *  x/p = (100*freespace/p - freetarget*totalspace/p) / (freetarget-100)
   *
   * The natural choice for p is 100, since it gives up the exact minimum
   * precision while ensuring that both 100*freespace/p and (given that
   * freetarget<100) freetarget*totalspace/p remain within the range of
   * representable numbers, assuming that freespace and totalspace were
   * themselves representable to begin with.
   *
   * For p==100 we get:
   *
   * x/100 = (100*freespace/100-freetarget*totalspace/100) / (freetarget-100)
   *       = (freespace - freetarget*totalspace/100) / (freetarget-100)
   *
   * Note that one of the two divisions by p has disappeared.  To recover as
   * much of that lost precision of the remaining division-by-p, we round to the
   * nearest integer, not downwards:
   */
  return 100 * ((freespace-freetarget*((totalspace+50)/100))/(freetarget-100));
}


bool parse_psi_line(const char line[], int *percent)
{
  if (!line || !percent) return false;

  const char *p = line;
  while (isspace((unsigned char)*p)) ++p;

  // Only the "some" line is interesting: it tells us that at least one task
  // stalled.  "full" means every task stalled, which is too late to act on.
  if (strncmp(p, "some", 4) != 0) return false;
  if (p[4] && !isspace((unsigned char)p[4])) return false;

  p = strstr(p, "avg10=");
  if (!p) return false;
  p += 6;

  char *endptr;
  errno = 0;
  long value = strtol(p, &endptr, 10);
  if (errno == ERANGE || endptr == p || value < 0) return false;

  // The fractional part is irrelevant for a threshold comparison.
  if (value > 100) value = 100;
  *percent = (int)value;
  return true;
}
