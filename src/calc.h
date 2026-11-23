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
#ifndef SWAPSPACE_CALC_H
#define SWAPSPACE_CALC_H

#include "memory.h"

/// Truncate n to a multiple of memory page size
memsize_t trunc_to_page(memsize_t n);

/// Round n upwards to a multiple of memory page size
memsize_t ext_to_page(memsize_t n);

/// Recommended change in swap size to reach freetarget from a given state
/** All sizes are in bytes.  A positive return value means "add this many
 * bytes of swap"; a negative one means "we have this much excess".
 *
 * @param totalspace combined memory and swap space currently available
 * @param freespace combined memory and swap space currently free
 * @param freetarget desired percentage of free space
 */
memsize_t ideal_swapsize(memsize_t totalspace, memsize_t freespace,
    int freetarget);

/// Parse the "some avg10" percentage from a /proc/pressure/memory line
/** @return true if line is the "some" line and percent was set */
bool parse_psi_line(const char line[], int *percent);

/// Parse a cgroup memory limit value
/** @return false for "max", negative values, or malformed input */
bool parse_cgroup_limit(const char text[], memsize_t *limit);

/// Swap adjustment wanted in percentage mode (lower/upper limits and target)
/** @param freepct free space as a percentage of totalspace
 * @param pressure whether memory pressure (PSI) is above its threshold
 * @return bytes to add (positive) or excess (negative); zero inside the band
 */
memsize_t percent_swap_request(memsize_t totalspace, memsize_t freespace,
    int freepct, int lower_freelimit, int upper_freelimit, int freetarget,
    bool pressure);

/// Swap adjustment wanted in absolute mode (min_free_swap / max_free_swap bytes)
/** The band is [min_free, max_free]; max_free of zero means 1.5 times min_free
 * (saturating at LLONG_MAX).
 * Outside the band we steer for its middle.  Under pressure we also top up while
 * below the middle, and never release.  Returns zero when min_free is zero.
 */
memsize_t absolute_swap_request(memsize_t freeswap, memsize_t min_free,
    memsize_t max_free, bool pressure);

/// Smallest useful swapfile in absolute mode
/** The amount that would be added when free swap sits exactly at min_free, i.e.
 * the gap to the middle of the band.  Zero when min_free is zero.
 */
memsize_t absolute_min_swapfile(memsize_t min_free, memsize_t max_free);

#endif
