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

/// Compaction level for a given number of active swapfiles
/** Level 0 means no compaction.  Otherwise the level is active-threshold+1:
 * the new swapfile allocated when active == threshold, which brings the count
 * past the threshold, is level 1, the next one level 2, and so on.
 * Compaction is off if threshold <= 0 (disabled), factor <= 1 (no effect) or
 * active < threshold.
 */
int compact_level(int active, int threshold, int factor);

/// Effective swapfile size limits while swapfile compaction applies
/** At compaction level n (see compact_level()) both limits are multiplied by
 * factor^n, so each new swapfile is about factor times the size of the one
 * before it.  The multiplication saturates at the largest page-aligned value
 * instead of overflowing.  If cap > 0 it bounds the effective maximum, but
 * never below the configured maximum (compaction only ever enlarges
 * swapfiles); the effective minimum is bounded by the effective maximum.
 * Without compaction the limits are passed through unchanged.  The function is
 * stateless, so the limits step back down by themselves when swapfiles are
 * retired.  It is monotonic: minsize <= maxsize implies *outmin <= *outmax.
 *
 * @param active number of swapfiles currently in use
 * @param threshold active count at which compaction starts; 0 disables it
 * @param factor size multiplier per compaction level; 1 means no effect
 * @param cap upper bound for the compacted maximum size in bytes; 0 for none
 * @param minsize configured minimum swapfile size, in bytes
 * @param maxsize configured maximum swapfile size, in bytes
 * @param outmin receives the effective minimum size
 * @param outmax receives the effective maximum size
 * @return true if compaction applies
 */
bool compact_limits(int active, int threshold, int factor, memsize_t cap,
    memsize_t minsize, memsize_t maxsize,
    memsize_t *outmin, memsize_t *outmax);

/// Pick the size of a new swapfile that fits the free disk space
/** The request is clamped to the compacted limits; if that does not fit in
 * free_space, to the configured (base) limits, so compaction never makes an
 * allocation fail that would have succeeded without it.  All sizes are in
 * bytes and the limits must be positive.
 *
 * @param request desired size
 * @param base_min configured minimum swapfile size
 * @param base_max configured maximum swapfile size
 * @param compact_min effective minimum from compact_limits()
 * @param compact_max effective maximum from compact_limits()
 * @param free_space free space on the swap filesystem
 * @param out_min receives the limits that go with the chosen size
 * @param out_max receives the limits that go with the chosen size
 * @return the size to create, or 0 if even the base size does not fit
 */
memsize_t choose_swapsize(memsize_t request,
    memsize_t base_min, memsize_t base_max,
    memsize_t compact_min, memsize_t compact_max,
    memsize_t free_space, memsize_t *out_min, memsize_t *out_max);

/// Parse the "some avg10" percentage from a /proc/pressure/memory line
/** @return true if line is the "some" line and percent was set */
bool parse_psi_line(const char line[], int *percent);

/// Parse a cgroup memory limit value
/** @return false for "max", negative values, or malformed input */
bool parse_cgroup_limit(const char text[], memsize_t *limit);

#endif
