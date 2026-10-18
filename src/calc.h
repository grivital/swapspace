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

#endif
