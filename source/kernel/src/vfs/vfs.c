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
#include <sys/vfs/vfs.h>
#include <sys/vfs/node.h>
#include <sys/vfs/real.h>
#include <sys/vfs/info.h>
#include <sys/vfs/request.h>
#include <sys/vfs/driver.h>
#include <sys/vfs/file.h>
#include <sys/vfs/dir.h>
#include <sys/vfs/link.h>
#include <sys/vfs/channel.h>
#include <sys/vfs/pipe.h>
#include <sys/vfs/server.h>
#include <sys/task/proc.h>
#include <sys/task/event.h>
#include <sys/mem/paging.h>
#include <sys/mem/cache.h>
#include <sys/mem/dynarray.h>
#include <sys/util.h>
#include <sys/video.h>
#include <esc/messages.h>
#include <esc/sllist.h>
#include <string.h>
#include <assert.h>
#include <errors.h>

#define FILE_COUNT					((file_t)(gftArray.objCount))

/* an entry in the global file table */
typedef struct sGFTEntry {
	/* read OR write; flags = 0 => entry unused */
	ushort flags;
	/* the owner of this file */
	pid_t owner;
	/* number of references */
	ushort refCount;
	/* current position in file */
	off_t position;
	/* node-number */
	inode_t nodeNo;
	/* the node, if devNo == VFS_DEV_NO, otherwise null */
	sVFSNode *node;
	/* the device-number */
	dev_t devNo;
	/* for the freelist */
	struct sGFTEntry *next;
} sGFTEntry;

/**
 * Searches for a free file for the given flags and node-number
 */
static file_t vfs_getFreeFile(pid_t pid,ushort flags,inode_t nodeNo,dev_t devNo,sVFSNode *n);
/**
 * Releases the given file
 */
static void vfs_releaseFile(sGFTEntry *e);

/* global file table (expands dynamically) */
static sDynArray gftArray;
static sGFTEntry *gftFreeList;
static sVFSNode *procsNode;
static sVFSNode *devNode;

void vfs_init(void) {
	sVFSNode *root,*sys;
	dyna_start(&gftArray,sizeof(sGFTEntry),GFT_AREA,GFT_AREA_SIZE);
	gftFreeList = NULL;
	vfs_node_init();

	/*
	 *  /
	 *   system
	 *     |-pipe
	 *     |-processes
	 *     |-devices
	 *   dev
	 */
	root = vfs_dir_create(KERNEL_PID,NULL,(char*)"");
	sys = vfs_dir_create(KERNEL_PID,root,(char*)"system");
	vfs_dir_create(KERNEL_PID,sys,(char*)"pipe");
	procsNode = vfs_dir_create(KERNEL_PID,sys,(char*)"processes");
	vfs_dir_create(KERNEL_PID,sys,(char*)"devices");
	devNode = vfs_dir_create(KERNEL_PID,root,(char*)"dev");
}

int vfs_hasAccess(pid_t pid,sVFSNode *n,ushort flags) {
	sProc *p;
	uint mode;
	if(n->name == NULL)
		return ERR_INVALID_FILE;
	/* kernel is allmighty :P */
	if(pid == KERNEL_PID)
		return 0;

	p = proc_getByPid(pid);
	/* root is (nearly) allmighty as well */
	if(p->euid == ROOT_UID) {
		/* root has exec-permission if at least one has exec-permission */
		if(flags & VFS_EXEC)
			return (n->mode & MODE_EXEC) ? 0 : ERR_NO_EXEC_PERM;
		return 0;
	}

	/* determine mask */
	if(p->euid == n->uid)
		mode = n->mode & S_IRWXU;
	else if(p->egid == n->gid || groups_contains(p->groups,n->gid))
		mode = n->mode & S_IRWXG;
	else
		mode = n->mode & S_IRWXO;

	/* check access */
	if((flags & VFS_READ) && !(mode & MODE_READ))
		return ERR_NO_READ_PERM;
	if((flags & VFS_WRITE) && !(mode & MODE_WRITE))
		return ERR_NO_WRITE_PERM;
	if((flags & VFS_EXEC) && !(mode & MODE_EXEC))
		return ERR_NO_EXEC_PERM;
	return 0;
}

static sGFTEntry *vfs_getGFTEntry(file_t file) {
	return (sGFTEntry*)dyna_getObj(&gftArray,file);
}

bool vfs_isDriver(file_t file) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	assert(file >= 0 && file < FILE_COUNT);
	return e->flags & VFS_DRIVER;
}

int vfs_incRefs(file_t file) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	if(file < 0 || file >= FILE_COUNT || e->flags == 0)
		return ERR_INVALID_FILE;
	e->refCount++;
	return 0;
}

sVFSNode *vfs_getVNode(file_t file) {
	sVFSNode *n;
	sGFTEntry *e = vfs_getGFTEntry(file);
	if(file < 0 || file >= FILE_COUNT || e->flags == 0)
		return NULL;
	n = e->node;
	if(n->name == NULL)
		return NULL;
	return n;
}

int vfs_getFileId(file_t file,inode_t *ino,dev_t *dev) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	if(file < 0 || file >= FILE_COUNT || e->flags == 0)
		return ERR_INVALID_FILE;

	*ino = e->nodeNo;
	*dev = e->devNo;
	return 0;
}

int vfs_fcntl(pid_t pid,file_t file,uint cmd,int arg) {
	UNUSED(pid);
	sGFTEntry *e = vfs_getGFTEntry(file);
	assert(file >= 0 && file < FILE_COUNT);
	switch(cmd) {
		case F_GETACCESS:
			return e->flags & (VFS_READ | VFS_WRITE | VFS_MSGS);
		case F_GETFL:
			return e->flags & VFS_NOBLOCK;
		case F_SETFL:
			e->flags &= VFS_READ | VFS_WRITE | VFS_MSGS | VFS_CREATE | VFS_DRIVER;
			e->flags |= arg & VFS_NOBLOCK;
			return 0;
		case F_SETDATA: {
			sVFSNode *n = e->node;
			if(e->devNo != VFS_DEV_NO || n->name == NULL || !IS_DRIVER(n->mode))
				return ERR_INVALID_ARGS;
			return vfs_server_setReadable(n,(bool)arg);
		}
	}
	return ERR_INVALID_ARGS;
}

bool vfs_shouldBlock(file_t file) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	assert(file >= 0 && file < FILE_COUNT);
	return !(e->flags & VFS_NOBLOCK);
}

file_t vfs_openPath(pid_t pid,ushort flags,const char *path) {
	char apath[MAX_PATH_LEN + 1];
	inode_t nodeNo;
	file_t file;
	bool created;
	sVFSNode *node = NULL;
	int err;

	/* resolve path */
	path = vfs_node_absolutize(apath,sizeof(apath),path);
	err = vfs_node_resolvePath(path,&nodeNo,&created,flags);
	if(err == ERR_REAL_PATH) {
		/* unfortunatly we have to check for the process-ids of ata and fs here. because e.g.
		 * if the user tries to mount the device "/realfile" the userspace has no opportunity
		 * to distinguish between virtual and real files. therefore fs will try to open this
		 * path and shoot itself in the foot... */
		if(pid == DISK_PID || pid == FS_PID)
			return ERR_PATH_NOT_FOUND;

		/* send msg to fs and wait for reply */
		file = vfs_real_openPath(pid,flags,path);
		if(file < 0)
			return file;
	}
	else {
		/* handle virtual files */
		node = vfs_node_get(nodeNo);
		if(err < 0)
			return err;

		/* if its a driver, create the channel-node */
		if(IS_DRIVER(node->mode)) {
			sVFSNode *child;
			/* check if we can access the driver */
			if((err = vfs_hasAccess(pid,node,flags)) < 0)
				return err;
			child = vfs_chan_create(pid,node);
			if(child == NULL)
				return ERR_NOT_ENOUGH_MEM;
			node = child;
			nodeNo = vfs_node_getNo(node);
		}

		/* open file */
		file = vfs_openFile(pid,flags,nodeNo,VFS_DEV_NO);
		if(file < 0)
			return file;

		/* if it is a driver, call the driver open-command */
		if(IS_CHANNEL(node->mode)) {
			err = vfs_drv_open(pid,file,node,flags);
			if(err < 0) {
				/* close removes the driver-usage-node, if it is one */
				vfs_closeFile(pid,file);
				return err;
			}
		}
	}

	/* append? */
	if(flags & VFS_APPEND) {
		err = vfs_seek(pid,file,0,SEEK_END);
		if(err < 0) {
			vfs_closeFile(pid,file);
			return err;
		}
	}
	return file;
}

file_t vfs_openFile(pid_t pid,ushort flags,inode_t nodeNo,dev_t devNo) {
	sGFTEntry *e;
	sVFSNode *n = NULL;
	file_t f;

	/* cleanup flags */
	flags &= VFS_READ | VFS_WRITE | VFS_MSGS | VFS_NOBLOCK | VFS_DRIVER;

	if(devNo == VFS_DEV_NO) {
		int err;
		n = vfs_node_get(nodeNo);
		if((err = vfs_hasAccess(pid,n,flags)) < 0)
			return err;
	}

	/* determine free file */
	f = vfs_getFreeFile(pid,flags,nodeNo,devNo,n);
	if(f < 0)
		return f;

	e = vfs_getGFTEntry(f);
	/* unused file? */
	if(e->flags == 0) {
		/* count references of virtual nodes */
		if(devNo == VFS_DEV_NO) {
			n->refCount++;
			e->node = n;
		}
		else
			e->node = NULL;
		e->owner = pid;
		e->flags = flags;
		e->refCount = 1;
		e->position = 0;
		e->devNo = devNo;
		e->nodeNo = nodeNo;
	}
	else
		e->refCount++;
	return f;
}

static file_t vfs_getFreeFile(pid_t pid,ushort flags,inode_t nodeNo,dev_t devNo,sVFSNode *n) {
	const uint userFlags = VFS_READ | VFS_WRITE | VFS_MSGS | VFS_NOBLOCK | VFS_DRIVER;
	file_t i;
	bool isDrvUse = false;
	sGFTEntry *e;
	/* ensure that we don't increment usages of an unused slot */
	assert(flags & (VFS_READ | VFS_WRITE | VFS_MSGS));
	assert(!(flags & ~userFlags));

	if(devNo == VFS_DEV_NO) {
		/* we can add pipes here, too, since every open() to a pipe will get a new node anyway */
		isDrvUse = (n->mode & (MODE_TYPE_CHANNEL | MODE_TYPE_PIPE)) ? true : false;
	}

	/* for drivers it doesn't matter whether we use an existing file or a new one, because it is
	 * no problem when multiple threads use it for writing */
	if(!isDrvUse) {
		ushort rwFlags = flags & userFlags;
		for(i = 0; i < FILE_COUNT; i++) {
			e = vfs_getGFTEntry(i);
			/* used slot and same node? */
			if(e->flags != 0) {
				/* same file? */
				if(e->devNo == devNo && e->nodeNo == nodeNo) {
					if(e->owner == pid) {
						/* if the flags are the same we don't need a new file */
						if((e->flags & userFlags) == rwFlags)
							return i;
					}
					/* two procs that want to write at the same time? no! */
					else if(!isDrvUse && (rwFlags & VFS_WRITE) && (e->flags & VFS_WRITE))
						return ERR_FILE_IN_USE;
				}
			}
		}
	}

	/* if there is no free slot anymore, extend our dyn-array */
	if(gftFreeList == NULL) {
		i = gftArray.objCount;
		file_t j;
		if(!dyna_extend(&gftArray))
			return ERR_NO_FREE_FILE;
		/* put all except i on the freelist */
		for(j = i + 1; j < (file_t)gftArray.objCount; j++) {
			e = vfs_getGFTEntry(j);
			e->next = gftFreeList;
			gftFreeList = e;
		}
		return i;
	}

	/* use the first from the freelist */
	e = gftFreeList;
	gftFreeList = gftFreeList->next;
	return dyna_getIndex(&gftArray,e);
}

static void vfs_releaseFile(sGFTEntry *e) {
	e->next = gftFreeList;
	gftFreeList = e;
}

off_t vfs_tell(pid_t pid,file_t file) {
	UNUSED(pid);
	sGFTEntry *e = vfs_getGFTEntry(file);
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);
	return e->position;
}

int vfs_stat(pid_t pid,const char *path,sFileInfo *info) {
	char apath[MAX_PATH_LEN + 1];
	inode_t nodeNo;
	path = vfs_node_absolutize(apath,sizeof(apath),path);
	int res = vfs_node_resolvePath(path,&nodeNo,NULL,VFS_READ);
	if(res == ERR_REAL_PATH)
		res = vfs_real_stat(pid,path,info);
	else if(res == 0)
		res = vfs_node_getInfo(nodeNo,info);
	return res;
}

int vfs_fstat(pid_t pid,file_t file,sFileInfo *info) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	int res;
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);
	if(e->devNo == VFS_DEV_NO)
		res = vfs_node_getInfo(e->nodeNo,info);
	else
		res = vfs_real_istat(pid,e->nodeNo,e->devNo,info);
	return res;
}

int vfs_chmod(pid_t pid,const char *path,mode_t mode) {
	char apath[MAX_PATH_LEN + 1];
	inode_t nodeNo;
	path = vfs_node_absolutize(apath,sizeof(apath),path);
	int res = vfs_node_resolvePath(path,&nodeNo,NULL,VFS_READ);
	if(res == ERR_REAL_PATH)
		res = vfs_real_chmod(pid,path,mode);
	else if(res == 0)
		res = vfs_node_chmod(pid,nodeNo,mode);
	return res;
}

int vfs_chown(pid_t pid,const char *path,uid_t uid,gid_t gid) {
	char apath[MAX_PATH_LEN + 1];
	inode_t nodeNo;
	path = vfs_node_absolutize(apath,sizeof(apath),path);
	int res = vfs_node_resolvePath(path,&nodeNo,NULL,VFS_READ);
	if(res == ERR_REAL_PATH)
		res = vfs_real_chown(pid,path,uid,gid);
	else if(res == 0)
		res = vfs_node_chown(pid,nodeNo,uid,gid);
	return res;
}

off_t vfs_seek(pid_t pid,file_t file,off_t offset,uint whence) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	off_t oldPos = e->position;
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	if(e->devNo == VFS_DEV_NO) {
		sVFSNode *n = e->node;
		/* node not present anymore */
		if(n->name == NULL)
			return ERR_INVALID_FILE;
		if(n->seek == NULL)
			return ERR_UNSUPPORTED_OP;
		e->position = n->seek(pid,n,e->position,offset,whence);
	}
	else {
		if(whence == SEEK_END) {
			sFileInfo info;
			int res = vfs_real_istat(pid,e->nodeNo,e->devNo,&info);
			if(res < 0)
				return res;
			e->position = info.size;
		}
		/* since the fs-driver validates the position anyway we can simply set it */
		else if(whence == SEEK_SET)
			e->position = offset;
		else
			e->position += offset;
	}

	if(e->position < 0) {
		e->position = oldPos;
		return ERR_INVALID_ARGS;
	}
	return e->position;
}

ssize_t vfs_readFile(pid_t pid,file_t file,void *buffer,size_t count) {
	ssize_t readBytes;
	sGFTEntry *e = vfs_getGFTEntry(file);
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	if(!(e->flags & VFS_READ))
		return ERR_NO_READ_PERM;

	if(e->devNo == VFS_DEV_NO) {
		sVFSNode *n = e->node;
		if(n->name == NULL)
			return ERR_INVALID_FILE;
		if(n->read == NULL)
			return ERR_NO_READ_PERM;

		/* use the read-handler */
		readBytes = n->read(pid,file,n,buffer,e->position,count);
		if(readBytes > 0)
			e->position += readBytes;
	}
	else {
		/* query the fs-driver to read from the inode */
		readBytes = vfs_real_read(pid,e->nodeNo,e->devNo,buffer,e->position,count);
		if(readBytes > 0)
			e->position += readBytes;
	}

	if(readBytes > 0 && pid != KERNEL_PID) {
		sProc *p = proc_getByPid(pid);
		p->stats.input += readBytes;
	}
	return readBytes;
}

ssize_t vfs_writeFile(pid_t pid,file_t file,const void *buffer,size_t count) {
	ssize_t writtenBytes;
	sGFTEntry *e = vfs_getGFTEntry(file);
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	if(!(e->flags & VFS_WRITE))
		return ERR_NO_WRITE_PERM;

	if(e->devNo == VFS_DEV_NO) {
		sVFSNode *n = e->node;
		if(n->name == NULL)
			return ERR_INVALID_FILE;
		if(n->write == NULL)
			return ERR_NO_WRITE_PERM;

		/* write to the node */
		writtenBytes = n->write(pid,file,n,buffer,e->position,count);
		if(writtenBytes > 0)
			e->position += writtenBytes;
	}
	else {
		/* query the fs-driver to write to the inode */
		writtenBytes = vfs_real_write(pid,e->nodeNo,e->devNo,buffer,e->position,count);
		if(writtenBytes > 0)
			e->position += writtenBytes;
	}

	if(writtenBytes > 0 && pid != KERNEL_PID) {
		sProc *p = proc_getByPid(pid);
		p->stats.output += writtenBytes;
	}
	return writtenBytes;
}

ssize_t vfs_sendMsg(pid_t pid,file_t file,msgid_t id,const void *data,size_t size) {
	ssize_t err;
	sGFTEntry *e = vfs_getGFTEntry(file);
	sVFSNode *n;
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	if(e->devNo != VFS_DEV_NO)
		return ERR_INVALID_FILE;
	/* the driver-messages (open, read, write, close) are always allowed */
	if(!IS_DRIVER_MSG(id) && !(e->flags & VFS_MSGS))
		return ERR_NO_EXEC_PERM;

	/* send the message */
	n = e->node;
	if(n->name == NULL)
		return ERR_INVALID_FILE;
	if(!IS_CHANNEL(n->mode))
		return ERR_UNSUPPORTED_OP;
	err = vfs_chan_send(pid,file,n,id,data,size);

	if(err == 0 && pid != KERNEL_PID) {
		sProc *p = proc_getByPid(pid);
		p->stats.output += size;
	}
	return err;
}

ssize_t vfs_receiveMsg(pid_t pid,file_t file,msgid_t *id,void *data,size_t size) {
	ssize_t err;
	sGFTEntry *e = vfs_getGFTEntry(file);
	sVFSNode *n;
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	if(e->devNo != VFS_DEV_NO)
		return ERR_INVALID_FILE;

	/* receive the message */
	n = e->node;
	if(n->name == NULL)
		return ERR_INVALID_FILE;
	if(!IS_CHANNEL(n->mode))
		return ERR_UNSUPPORTED_OP;
	err = vfs_chan_receive(pid,file,n,id,data,size);

	if(err > 0 && pid != KERNEL_PID) {
		sProc *p = proc_getByPid(pid);
		p->stats.input += err;
	}
	return err;
}

void vfs_closeFile(pid_t pid,file_t file) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	/* decrement references */
	if(--(e->refCount) == 0) {
		if(e->devNo == VFS_DEV_NO) {
			sVFSNode *n = e->node;
			if(n->name != NULL) {
				n->refCount--;
				if(n->close)
					n->close(pid,file,n);
			}
		}
		else
			vfs_real_close(pid,e->nodeNo,e->devNo);

		/* mark unused */
		e->flags = 0;
		vfs_releaseFile(e);
	}
}

int vfs_link(pid_t pid,const char *oldPath,const char *newPath) {
	char newPathCpy[MAX_PATH_LEN + 1];
	char *name,*namecpy,backup;
	size_t len;
	inode_t oldIno,newIno;
	sVFSNode *dir,*target;
	int oldRes,newRes;
	/* first check whether it is a realpath */
	oldRes = vfs_node_resolvePath(oldPath,&oldIno,NULL,VFS_READ);
	newRes = vfs_node_resolvePath(newPath,&newIno,NULL,VFS_WRITE);
	if(oldRes == ERR_REAL_PATH) {
		char *absNew,*absOld;
		if(newRes != ERR_REAL_PATH)
			return ERR_LINK_DEVICE;

		/* absolutize the paths */
		absOld = (char*)cache_alloc(MAX_PATH_LEN + 1);
		if(!absOld)
			return ERR_NOT_ENOUGH_MEM;
		absNew = (char*)cache_alloc(MAX_PATH_LEN + 1);
		if(!absNew) {
			cache_free(absOld);
			return ERR_NOT_ENOUGH_MEM;
		}
		oldPath = vfs_node_absolutize(absOld,MAX_PATH_LEN + 1,oldPath);
		newPath = vfs_node_absolutize(absNew,MAX_PATH_LEN + 1,newPath);
		oldRes = vfs_real_link(pid,oldPath,newPath);
		cache_free(absOld);
		cache_free(absNew);
		return oldRes;
	}
	if(oldRes < 0)
		return oldRes;
	if(newRes >= 0)
		return ERR_FILE_EXISTS;

	/* TODO check access-rights */

	/* copy path because we have to change it */
	len = strlen(newPath);
	if(len >= MAX_PATH_LEN)
		return ERR_INVALID_PATH;
	strcpy(newPathCpy,newPath);
	/* check whether the directory exists */
	name = vfs_node_basename((char*)newPathCpy,&len);
	backup = *name;
	vfs_node_dirname((char*)newPathCpy,len);
	newRes = vfs_node_resolvePath(newPathCpy,&newIno,NULL,VFS_WRITE);
	if(newRes < 0)
		return ERR_PATH_NOT_FOUND;

	/* links to directories not allowed */
	target = vfs_node_get(oldIno);
	if(S_ISDIR(target->mode))
		return ERR_IS_DIR;

	/* make copy of name */
	*name = backup;
	len = strlen(name);
	namecpy = cache_alloc(len + 1);
	if(namecpy == NULL)
		return ERR_NOT_ENOUGH_MEM;
	strcpy(namecpy,name);
	/* now create link */
	dir = vfs_node_get(newIno);
	/* file exists? */
	if(vfs_node_findInDir(dir,namecpy,len) != NULL) {
		cache_free(namecpy);
		return ERR_FILE_EXISTS;
	}
	if(vfs_link_create(pid,dir,namecpy,target) == NULL) {
		cache_free(namecpy);
		return ERR_NOT_ENOUGH_MEM;
	}
	return 0;
}

int vfs_unlink(pid_t pid,const char *path) {
	char apath[MAX_PATH_LEN + 1];
	int res;
	inode_t ino;
	sVFSNode *n;
	path = vfs_node_absolutize(apath,sizeof(apath),path);
	res = vfs_node_resolvePath(path,&ino,NULL,VFS_WRITE | VFS_NOLINKRES);
	if(res == ERR_REAL_PATH)
		return vfs_real_unlink(pid,path);
	if(res < 0)
		return ERR_PATH_NOT_FOUND;
	/* TODO check access-rights */
	n = vfs_node_get(ino);
	if(!S_ISREG(n->mode) && !S_ISLNK(n->mode))
		return ERR_NO_FILE_OR_LINK;
	vfs_node_destroy(n);
	return 0;
}

int vfs_mkdir(pid_t pid,const char *path) {
	char pathCpy[MAX_PATH_LEN + 1];
	char *name,*namecpy;
	char backup;
	int res;
	size_t len = strlen(path);
	inode_t inodeNo;
	sVFSNode *node,*child;

	/* copy path because we'll change it */
	if(len >= MAX_PATH_LEN)
		return ERR_INVALID_PATH;
	strcpy(pathCpy,path);

	/* extract name and directory */
	name = vfs_node_basename(pathCpy,&len);
	backup = *name;
	vfs_node_dirname(pathCpy,len);

	/* get the parent-directory */
	res = vfs_node_resolvePath(pathCpy,&inodeNo,NULL,VFS_WRITE);
	/* special-case: directories in / should be created in the real fs! */
	if(res == ERR_REAL_PATH || (res >= 0 && strcmp(pathCpy,"/") == 0)) {
		path = vfs_node_absolutize(pathCpy,sizeof(pathCpy),path);
		/* let fs handle the request */
		return vfs_real_mkdir(pid,path);
	}
	if(res < 0)
		return res;

	/* alloc space for name and copy it over */
	*name = backup;
	len = strlen(name);
	namecpy = cache_alloc(len + 1);
	if(namecpy == NULL)
		return ERR_NOT_ENOUGH_MEM;
	strcpy(namecpy,name);
	/* create dir */
	node = vfs_node_get(inodeNo);
	if(vfs_node_findInDir(node,namecpy,len) != NULL) {
		cache_free(namecpy);
		return ERR_FILE_EXISTS;
	}
	/* TODO check access-rights */
	child = vfs_dir_create(pid,node,namecpy);
	if(child == NULL) {
		cache_free(namecpy);
		return ERR_NOT_ENOUGH_MEM;
	}
	return 0;
}

int vfs_rmdir(pid_t pid,const char *path) {
	char apath[MAX_PATH_LEN + 1];
	int res;
	sVFSNode *node;
	inode_t inodeNo;
	path = vfs_node_absolutize(apath,sizeof(apath),path);
	res = vfs_node_resolvePath(path,&inodeNo,NULL,VFS_WRITE);
	if(res == ERR_REAL_PATH)
		return vfs_real_rmdir(pid,path);
	if(res < 0)
		return ERR_PATH_NOT_FOUND;

	/* TODO check access-rights */
	node = vfs_node_get(inodeNo);
	if(!S_ISDIR(node->mode))
		return ERR_NO_DIRECTORY;
	vfs_node_destroy(node);
	return 0;
}

file_t vfs_createDriver(pid_t pid,const char *name,uint flags) {
	sVFSNode *drv = devNode;
	sVFSNode *n = drv->firstChild;
	size_t len;
	char *hname;
	vassert(name != NULL,"name == NULL");

	/* TODO check permissions */

	/* we don't want to have exotic driver-names */
	if((len = strlen(name)) == 0 || !isalnumstr(name))
		return ERR_INV_DRIVER_NAME;

	while(n != NULL) {
		/* entry already existing? */
		if(strcmp(n->name,name) == 0)
			return ERR_DRIVER_EXISTS;
		n = n->next;
	}

	/* copy name to kernel-heap */
	hname = (char*)cache_alloc(len + 1);
	if(hname == NULL)
		return ERR_NOT_ENOUGH_MEM;
	strncpy(hname,name,len);
	hname[len] = '\0';

	/* create node */
	n = vfs_server_create(pid,drv,hname,flags);
	if(n != NULL)
		return vfs_openFile(pid,VFS_MSGS | VFS_DRIVER,vfs_node_getNo(n),VFS_DEV_NO);

	/* failed, so cleanup */
	cache_free(hname);
	return ERR_NOT_ENOUGH_MEM;
}

bool vfs_hasMsg(pid_t pid,file_t file) {
	UNUSED(pid);
	sGFTEntry *e = vfs_getGFTEntry(file);
	sVFSNode *n;
	assert(file >= 0 && file < FILE_COUNT);
	if(e->devNo != VFS_DEV_NO)
		return false;
	n = e->node;
	return n->name != NULL && IS_CHANNEL(n->mode) && vfs_chan_hasReply(n);
}

bool vfs_hasData(pid_t pid,file_t file) {
	UNUSED(pid);
	sGFTEntry *e = vfs_getGFTEntry(file);
	sVFSNode *n;
	assert(file >= 0 && file < FILE_COUNT);
	if(e->devNo != VFS_DEV_NO)
		return false;
	n = e->node;
	return n->name != NULL && IS_DRIVER(n->parent->mode) && vfs_server_isReadable(n->parent);
}

bool vfs_hasWork(pid_t pid,const file_t *files,size_t count) {
	UNUSED(pid);
	sVFSNode *node;
	size_t i;
	for(i = 0; i < count; i++) {
		sGFTEntry *e = vfs_getGFTEntry(files[i]);
		if(files[i] < 0 || files[i] >= FILE_COUNT || e->devNo != VFS_DEV_NO)
			continue;

		node = e->node;
		if(!IS_DRIVER(node->mode))
			continue;

		if(vfs_server_hasWork(node))
			return true;
	}
	return false;
}

inode_t vfs_getClient(pid_t pid,const file_t *files,size_t count,size_t *index) {
	UNUSED(pid);
	sVFSNode *node,*client,*match = NULL;
	size_t i;
	bool retry,cont = true;
start:
	retry = false;
	for(i = 0; cont && i < count; i++) {
		sGFTEntry *e = vfs_getGFTEntry(files[i]);
		if(files[i] < 0 || files[i] >= FILE_COUNT || e->devNo != VFS_DEV_NO)
			return ERR_INVALID_FILE;

		node = e->node;
		if(!IS_DRIVER(node->mode))
			return ERR_NOT_OWN_DRIVER;

		client = vfs_server_getWork(node,&cont,&retry);
		if(client) {
			if(index)
				*index = i;
			if(cont)
				match = client;
			else
				return vfs_node_getNo(client);
		}
	}
	/* if we have a match, use this one */
	if(match)
		return vfs_node_getNo(match);
	/* if not and we've skipped a client, try another time */
	if(retry)
		goto start;
	return ERR_NO_CLIENT_WAITING;
}

inode_t vfs_getClientId(pid_t pid,file_t file) {
	UNUSED(pid);
	sGFTEntry *e = vfs_getGFTEntry(file);
	sVFSNode *n = e->node;
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	if(e->devNo != VFS_DEV_NO || !IS_CHANNEL(n->mode))
		return ERR_INVALID_FILE;
	return e->nodeNo;
}

file_t vfs_openClient(pid_t pid,file_t file,inode_t clientId) {
	sGFTEntry *e = vfs_getGFTEntry(file);
	sVFSNode *n;
	vassert(file >= 0 && file < FILE_COUNT && e->flags != 0,"Invalid file %d",file);

	/* search for the client */
	n = vfs_node_getFirstChild(e->node);
	while(n != NULL) {
		if(vfs_node_getNo(n) == clientId)
			break;
		n = n->next;
	}
	if(n == NULL)
		return ERR_PATH_NOT_FOUND;

	/* open file */
	return vfs_openFile(pid,VFS_MSGS | VFS_DRIVER,vfs_node_getNo(n),VFS_DEV_NO);
}

sVFSNode *vfs_createProcess(pid_t pid,fRead handler) {
	char *name;
	sVFSNode *proc = procsNode;
	sVFSNode *n = proc->firstChild;
	sVFSNode *dir,*tdir;

	/* build name */
	name = (char*)cache_alloc(12);
	if(name == NULL)
		return NULL;

	itoa(name,12,pid);

	/* go to last entry */
	while(n != NULL) {
		/* entry already existing? */
		if(strcmp(n->name,name) == 0)
			goto errorName;
		n = n->next;
	}

	/* create dir */
	dir = vfs_dir_create(KERNEL_PID,proc,name);
	if(dir == NULL)
		goto errorName;

	/* create process-info-node */
	n = vfs_file_create(KERNEL_PID,dir,(char*)"info",handler,NULL);
	if(n == NULL)
		goto errorDir;

	/* create virt-mem-info-node */
	n = vfs_file_create(KERNEL_PID,dir,(char*)"virtmem",vfs_info_virtMemReadHandler,NULL);
	if(n == NULL)
		goto errorDir;

	/* create regions-info-node */
	n = vfs_file_create(KERNEL_PID,dir,(char*)"regions",vfs_info_regionsReadHandler,NULL);
	if(n == NULL)
		goto errorDir;

	/* create threads-dir */
	tdir = vfs_dir_create(KERNEL_PID,dir,(char*)"threads");
	if(tdir == NULL)
		goto errorDir;

	return tdir;

errorDir:
	vfs_node_destroy(dir);
errorName:
	cache_free(name);
	return NULL;
}

void vfs_removeProcess(pid_t pid) {
	/* remove from /system/processes */
	sProc *p = proc_getByPid(pid);
	vfs_node_destroy(p->threadDir->parent);
}

bool vfs_createThread(tid_t tid) {
	char *name;
	sVFSNode *n,*dir;
	sThread *t = thread_getById(tid);

	/* build name */
	name = (char*)cache_alloc(12);
	if(name == NULL)
		return false;
	itoa(name,12,tid);

	/* create dir */
	dir = vfs_dir_create(KERNEL_PID,t->proc->threadDir,name);
	if(dir == NULL)
		goto errorDir;

	/* create info-node */
	n = vfs_file_create(KERNEL_PID,dir,(char*)"info",vfs_info_threadReadHandler,NULL);
	if(n == NULL)
		goto errorInfo;

	/* create trace-node */
	n = vfs_file_create(KERNEL_PID,dir,(char*)"trace",vfs_info_traceReadHandler,NULL);
	if(n == NULL)
		goto errorInfo;
	return true;

errorInfo:
	vfs_node_destroy(dir);
errorDir:
	cache_free(name);
	return false;
}

void vfs_removeThread(tid_t tid) {
	sThread *t = thread_getById(tid);
	sVFSNode *n;
	char *name;

	/* build name */
	name = (char*)cache_alloc(12);
	if(name == NULL)
		return;
	itoa(name,12,tid);

	/* search for thread-node and remove it */
	n = vfs_node_getFirstChild(t->proc->threadDir);
	while(n != NULL) {
		if(strcmp(n->name,name) == 0) {
			vfs_node_destroy(n);
			break;
		}
		n = n->next;
	}

	cache_free(name);
}

size_t vfs_dbg_getGFTEntryCount(void) {
	file_t i;
	size_t count = 0;
	for(i = 0; i < FILE_COUNT; i++) {
		if(vfs_getGFTEntry(i)->flags != 0)
			count++;
	}
	return count;
}

void vfs_printMsgs(void) {
	sVFSNode *drv = vfs_node_getFirstChild(devNode);
	vid_printf("Messages:\n");
	while(drv != NULL) {
		if(IS_DRIVER(drv->mode))
			vfs_server_print(drv);
		drv = drv->next;
	}
}

void vfs_printGFT(void) {
	file_t i;
	sGFTEntry *e;
	vid_printf("Global File Table:\n");
	for(i = 0; i < FILE_COUNT; i++) {
		e = vfs_getGFTEntry(i);
		if(e->flags != 0) {
			vid_printf("\tfile @ index %d\n",i);
			vid_printf("\t\tflags: ");
			if(e->flags & VFS_READ)
				vid_printf("READ ");
			if(e->flags & VFS_WRITE)
				vid_printf("WRITE ");
			if(e->flags & VFS_NOBLOCK)
				vid_printf("NOBLOCK ");
			if(e->flags & VFS_DRIVER)
				vid_printf("DRIVER ");
			if(e->flags & VFS_MSGS)
				vid_printf("MSGS ");
			vid_printf("\n");
			vid_printf("\t\tnodeNo: %d\n",e->nodeNo);
			vid_printf("\t\tdevNo: %d\n",e->devNo);
			vid_printf("\t\tpos: %Od\n",e->position);
			vid_printf("\t\trefCount: %d\n",e->refCount);
			if(e->owner == KERNEL_PID)
				vid_printf("\t\towner: %d (kernel)\n",e->owner);
			else {
				sProc *p = proc_getByPid(e->owner);
				vid_printf("\t\towner: %d:%s\n",p->pid,p->command);
			}
			if(e->devNo == VFS_DEV_NO) {
				sVFSNode *n = e->node;
				if(IS_CHANNEL(n->mode))
					vid_printf("\t\tDriver-Usage: %s @ %s\n",n->name,n->parent->name);
				else
					vid_printf("\t\tFile: '%s'\n",vfs_node_getPath(vfs_node_getNo(n)));
			}
		}
	}
}
