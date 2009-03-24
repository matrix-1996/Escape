/**
 * @version		$Id$
 * @author		Nils Asmussen <nils@script-solution.de>
 * @copyright	2008 Nils Asmussen
 */

#include "../h/util.h"
#include "../h/intrpt.h"
#include "../h/proc.h"
#include "../h/ksymbols.h"
#include "../h/paging.h"
#include "../h/video.h"
#include <stdarg.h>
#include <string.h>

/* the x86-call instruction is 5 bytes long */
#define CALL_INSTR_SIZE 5

/**
 * @return the address of the stack-frame-start
 */
extern u32 getStackFrameStart(void);

/**
 * The beginning of the kernel-stack
 */
extern u32 kernelStack;

void panic(const char *fmt,...) {
	sProc *p = proc_getRunning();
	va_list ap;
	vid_printf("\n");
	vid_setLineBG(vid_getLine(),RED);
	vid_useColor(RED,WHITE);
	vid_printf("PANIC: ");

	/* print message */
	va_start(ap,fmt);
	vid_vprintf(fmt,ap);
	va_end(ap);

	vid_printf("\n");
	vid_restoreColor();
	vid_printf("Caused by process %d (%s)\n\n",p->pid,p->command);
	printStackTrace(getKernelStackTrace());
	printStackTrace(getUserStackTrace(p,intrpt_getCurStack()));
	intrpt_setEnabled(false);
	halt();
}

sFuncCall *getUserStackTrace(sProc *p,sIntrptStackFrame *stack) {
	return getStackTrace((u32*)stack->ebp,
			KERNEL_AREA_V_ADDR - p->stackPages * PAGE_SIZE,KERNEL_AREA_V_ADDR);
}

sFuncCall *getKernelStackTrace(void) {
	u32 start,end;
	u32* ebp = (u32*)getStackFrameStart();

	/* determine the stack-bounds; we have a temp stack at the beginning */
	if((u32)ebp >= KERNEL_STACK && (u32)ebp < KERNEL_STACK + PAGE_SIZE) {
		start = KERNEL_STACK;
		end = KERNEL_STACK + PAGE_SIZE;
	}
	else {
		start = ((u32)&kernelStack) - TMP_STACK_SIZE;
		end = (u32)&kernelStack;
	}

	return getStackTrace(ebp,start,end);
}

sFuncCall *getStackTrace(u32 *ebp,u32 start,u32 end) {
	static sFuncCall frames[MAX_STACK_DEPTH];
	u32 i;
	bool isKernel = (u32)ebp >= KERNEL_AREA_V_ADDR;
	sFuncCall *frame = &frames[0];
	sSymbol *sym;

	for(i = 0; i < MAX_STACK_DEPTH; i++) {
		/* prevent page-fault */
		if((u32)ebp < start || (u32)ebp >= end)
			break;
		frame->addr = *(ebp + 1) - CALL_INSTR_SIZE;
		if(isKernel) {
			sym = ksym_getSymbolAt(frame->addr);
			frame->funcAddr = sym->address;
			frame->funcName = sym->funcName;
		}
		else {
			frame->funcAddr = frame->addr;
			frame->funcName = "Unknown";
		}
		ebp = (u32*)*ebp;
		frame++;
	}

	/* terminate */
	frame->addr = 0;
	return &frames[0];
}

void printStackTrace(sFuncCall *trace) {
	if(trace->addr < KERNEL_AREA_V_ADDR)
		vid_printf("User-Stacktrace:\n");
	else
		vid_printf("Kernel-Stacktrace:\n");

	/* TODO maybe we should skip printStackTrace here? */
	while(trace->addr != 0) {
		vid_printf("\t0x%08x -> 0x%08x (%s)\n",(trace + 1)->addr,trace->funcAddr,trace->funcName);
		trace++;
	}
}

void dumpMem(void *addr,u32 dwordCount) {
	u32 *ptr = (u32*)addr;
	while(dwordCount-- > 0) {
		vid_printf("0x%x: 0x%x\n",ptr,*ptr);
		ptr++;
	}
}

void dumpBytes(void *addr,u32 byteCount) {
	u32 i = 0;
	u8 *ptr = (u8*)addr;
	for(i = 0; byteCount-- > 0; i++) {
		vid_printf("%02x ",*ptr);
		ptr++;
		if(i % 12 == 11)
			vid_printf("\n");
	}
}

bool copyUserToKernel(u8 *src,u8 *dst,u32 count) {
	if(!paging_isRangeUserReadable((u32)src,count))
		return false;
	memcpy(dst,src,count);
	return true;
}
