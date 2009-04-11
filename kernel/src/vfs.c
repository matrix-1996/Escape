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

#include <common.h>
#include <vfs.h>
#include <vfsnode.h>
#include <vfsreal.h>
#include <proc.h>
#include <util.h>
#include <kheap.h>
#include <sched.h>
#include <video.h>
#include <string.h>
#include <sllist.h>
#include <assert.h>
#include <errors.h>

/* max number of open files */
#define FILE_COUNT					(PROC_COUNT * 16)
/* the processes node */
#define PROCESSES()					(nodes + 8)
/* the services node */
#define SERVICES()					(nodes + 11)

/* the initial size of the write-cache for service-usage-nodes */
#define VFS_INITIAL_WRITECACHE		128

/* an entry in the global file table */
typedef struct {
	/* read OR write; flags = 0 => entry unused */
	u8 flags;
	/* the owner of this file; sharing of a file is not possible for different processes */
	tPid owner;
	/* number of references */
	u16 refCount;
	/* current position in file */
	u32 position;
	/* node-number; if MSB = 1 => virtual, otherwise real inode-number */
	tVFSNodeNo nodeNo;
} sGFTEntry;

/* a message (for communicating with services) */
typedef struct {
	u32 length;
} sMessage;

/**
 * Searches for a free file for the given flags and node-number
 *
 * @param pid the process to use
 * @param flags the flags (read, write)
 * @param nodeNo the node-number to open
 * @return the file-number on success or the negative error-code
 */
static tFileNo vfs_getFreeFile(tPid pid,u8 flags,tVFSNodeNo nodeNo);

/**
 * The write-handler for the VFS
 *
 * @param pid the process to use
 * @param node the VFS node
 * @param buffer the buffer where to read from
 * @param offset the offset where to start
 * @param count the number of bytes
 * @return the number of written bytes
 */
static s32 vfs_writeHandler(tPid pid,sVFSNode *n,u8 *buffer,u32 offset,u32 count);

/* global file table */
static sGFTEntry globalFileTable[FILE_COUNT];

void vfs_init(void) {
	sVFSNode *root,*sys;
	vfsn_init();

	/*
	 *  /
	 *   file:
	 *   system:
	 *     |-pipe
	 *     |-processes
	 *   services:
	 */
	root = vfsn_createDir(NULL,(char*)"");
	vfsn_createServiceNode(KERNEL_PID,root,(char*)"file",0);
	sys = vfsn_createDir(root,(char*)"system");
	vfsn_createPipeCon(sys,(char*)"pipe");
	vfsn_createDir(sys,(char*)"processes");
	vfsn_createDir(root,(char*)"services");
	vfsn_createDir(sys,(char*)"devices");
	vfsn_createDir(sys,(char*)"bin");
}

s32 vfs_hasAccess(tPid pid,tVFSNodeNo nodeNo,u8 flags) {
	sVFSNode *n = nodes + VIRT_INDEX(nodeNo);
	/* kernel is allmighty :P */
	if(pid == KERNEL_PID)
		return 0;

	if(n->owner == pid) {
		if((flags & VFS_READ) && !(n->mode & MODE_OWNER_READ))
			return ERR_NO_READ_PERM;
		if((flags & VFS_WRITE) && !(n->mode & MODE_OWNER_WRITE))
			return ERR_NO_WRITE_PERM;
	}
	else {
		if((flags & VFS_READ) && !(n->mode & MODE_OTHER_READ))
			return ERR_NO_READ_PERM;
		if((flags & VFS_WRITE) && !(n->mode & MODE_OTHER_WRITE))
			return ERR_NO_WRITE_PERM;
	}
	return 0;
}

tFileNo vfs_inheritFileNo(tPid pid,tFileNo file) {
	sGFTEntry *e = globalFileTable + file;
	sVFSNode *n = vfsn_getNode(e->nodeNo);
	/* we can't share multipipe-service-usages since each process has his own node */
	if((n->mode & MODE_TYPE_SERVUSE) && !(n->parent->mode & MODE_SERVICE_SINGLEPIPE)) {
		sVFSNode *child;
		tVFSNodeNo nodeNo;
		tFileNo newFile;
		s32 err = vfsn_createServiceUse(pid,n->parent,&child);
		if(err < 0)
			return -1;

		nodeNo = NADDR_TO_VNNO(child);
		newFile = vfs_openFile(pid,e->flags,nodeNo);
		if(newFile < 0)
			return -1;
		return newFile;
	}
	/* if a pipe is inherited we need a new file for it (position should be different )*/
	else if(n->mode & MODE_TYPE_PIPE) {
		tFileNo newFile;
		/* we'll get a new file since the pid is different */
		newFile = vfs_openFile(pid,e->flags,e->nodeNo);
		if(newFile < 0)
			return -1;
		return newFile;
	}
	else {
		/* just increase references */
		e->refCount++;
		return file;
	}
}

s32 vfs_incRefs(tFileNo file) {
	sGFTEntry *e;
	/* invalid file-number? */
	if(file < 0 || file >= FILE_COUNT)
		return ERR_INVALID_FILE;

	e = globalFileTable + file;
	if(e->flags == 0)
		return ERR_INVALID_FILE;

	e->refCount++;
	return 0;
}

tVFSNodeNo vfs_getNodeNo(tFileNo file) {
	sGFTEntry *e;

	/* invalid file-number? */
	if(file < 0 || file >= FILE_COUNT)
		return ERR_INVALID_FILE;

	/* not in use? */
	e = globalFileTable + file;
	if(e->flags == 0)
		return ERR_INVALID_FILE;

	return e->nodeNo;
}

tFileNo vfs_openFile(tPid pid,u8 flags,tVFSNodeNo nodeNo) {
	sGFTEntry *e;

	/* determine free file */
	tFileNo f = vfs_getFreeFile(pid,flags,nodeNo);
	if(f < 0)
		return f;

	/* count references of virtual nodes */
	if(IS_VIRT(nodeNo)) {
		s32 err;
		sVFSNode *n = nodes + VIRT_INDEX(nodeNo);
		if((err = vfs_hasAccess(pid,nodeNo,flags)) < 0)
			return err;
		n->refCount++;
	}

	/* unused file? */
	e = globalFileTable + f;
	if(e->flags == 0) {
		e->owner = pid;
		e->flags = flags;
		e->refCount = 1;
		e->position = 0;
		e->nodeNo = nodeNo;
	}
	else
		e->refCount++;

	return f;
}

static tFileNo vfs_getFreeFile(tPid pid,u8 flags,tVFSNodeNo nodeNo) {
	tFileNo i;
	tFileNo freeSlot = ERR_NO_FREE_FD;
	bool isServUse = false;
	sGFTEntry *e = &globalFileTable[0];

	vassert(flags & (VFS_READ | VFS_WRITE),"flags empty");
	vassert(!(flags & ~(VFS_READ | VFS_WRITE)),"flags contains invalid bits");
	/* ensure that we don't increment usages of an unused slot */
	vassert(flags != 0,"No flags given");

	if(IS_VIRT(nodeNo)) {
		vassert(VIRT_INDEX(nodeNo) < NODE_COUNT,"nodeNo invalid");
		sVFSNode *n = vfsn_getNode(nodeNo);
		/* we can add pipes here, too, since every open() to a pipe will get a new node anyway */
		isServUse = (n->mode & (MODE_TYPE_SERVUSE | MODE_TYPE_PIPE)) ? true : false;
	}

	for(i = 0; i < FILE_COUNT; i++) {
		/* used slot and same node? */
		if(e->flags != 0) {
			/* we don't want to share files with different processes */
			/* this is allowed only if we create a child-process. he will inherit the files.
			 * in this case we trust the processes that they know what they do :) */
			if(e->nodeNo == nodeNo && e->owner == pid) {
				/* service-usages may use a file twice for reading and writing because we
				 * will prevent trouble anyway */
				if(isServUse && e->flags == flags)
					return i;

				/* someone does already write to this file? so it's not really good
				 * to use it atm, right? */
				if(!isServUse && e->flags & VFS_WRITE)
					return ERR_FILE_IN_USE;

				/* if the flags are different we need a different slot */
				if(e->flags == flags)
					return i;
			}
		}
		/* remember free slot */
		else if(freeSlot == ERR_NO_FREE_FD) {
			freeSlot = i;
			/* just for performance: if we've found an unused file and want to use a service,
			 * use this slot because it doesn't really matter wether we use a new file or an
			 * existing one (if there even is any) */
			/* note: we can share a file for writing in this case! */
			if(isServUse)
				break;
		}

		e++;
	}

	return freeSlot;
}

tFileNo vfs_openFileForKernel(tPid pid,tVFSNodeNo nodeNo) {
	sVFSNode *node = vfsn_getNode(nodeNo);
	sVFSNode *n = NODE_FIRST_CHILD(node);

	/* not not already present? */
	if(n == NULL || n->owner != KERNEL_PID) {
		n = vfsn_createNode(node,(char*)SERVICE_CLIENT_KERNEL);
		if(n == NULL)
			return ERR_NOT_ENOUGH_MEM;

		/* init node */
		/* the service has read/write permission */
		n->mode = MODE_TYPE_SERVUSE | MODE_OTHER_READ | MODE_OTHER_WRITE;
		n->readHandler = &vfs_serviceUseReadHandler;
		n->data.servuse.locked = -1;
		n->owner = KERNEL_PID;

		/* insert as first child */
		n->prev = NULL;
		n->next = node->firstChild;
		node->firstChild = n;
		if(node->lastChild == NULL)
			node->lastChild = n;
		n->parent = node;
	}

	/* open the file and return it */
	/* we don't need the file-descriptor here */
	return vfs_openFile(pid,VFS_READ | VFS_WRITE,NADDR_TO_VNNO(n));
}

bool vfs_eof(tPid pid,tFileNo file) {
	sGFTEntry *e = globalFileTable + file;
	bool eof = true;

	if(IS_VIRT(e->nodeNo)) {
		tVFSNodeNo i = VIRT_INDEX(e->nodeNo);
		sVFSNode *n = nodes + i;

		if(n->mode & MODE_TYPE_SERVUSE) {
			if(n->parent->owner == pid)
				eof = sll_length(n->data.servuse.sendList) == 0;
			else
				eof = sll_length(n->data.servuse.recvList) == 0;
		}
		else
			eof = e->position >= n->data.def.pos;
	}
	else {
		/* TODO */
	}

	return eof;
}

s32 vfs_seek(tPid pid,tFileNo file,u32 position) {
	sGFTEntry *e = globalFileTable + file;

	if(IS_VIRT(e->nodeNo)) {
		tVFSNodeNo i = VIRT_INDEX(e->nodeNo);
		sVFSNode *n = nodes + i;

		if(n->mode & MODE_TYPE_SERVUSE)
			return ERR_SERVUSE_SEEK;

		/* set position */
		e->position = MIN(n->data.def.pos - 1,position);
	}
	else {
		/* TODO */
	}

	return 0;
}

s32 vfs_readFile(tPid pid,tFileNo file,u8 *buffer,u32 count) {
	s32 err,readBytes;
	sGFTEntry *e = globalFileTable + file;

	if(IS_VIRT(e->nodeNo)) {
		tVFSNodeNo i = VIRT_INDEX(e->nodeNo);
		sVFSNode *n = nodes + i;

		if((err = vfs_hasAccess(pid,e->nodeNo,VFS_READ)) < 0)
			return err;

		/* node not present anymore? */
		if(n->name == NULL)
			return ERR_INVALID_FILE;

		/* NOTE: we have to lock service-usages for reading because if we have a single-pipe-
		 * service it would be possible to steal a message if no locking is made.
		 * P1 reads the header of a message (-> message still present), P2 reads the complete
		 * message (-> message deleted). So P1 missed the data of the message. */

		/* wait until the node is unlocked, if necessary */
		if((n->mode & MODE_TYPE_SERVUSE) && n->data.servuse.locked != file) {
			/* don't let the kernel wait here (-> deadlock) */
			if(n->data.servuse.locked != -1 && pid == KERNEL_PID)
				return 0;

			while(n->data.servuse.locked != -1)
				proc_switch();
		}

		/* use the read-handler */
		readBytes = n->readHandler(pid,n,buffer,e->position,count);

		if((n->mode & MODE_TYPE_SERVUSE)) {
			/* store position in first message */
			if(readBytes <= 0) {
				readBytes = -readBytes;
				e->position = 0;
				/* unlock node */
				n->data.servuse.locked = -1;
			}
			else {
				e->position += readBytes;
				/* lock node */
				n->data.servuse.locked = file;
			}
		}
		else
			e->position += readBytes;
	}
	else {
		/* query the fs-service to read from the inode */
		readBytes = vfsr_readFile(pid,e->nodeNo,buffer,e->position,count);
		if(readBytes > 0)
			e->position += readBytes;
	}

	return readBytes;
}

s32 vfs_writeFile(tPid pid,tFileNo file,u8 *buffer,u32 count) {
	s32 err,writtenBytes;
	sVFSNode *n;
	sGFTEntry *e = globalFileTable + file;

	if(IS_VIRT(e->nodeNo)) {
		tVFSNodeNo i = VIRT_INDEX(e->nodeNo);
		n = nodes + i;

		if((err = vfs_hasAccess(pid,e->nodeNo,VFS_WRITE)) < 0)
			return err;

		/* node not present anymore? */
		if(n->name == NULL)
			return ERR_INVALID_FILE;

		/* write to the node */
		writtenBytes = vfs_writeHandler(pid,n,buffer,e->position,count);
		if(writtenBytes < 0)
			return writtenBytes;

		/* don't change the position for service-usages */
		/* since we don't need it and it would cause problems with the next read-op */
		if(!(n->mode & MODE_TYPE_SERVUSE))
			e->position += writtenBytes;
	}
	else {
		/* query the fs-service to write to the inode */
		writtenBytes = vfsr_writeFile(pid,e->nodeNo,buffer,e->position,count);
		if(writtenBytes > 0)
			e->position += writtenBytes;
	}

	return writtenBytes;
}

void vfs_closeFile(tFileNo file) {
	sGFTEntry *e = globalFileTable + file;

	/* decrement references */
	if(--(e->refCount) == 0) {
		tVFSNodeNo i = VIRT_INDEX(e->nodeNo);
		sVFSNode *n = nodes + i;

		if(IS_VIRT(e->nodeNo)) {
			if(n->name != NULL) {
				/* we HAVE TO unlock the node if the file has locked it (read a msg incompletely) */
				if(n->data.servuse.locked == file)
					n->data.servuse.locked = -1;

				/* last usage? */
				if(--(n->refCount) == 0) {
					/* we have to remove the service-usage-node, if it is one */
					if((n->mode & MODE_TYPE_SERVUSE)) {
						/* if there are message for the service we don't want to throw them away */
						/* if there are any in the receivelist (and no references of the node) we
						 * can throw them away because no one will read them anymore
						 * (it means that the client has already closed the file) */
						/* note also that we can assume that the service is still running since we
						 * would have deleted the whole service-node otherwise */
						if(n->data.servuse.sendList == NULL || sll_length(n->data.servuse.sendList) == 0) {
							/* free send and receive list */
							if(n->data.servuse.recvList != NULL) {
								sll_destroy(n->data.servuse.recvList,true);
								n->data.servuse.recvList = NULL;
							}
							if(n->data.servuse.sendList != NULL) {
								sll_destroy(n->data.servuse.sendList,true);
								n->data.servuse.sendList = NULL;
							}

							/* free node */
							if((n->parent->mode & MODE_SERVICE_SINGLEPIPE) == 0)
								kheap_free(n->name);
							vfsn_removeChild(n->parent,n);
						}
					}

					/* remove pipe */
					if((n->mode & MODE_TYPE_PIPE)) {
						kheap_free(n->name);
						vfsn_removeChild(n->parent,n);
					}
				}
			}
		}
		else
			vfsr_closeFile(e->nodeNo);

		/* mark unused */
		e->flags = 0;
	}
}

tServ vfs_createService(tPid pid,const char *name,u32 type) {
	sVFSNode *serv = SERVICES();
	sVFSNode *n = serv->firstChild;
	u32 len;
	char *hname;

	vassert(name != NULL,"name == NULL");

	/* we don't want to have exotic service-names */
	if((len = strlen(name)) == 0 || !isalnumstr(name))
		return ERR_INV_SERVICE_NAME;

	while(n != NULL) {
		/* entry already existing? */
		if(strcmp(n->name,name) == 0)
			return ERR_SERVICE_EXISTS;
		n = n->next;
	}

	/* copy name to kernel-heap */
	hname = (char*)kheap_alloc(len + 1);
	if(hname == NULL)
		return ERR_NOT_ENOUGH_MEM;
	strncpy(hname,name,len);
	hname[len] = '\0';

	/* create node */
	n = vfsn_createServiceNode(pid,serv,hname,type);
	if(n != NULL) {
		/* TODO that's not really nice ;) */
		if(strcmp(name,"fs") == 0)
			vfsr_setFSService(NADDR_TO_VNNO(n));
		return NADDR_TO_VNNO(n);
	}

	/* failed, so cleanup */
	kheap_free(hname);
	return ERR_NOT_ENOUGH_MEM;
}

bool vfs_msgAvailableFor(tPid pid,u8 events) {
	sVFSNode *n = SERVICES();
	sProc *p = proc_getByPid(pid);
	bool isService = false;
	bool isClient = false;
	tFD i;

	/* at first we check wether the process is a service */
	if(events & EV_CLIENT) {
		n = NODE_FIRST_CHILD(n);
		while(n != NULL) {
			if(n->owner == pid) {
				tVFSNodeNo nodeNo = NADDR_TO_VNNO(n);
				tVFSNodeNo client = vfs_getClient(pid,&nodeNo,1);
				isService = true;
				if(vfsn_isValidNodeNo(client))
					return true;
			}
			n = n->next;
		}
	}

	/* now search through the file-descriptors if there is any message */
	if(events & EV_RECEIVED_MSG) {
		for(i = 0; i < MAX_FD_COUNT; i++) {
			if(p->fileDescs[i] != -1) {
				sGFTEntry *e = globalFileTable + p->fileDescs[i];
				if(IS_VIRT(e->nodeNo)) {
					n = vfsn_getNode(e->nodeNo);
					/* service-usage and a message in the receive-list? */
					/* we don't want to check that if it is our own service. because it makes no
					 * sense to read from ourself ;) */
					if((n->mode & MODE_TYPE_SERVUSE) && n->parent->owner != pid) {
						isClient = true;
						if(n->data.servuse.recvList != NULL && sll_length(n->data.servuse.recvList) > 0)
							return true;
					}
				}
			}
		}
	}

	/* if we are no client and no service we'll never receive a message */
	/*if(isClient || isService)*/
		return false;
	/*return true;*/
}

s32 vfs_getClient(tPid pid,tVFSNodeNo *vfsNodes,u32 count) {
	sVFSNode *n,*node;
	u32 i;
	for(i = 0; i < count; i++) {
		if(!vfsn_isValidNodeNo(vfsNodes[i]))
			return ERR_INVALID_NODENO;

		node = nodes + vfsNodes[i];
		if(node->owner != pid || !(node->mode & MODE_TYPE_SERVICE))
			return ERR_NOT_OWN_SERVICE;

		/* search for a slot that needs work */
		n = NODE_FIRST_CHILD(node);
		while(n != NULL) {
			/* data available? */
			if(n->data.servuse.sendList != NULL && sll_length(n->data.servuse.sendList) > 0)
				break;
			n = n->next;
		}

		if(n != NULL)
			return NADDR_TO_VNNO(n);
	}
	return ERR_NO_CLIENT_WAITING;;
}

tFileNo vfs_openClient(tPid pid,tVFSNodeNo *vfsNodes,u32 count,tVFSNodeNo *servNode) {
	sVFSNode *n;
	tVFSNodeNo client = vfs_getClient(pid,vfsNodes,count);
	/* error? */
	if(!vfsn_isValidNodeNo(client))
		return client;

	/* open a file for it so that the service can read and write with it */
	n = vfsn_getNode(client);
	*servNode = NADDR_TO_VNNO(n->parent);
	return vfs_openFile(pid,VFS_READ | VFS_WRITE,client);
}

s32 vfs_removeService(tPid pid,tVFSNodeNo nodeNo) {
	sVFSNode *serv = SERVICES();
	sVFSNode *m,*t,*n = nodes + nodeNo;

	vassert(vfsn_isValidNodeNo(nodeNo),"Invalid node number %d",nodeNo);

	if(n->owner != pid || !(n->mode & MODE_TYPE_SERVICE))
		return ERR_NOT_OWN_SERVICE;

	/* remove childs (service-usages) */
	m = NODE_FIRST_CHILD(n);
	while(m != NULL) {
		t = m->next;
		/* free memory */
		if((n->mode & MODE_SERVICE_SINGLEPIPE) == 0)
			kheap_free(m->name);

		/* free send and receive list */
		if(m->data.servuse.recvList != NULL) {
			sll_destroy(m->data.servuse.recvList,true);
			m->data.servuse.recvList = NULL;
		}
		if(m->data.servuse.sendList != NULL) {
			sll_destroy(m->data.servuse.sendList,true);
			m->data.servuse.sendList = NULL;
		}

		/* remove node */
		vfsn_removeChild(n,m);

		/* to next */
		m = t;
	}

	/* free node */
	kheap_free(n->name);
	vfsn_removeChild(serv,n);
	return 0;
}

bool vfs_createProcess(tPid pid,fRead handler) {
	char *name;
	sVFSNode *proc = PROCESSES();
	sVFSNode *n = proc->firstChild;

	/* build name */
	name = (char*)kheap_alloc(sizeof(char) * 12);
	if(name == NULL)
		return false;

	itoa(name,pid);

	/* go to last entry */
	while(n != NULL) {
		/* entry already existing? */
		if(strcmp(n->name,name) == 0) {
			kheap_free(name);
			return false;
		}
		n = n->next;
	}

	n = vfsn_createInfo(KERNEL_PID,proc,name,handler);
	if(n != NULL) {
		/* invalidate cache */
		if(proc->data.def.cache != NULL) {
			kheap_free(proc->data.def.cache);
			proc->data.def.cache = NULL;
		}
		return true;
	}

	kheap_free(name);
	return false;
}

void vfs_removeProcess(tPid pid) {
	sVFSNode *proc = PROCESSES();
	sVFSNode *serv = SERVICES();
	char name[12];
	sVFSNode *n,*t;
	itoa(name,pid);

	/* remove from system:/processes */
	n = NODE_FIRST_CHILD(proc->firstChild);
	while(n != NULL) {
		/* found node? */
		if(strcmp(n->name,name) == 0) {
			/* free node */
			kheap_free(n->name);
			vfsn_removeChild(proc,n);
			break;
		}
		n = n->next;
	}

	/* check if the process is a service */
	n = NODE_FIRST_CHILD(serv->firstChild);
	while(n != NULL) {
		if((n->mode & MODE_TYPE_SERVICE) && n->owner == pid) {
			t = n->next;
			vfs_removeService(pid,NADDR_TO_VNNO(n));
			n = t;
		}
		else
			n = n->next;
	}

	/* invalidate cache */
	if(proc->data.def.cache != NULL) {
		kheap_free(proc->data.def.cache);
		proc->data.def.cache = NULL;
	}
}

s32 vfs_defReadHandler(tPid pid,sVFSNode *node,u8 *buffer,u32 offset,u32 count) {
	s32 byteCount;
	UNUSED(pid);
	/* no data available? */
	if(node->data.def.cache == NULL)
		return 0;

	if(offset > node->data.def.pos)
		offset = node->data.def.pos;
	byteCount = MIN(node->data.def.pos - offset,count);
	if(byteCount > 0) {
		/* simply copy the data to the buffer */
		memcpy(buffer,(u8*)node->data.def.cache + offset,byteCount);
	}
	return byteCount;
}

s32 vfs_readHelper(tPid pid,sVFSNode *node,u8 *buffer,u32 offset,u32 count,u32 dataSize,
		fReadCallBack callback) {
	void *mem;

	UNUSED(pid);
	vassert(node != NULL,"node == NULL");
	vassert(buffer != NULL,"buffer == NULL");

	/* can we copy it directly? */
	if(offset == 0 && count == dataSize)
		mem = buffer;
	/* don't waste time in this case */
	else if(offset >= dataSize)
		return 0;
	/* ok, use the heap as temporary storage */
	else {
		mem = kheap_alloc(dataSize);
		if(mem == NULL)
			return 0;
	}

	/* copy values to public struct */
	callback(node,mem);

	/* stored on kheap? */
	if((u32)mem != (u32)buffer) {
		/* correct vars */
		if(offset > dataSize)
			offset = dataSize;
		count = MIN(dataSize - offset,count);
		/* copy */
		if(count > 0)
			memcpy(buffer,(u8*)mem + offset,count);
		/* free temp storage */
		kheap_free(mem);
	}

	return count;
}

s32 vfs_serviceUseReadHandler(tPid pid,sVFSNode *node,u8 *buffer,u32 offset,u32 count) {
	sSLList *list;
	sMessage *msg;

	/* services reads from the send-list */
	if(node->parent->owner == pid) {
		list = node->data.servuse.sendList;
		/* don't block service-reads */
		if(sll_length(list) == 0)
			return 0;
	}
	/* other processes read from the receive-list */
	else {
		/* don't block the kernel ;) */
		if(pid != KERNEL_PID) {
			/* wait until a message arrives */
			/* don't cache the list here, because the pointer changes if the list is NULL */
			while(sll_length(node->data.servuse.recvList) == 0) {
				proc_wait(pid,EV_RECEIVED_MSG);
				proc_switch();
			}
		}
		else if(sll_length(node->data.servuse.recvList) == 0)
			return 0;

		list = node->data.servuse.recvList;
	}

	/* get first element and copy data to buffer */
	msg = sll_get(list,0);
	offset = MIN(msg->length - 1,offset);
	count = MIN(msg->length - offset,count);
	/* the data is behind the message */
	memcpy(buffer,(u8*)(msg + 1) + offset,count);

	/*vid_printf("\n%s read msg from %s; src=0x%x,length=%d\n",
					proc_getByPid(pid)->name,node->parent->name,(u8*)(msg + 1) + offset,count);*/

	/*vid_printf("\n%s read msg from %s:\n---\n",proc_getByPid(pid)->name,node->parent->name);
	util_dumpBytes(buffer,count);
	vid_printf("\n---\n");*/

	/* free data and remove element from list if the complete message has been read */
	if(offset + count >= msg->length) {
		kheap_free(msg);
		sll_removeIndex(list,0);
		/* negative because we have read the complete msg */
		return -count;
	}

	return count;
}

static s32 vfs_writeHandler(tPid pid,sVFSNode *n,u8 *buffer,u32 offset,u32 count) {
	void *cache;
	void *oldCache;
	u32 newSize = 0;

	/* determine the cache to use */
	if((n->mode & MODE_TYPE_SERVUSE)) {
		sSLList **list;
		sMessage *msg;
		/* services write to the receive-list (which will be read by other processes) */
		/* special-case: pid == KERNEL_PID: the kernel wants to write to a service */
		if(pid != KERNEL_PID && n->parent->owner == pid)
			list = &(n->data.servuse.recvList);
		/* other processes write to the send-list (which will be read by the service) */
		else
			list = &(n->data.servuse.sendList);

		if(*list == NULL)
			*list = sll_create();

		/* create message and copy data to it */
		msg = kheap_alloc(sizeof(sMessage) + count * sizeof(u8));
		if(msg == NULL)
			return ERR_NOT_ENOUGH_MEM;

		msg->length = count;
		memcpy(msg + 1,buffer,count);

		/*vid_printf("\n%s Wrote msg to %s; dest=0x%x,length=%d\n",
				proc_getByPid(pid)->name,n->parent->name,msg + 1,count);*/
		/*util_dumpBytes(buffer,count);
		vid_printf("\n---\n");*/

		/* append to list */
		sll_append(*list,msg);

		/* notify the service */
		if(list == &(n->data.servuse.sendList)) {
			if(n->parent->owner != KERNEL_PID)
				proc_wakeup(n->parent->owner,EV_CLIENT);
		}
		else {
			if(n->parent->mode & MODE_SERVICE_SINGLEPIPE) {
				/* we don't know who uses the service. Therefore we have to unblock all :/ */
				/* TODO is there a better way? */
				proc_wakeupAll(EV_RECEIVED_MSG);
			}
			else {
				/* notify the process that there is a message */
				if(n->owner != KERNEL_PID)
					proc_wakeup(n->owner,EV_RECEIVED_MSG);
				else
					vfsr_setGotMsg();
			}
		}
		return count;
	}

	cache = n->data.def.cache;
	oldCache = cache;

	/* need to create cache? */
	if(cache == NULL) {
		newSize = MAX(count,VFS_INITIAL_WRITECACHE);
		/* check for overflow */
		if(newSize > 0xFFFF)
			newSize = 0xFFFF;
		if(newSize < count)
			return ERR_NOT_ENOUGH_MEM;

		n->data.def.cache = kheap_alloc(newSize);
		cache = n->data.def.cache;
		/* reset position */
		n->data.def.pos = 0;
	}
	/* need to increase cache-size? */
	else if(n->data.def.size < offset + count) {
		/* ensure that we allocate enough memory */
		newSize = MAX(offset + count,(u32)n->data.def.size * 2);
		/* check for overflow */
		if(newSize > 0xFFFF)
			newSize = 0xFFFF;
		if(newSize < offset + count)
			return ERR_NOT_ENOUGH_MEM;

		n->data.def.cache = kheap_realloc(cache,newSize);
		cache = n->data.def.cache;
	}

	/* all ok? */
	if(cache != NULL) {
		/* copy the data into the cache */
		memcpy((u8*)cache + offset,buffer,count);
		/* set total size and number of used bytes */
		if(newSize)
			n->data.def.size = newSize;
		/* we have checked size for overflow. so it is ok here */
		n->data.def.pos = MAX(n->data.def.pos,offset + count);

		return count;
	}

	/* restore cache */
	n->data.def.cache = oldCache;
	return ERR_NOT_ENOUGH_MEM;
}


/* #### TEST/DEBUG FUNCTIONS #### */
#if DEBUGGING

u32 vfs_dbg_getGFTEntryCount(void) {
	u32 i,count = 0;
	for(i = 0; i < FILE_COUNT; i++) {
		if(globalFileTable[i].flags != 0)
			count++;
	}
	return count;
}

void vfs_dbg_printGFT(void) {
	tFileNo i;
	sGFTEntry *e = globalFileTable;
	vid_printf("Global File Table:\n");
	for(i = 0; i < FILE_COUNT; i++) {
		if(e->flags != 0) {
			sVFSNode *n = vfsn_getNode(e->nodeNo);
			vid_printf("\tfile @ index %d\n",i);
			vid_printf("\t\tread: %d\n",(e->flags & VFS_READ) ? true : false);
			vid_printf("\t\twrite: %d\n",(e->flags & VFS_WRITE) ? true : false);
			vid_printf("\t\tenv: %s\n",IS_VIRT(e->nodeNo) ? "virtual" : "real");
			vid_printf("\t\tnodeNo: %d\n",VIRT_INDEX(e->nodeNo));
			vid_printf("\t\tpos: %d\n",e->position);
			vid_printf("\t\trefCount: %d\n",e->refCount);
			vid_printf("\t\towner: %d\n",e->owner);
			if((n->mode & MODE_TYPE_SERVUSE))
				vid_printf("\t\tService-Usage of %s @ %s\n",n->name,n->parent->name);
		}
		e++;
	}
}

#endif
