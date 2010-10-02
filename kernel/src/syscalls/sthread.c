/**
 * $Id$
 * Copyright (C) 2008 - 2009 Nils Asmussen
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

#include <sys/common.h>
#include <sys/task/thread.h>
#include <sys/task/proc.h>
#include <sys/task/signals.h>
#include <sys/task/sched.h>
#include <sys/task/event.h>
#include <sys/task/lock.h>
#include <sys/machine/timer.h>
#include <sys/mem/kheap.h>
#include <sys/syscalls/thread.h>
#include <sys/syscalls.h>
#include <errors.h>

static s32 sysc_doWait(u32 events);

void sysc_gettid(sIntrptStackFrame *stack) {
	sThread *t = thread_getRunning();
	SYSC_RET1(stack,t->tid);
}

void sysc_getThreadCount(sIntrptStackFrame *stack) {
	sThread *t = thread_getRunning();
	SYSC_RET1(stack,sll_length(t->proc->threads));
}

void sysc_startThread(sIntrptStackFrame *stack) {
	u32 entryPoint = SYSC_ARG1(stack);
	void *arg = (void*)SYSC_ARG2(stack);
	s32 res = proc_startThread(entryPoint,arg);
	if(res < 0)
		SYSC_ERROR(stack,res);
	SYSC_RET1(stack,res);
}

void sysc_exit(sIntrptStackFrame *stack) {
	s32 exitCode = (s32)SYSC_ARG1(stack);
	proc_destroyThread(exitCode);
	thread_switch();
	util_panic("We shouldn't get here...");
}

void sysc_getCycles(sIntrptStackFrame *stack) {
	sThread *t = thread_getRunning();
	uLongLong cycles;
	cycles.val64 = t->stats.kcycleCount.val64 + t->stats.ucycleCount.val64;
	SYSC_RET1(stack,cycles.val32.lower);
	SYSC_RET2(stack,cycles.val32.upper);
}

void sysc_sleep(sIntrptStackFrame *stack) {
	u32 msecs = SYSC_ARG1(stack);
	sThread *t = thread_getRunning();
	s32 res;
	if((res = timer_sleepFor(t->tid,msecs)) < 0)
		SYSC_ERROR(stack,res);
	thread_switch();
	/* ensure that we're no longer in the timer-list. this may for example happen if we get a signal
	 * and the sleep-time was not over yet. */
	timer_removeThread(t->tid);
	if(sig_hasSignalFor(t->tid))
		SYSC_ERROR(stack,ERR_INTERRUPTED);
	SYSC_RET1(stack,0);
}

void sysc_yield(sIntrptStackFrame *stack) {
	UNUSED(stack);
	thread_switch();
}

void sysc_wait(sIntrptStackFrame *stack) {
	u32 events = SYSC_ARG1(stack);
	s32 res;

	if((events & ~EV_USER_WAIT_MASK) != 0)
		SYSC_ERROR(stack,ERR_INVALID_ARGS);

	res = sysc_doWait(events);
	if(res < 0)
		SYSC_ERROR(stack,res);
	SYSC_RET1(stack,res);
}

#if 0
void sysc_wait(sIntrptStackFrame *stack) {
	sWaitObject *uobjects = (sWaitObject*)SYSC_ARG1(stack);
	u32 i,objCount = SYSC_ARG2(stack);
	sWaitObject *kobjects;
	sThread *t = thread_getRunning();
	s32 res = ERR_INVALID_ARGS;

	if(!paging_isRangeUserReadable((u32)uobjects,objCount * sizeof(sWaitObject)))
		SYSC_ERROR(stack,ERR_INVALID_ARGS);

	kobjects = (sWaitObject*)kheap_alloc(objCount * sizeof(sWaitObject));
	if(kobjects == NULL)
		SYSC_ERROR(stack,ERR_NOT_ENOUGH_MEM);
	memcpy(kobjects,uobjects,objCount * sizeof(sWaitObject));

	for(i = 0; i < objCount; i++) {
		if(kobjects[i].events & ~(EV_USER_WAIT_MASK))
			goto error;
		if(kobjects[i].events & (EV_CLIENT | EV_RECEIVED_MSG | EV_DATA_READABLE)) {
			/* check flags */
			tFD fd;
			tFileNo file;
			if(kobjects[i].events & EV_CLIENT) {
				if(kobjects[i].events & ~(EV_CLIENT))
					goto error;
			}
			else if(kobjects[i].events & ~(EV_RECEIVED_MSG | EV_DATA_READABLE))
				goto error;
			/* translate fd to node-number */
			fd = (tFD)kobjects[i].object;
			file = proc_fdToFile(fd);
			if(file < 0)
				goto error;
			kobjects[i].object = vfs_getVNode(file);
			if(!kobjects[i].object)
				goto error;
		}
	}

	if(!ev_waitObjects(t->tid,kobjects,objCount)) {
		res = ERR_NOT_ENOUGH_MEM;
		goto error;
	}

	kheap_free(kobjects);
	SYSC_RET1(stack,0);
	return;

error:
	kheap_free(kobjects);
	SYSC_ERROR(stack,res);
}
#endif

void sysc_waitUnlock(sIntrptStackFrame *stack) {
	u32 events = SYSC_ARG1(stack);
	u32 ident = SYSC_ARG2(stack);
	bool global = (bool)SYSC_ARG3(stack);
	sProc *p = proc_getRunning();
	s32 res;

	if((events & ~EV_USER_WAIT_MASK) != 0)
		SYSC_ERROR(stack,ERR_INVALID_ARGS);

	/* release the lock */
	res = lock_release(global ? INVALID_PID : p->pid,ident);
	if(res < 0)
		SYSC_ERROR(stack,res);

	/* now wait */
	res = sysc_doWait(events);
	if(res < 0)
		SYSC_ERROR(stack,res);
	SYSC_RET1(stack,res);
}

void sysc_notify(sIntrptStackFrame *stack) {
	tTid tid = (tTid)SYSC_ARG1(stack);
	u32 events = SYSC_ARG2(stack);

	if((events & ~EV_USER_NOTIFY_MASK) != 0)
		SYSC_ERROR(stack,ERR_INVALID_ARGS);
	ev_wakeupThread(tid,events);
	SYSC_RET1(stack,0);
}

void sysc_lock(sIntrptStackFrame *stack) {
	u32 ident = SYSC_ARG1(stack);
	bool global = (bool)SYSC_ARG2(stack);
	u16 flags = (u16)SYSC_ARG3(stack);
	sProc *p = proc_getRunning();
	s32 res;

	res = lock_aquire(global ? INVALID_PID : p->pid,ident,flags);
	if(res < 0)
		SYSC_ERROR(stack,res);
	SYSC_RET1(stack,res);
}

void sysc_unlock(sIntrptStackFrame *stack) {
	u32 ident = SYSC_ARG1(stack);
	bool global = (bool)SYSC_ARG2(stack);
	sProc *p = proc_getRunning();
	s32 res;

	res = lock_release(global ? INVALID_PID : p->pid,ident);
	if(res < 0)
		SYSC_ERROR(stack,res);
	SYSC_RET1(stack,res);
}

void sysc_join(sIntrptStackFrame *stack) {
	tTid tid = (tTid)SYSC_ARG1(stack);
	sThread *t = thread_getRunning();
	if(tid != 0) {
		sThread *tt = thread_getById(tid);
		/* just threads from the own process */
		if(tt == NULL || tt->tid == t->tid || tt->proc->pid != t->proc->pid)
			SYSC_ERROR(stack,ERR_INVALID_ARGS);
	}

	/* wait until this thread doesn't exist anymore or there are no other threads than ourself */
	do {
		ev_wait(t->tid,EVI_THREAD_DIED,t->proc);
		thread_switchNoSigs();
	}
	while((tid == 0 && sll_length(t->proc->threads) > 1) ||
		(tid != 0 && thread_getById(tid) != NULL));

	SYSC_RET1(stack,0);
}

void sysc_suspend(sIntrptStackFrame *stack) {
	tTid tid = (tTid)SYSC_ARG1(stack);
	sThread *t = thread_getRunning();
	sThread *tt = thread_getById(tid);
	/* just threads from the own process */
	if(tt == NULL || tt->tid == t->tid || tt->proc->pid != t->proc->pid)
		SYSC_ERROR(stack,ERR_INVALID_ARGS);
	/* suspend it */
	thread_setSuspended(tt->tid,true);
	SYSC_RET1(stack,0);
}

void sysc_resume(sIntrptStackFrame *stack) {
	tTid tid = (tTid)SYSC_ARG1(stack);
	sThread *t = thread_getRunning();
	sThread *tt = thread_getById(tid);
	/* just threads from the own process */
	if(tt == NULL || tt->tid == t->tid || tt->proc->pid != t->proc->pid)
		SYSC_ERROR(stack,ERR_INVALID_ARGS);
	/* resume it */
	thread_setSuspended(tt->tid,false);
	SYSC_RET1(stack,0);
}

static s32 sysc_doWait(u32 events) {
	sThread *t = thread_getRunning();
	/* check whether there is a chance that we'll wake up again; if we already have a message
	 * that we should wait for, don't start waiting */
	if(vfs_msgAvailableFor(t->proc->pid,events))
		return 0;
	while(true) {
		ev_waitm(t->tid,events);
		thread_switch();
		if(sig_hasSignalFor(t->tid))
			return ERR_INTERRUPTED;
		/* if we wait for other events than received-msg and client, always wakeup (since we can't
		 * check that) */
		/* otherwise check, whether it really was an event for us => something is available */
		if((events & ~(EV_RECEIVED_MSG | EV_CLIENT)) || vfs_msgAvailableFor(t->proc->pid,events))
			break;
	}
	return 0;
}
