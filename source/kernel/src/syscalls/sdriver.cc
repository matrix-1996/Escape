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

#include <sys/common.h>
#include <sys/mem/pagedir.h>
#include <sys/mem/virtmem.h>
#include <sys/task/thread.h>
#include <sys/task/proc.h>
#include <sys/task/signals.h>
#include <sys/task/filedesc.h>
#include <sys/vfs/vfs.h>
#include <sys/vfs/openfile.h>
#include <sys/vfs/node.h>
#include <sys/vfs/channel.h>
#include <sys/syscalls.h>
#include <errno.h>
#include <string.h>

int Syscalls::createdev(Thread *t,IntrptStackFrame *stack) {
	char abspath[MAX_PATH_LEN + 1];
	const char *path = (const char*)SYSC_ARG1(stack);
	uint type = SYSC_ARG2(stack);
	uint ops = SYSC_ARG3(stack);
	pid_t pid = t->getProc()->getPid();
	if(!absolutizePath(abspath,sizeof(abspath),path))
		SYSC_ERROR(stack,-EFAULT);

	/* check type and ops */
	if(type != DEV_TYPE_BLOCK && type != DEV_TYPE_CHAR && type != DEV_TYPE_FS &&
			type != DEV_TYPE_FILE && type != DEV_TYPE_SERVICE)
		SYSC_ERROR(stack,-EINVAL);
	if(type != DEV_TYPE_FS && (ops & ~(DEV_OPEN | DEV_READ | DEV_WRITE | DEV_CLOSE)) != 0)
		SYSC_ERROR(stack,-EINVAL);

	/* create device and open it */
	OpenFile *file;
	int res = VFS::createdev(pid,abspath,type,ops,&file);
	if(res < 0)
		SYSC_ERROR(stack,res);

	/* assoc fd with it */
	int fd = FileDesc::assoc(file);
	if(fd < 0)
		SYSC_ERROR(stack,fd);
	SYSC_RET1(stack,fd);
}

int Syscalls::getclientid(Thread *t,IntrptStackFrame *stack) {
	int fd = (int)SYSC_ARG1(stack);
	pid_t pid = t->getProc()->getPid();

	OpenFile *file = FileDesc::request(fd);
	if(file == NULL)
		SYSC_ERROR(stack,-EBADF);

	inode_t id = file->getClientId(pid);
	FileDesc::release(file);
	if(id < 0)
		SYSC_ERROR(stack,id);
	SYSC_RET1(stack,id);
}

int Syscalls::getclient(Thread *t,IntrptStackFrame *stack) {
	int drvFd = (int)SYSC_ARG1(stack);
	inode_t cid = (inode_t)SYSC_ARG2(stack);
	pid_t pid = t->getProc()->getPid();

	/* get file */
	OpenFile *drvFile = FileDesc::request(drvFd);
	if(drvFile == NULL)
		SYSC_ERROR(stack,-EBADF);

	/* open client */
	OpenFile *file;
	int res = drvFile->openClient(pid,cid,&file);
	FileDesc::release(drvFile);
	if(res < 0)
		SYSC_ERROR(stack,res);

	/* associate fd with file */
	int fd = FileDesc::assoc(file);
	if(fd < 0) {
		file->close(pid);
		SYSC_ERROR(stack,fd);
	}
	SYSC_RET1(stack,fd);
}

int Syscalls::getwork(Thread *t,IntrptStackFrame *stack) {
	int fd = SYSC_ARG1(stack) >> 1;
	uint flags = SYSC_ARG1(stack) & 1;
	msgid_t *id = (msgid_t*)SYSC_ARG2(stack);
	void *data = (void*)SYSC_ARG3(stack);
	size_t size = SYSC_ARG4(stack);
	pid_t pid = t->getProc()->getPid();
	OpenFile *file;

	/* validate pointers */
	if(!PageDir::isInUserSpace((uintptr_t)id,sizeof(msgid_t)))
		SYSC_ERROR(stack,-EFAULT);
	if(!PageDir::isInUserSpace((uintptr_t)data,size))
		SYSC_ERROR(stack,-EFAULT);

	/* translate to files */
	file = FileDesc::request(fd);
	if(file == NULL)
		SYSC_ERROR(stack,-EBADF);

	/* open a client */
	VFSNode *client;
	ssize_t res = OpenFile::getClient(file,&client,flags);

	/* release files */
	FileDesc::release(file);

	if(res < 0)
		SYSC_ERROR(stack,res);

	/* open file */
	res = VFS::openFile(pid,VFS_MSGS | VFS_DEVICE,client,client->getNo(),VFS_DEV_NO,&file);
	VFSNode::release(client);
	if(res < 0) {
		/* we have to set the channel unused again; otherwise its ignored for ever */
		static_cast<VFSChannel*>(client)->setUsed(false);
		SYSC_ERROR(stack,res);
	}

	/* receive a message */
	res = file->receiveMsg(pid,id,data,size,false);
	if(res < 0) {
		file->close(pid);
		SYSC_ERROR(stack,res);
	}

	/* assoc with fd */
	int cfd = FileDesc::assoc(file);
	if(cfd < 0) {
		file->close(pid);
		SYSC_ERROR(stack,cfd);
	}

	SYSC_RET1(stack,cfd);
}
