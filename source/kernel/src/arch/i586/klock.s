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

.global klock_aquire
.global klock_release

# void klock_aquire(klock_t *l)
klock_aquire:
	mov		4(%esp),%edx
	mov		$1,%ecx
1:
	xor		%eax,%eax
	lock
	cmpxchg %ecx,(%edx)
	jz		2f
	# improves the performance and lowers the power-consumption of spinlocks
	pause
	jmp		1b
2:
	ret

# void klock_release(klock_t *l)
klock_release:
	mov		4(%esp),%eax
	movl	$0,(%eax)
	ret