;
; $Id$
; Copyright (C) 2008 - 2009 Nils Asmussen
;
; This program is free software; you can redistribute it and/or
; modify it under the terms of the GNU General Public License
; as published by the Free Software Foundation; either version 2
; of the License, or (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program; if not, write to the Free Software
; Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
;

[BITS 32]

%include "syscalls.s"

[extern errno]
[global outByte]
[global outWord]
[global inByte]
[global inWord]

; void outByte(u16 port,u8 val);
outByte:
	mov		dx,[esp+4]										; load port
	mov		al,[esp+8]										; load value
	out		dx,al													; write to port
	ret

; void outw(u16 port,u16 val);
outWord:
	mov		dx,[esp+4]										; load port
	mov		ax,[esp+8]										; load value
	out		dx,ax													; write to port
	ret

; u8 inByte(u16 port);
inByte:
	mov		dx,[esp+4]										; load port
	in		al,dx													; read from port
	ret

; u16 inWord(u16 port);
inWord:
	mov		dx,[esp+4]										; load port
	in		ax,dx													; read from port
	ret

SYSC_RET_2ARGS_ERR requestIOPorts,SYSCALL_REQIOPORTS
SYSC_RET_2ARGS_ERR releaseIOPorts,SYSCALL_RELIOPORTS
