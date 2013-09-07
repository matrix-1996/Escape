/**
 * $Id$
 * Copyright (C) 2008 - 2011 Nils Asmussen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#pragma once

#include <esc/common.h>
#include <esc/syscalls.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reads the timestamp-counter
 *
 * @return the number of cycles
 */
static inline uint64_t rdtsc(void);

/**
 * Converts the given TSC value to microseconds.
 *
 * @param tsc the TSC value
 * @return the number of microseconds
 */
static inline uint64_t tsctotime(uint64_t tsc) {
	uint64_t tmp = tsc;
	syscall1(SYSCALL_TSCTOTIME,(ulong)&tmp);
	return tmp;
}

#ifdef __cplusplus
}
#endif

#ifdef __i386__
#	include <esc/arch/i586/time.h>
#endif
#ifdef __eco32__
#	include <esc/arch/eco32/time.h>
#endif
#ifdef __mmix__
#	include <esc/arch/mmix/time.h>
#endif
