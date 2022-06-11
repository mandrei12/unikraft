/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2006-2007, Kohsuke Ohtani
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 * Copyright (c) 2019, NEC Europe Ltd., NEC Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * procfs_vnops.c - vnode operations for proc file system.
 */
#define _GNU_SOURCE

#include <uk/essentials.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/param.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <uk/page.h>
#include <vfscore/vnode.h>
#include <vfscore/mount.h>
#include <vfscore/uio.h>
#include <vfscore/file.h>

#include "procfs.h"
#include <dirent.h>
#include <fcntl.h>
#include <vfscore/fs.h>

/* for the /proc/version */
#include <uk/version.h>
// #include <uk/banner.h>
#include <uk/config.h>



static struct uk_mutex procfs_lock = UK_MUTEX_INITIALIZER(procfs_lock);
static uint64_t inode_count = 1; /* inode 0 is reserved to root */

static void
set_times_to_now(struct timespec *time1, struct timespec *time2,
		 struct timespec *time3)
{
	struct timespec now = {0, 0};

	/* TODO: implement the real clock_gettime */
	/* clock_gettime(CLOCK_REALTIME, &now); */
	if (time1)
		memcpy(time1, &now, sizeof(struct timespec));
	if (time2)
		memcpy(time2, &now, sizeof(struct timespec));
	if (time3)
		memcpy(time3, &now, sizeof(struct timespec));
}

struct procfs_node *
procfs_allocate_node(const char *name, int type)
{
	struct procfs_node *np;

	np = malloc(sizeof(struct procfs_node));
	if (np == NULL)
		return NULL;
	memset(np, 0, sizeof(struct procfs_node));

	np->rn_namelen = strlen(name);
	np->rn_name = (char *) malloc(np->rn_namelen + 1);
	if (np->rn_name == NULL) {
		free(np);
		return NULL;
	}
	strlcpy(np->rn_name, name, np->rn_namelen + 1);
	np->rn_type = type;

	if (type == VDIR)
		np->rn_mode = S_IFDIR|0777;
	else if (type == VLNK)
		np->rn_mode = S_IFLNK|0777;
	else
		np->rn_mode = S_IFREG|0777;

	set_times_to_now(&(np->rn_ctime), &(np->rn_atime), &(np->rn_mtime));
	np->rn_owns_buf = true;

	return np;
}

void
procfs_free_node(struct procfs_node *np)
{
	if (np->rn_buf != NULL && np->rn_owns_buf)
		free(np->rn_buf);

	free(np->rn_name);
	free(np);
}

static struct procfs_node *
procfs_add_node(struct procfs_node *dnp, char *name, int type)
{
	struct procfs_node *np, *prev;

	uk_pr_debug("i enter procfs_add_node with name %s\n", name);

	np = procfs_allocate_node(name, type);
	if (np == NULL)
		return NULL;

	uk_mutex_lock(&procfs_lock);

	/* Link to the directory list */
	if (dnp->rn_child == NULL) {
		dnp->rn_child = np;
	} else {
		prev = dnp->rn_child;
		while (prev->rn_next != NULL)
			prev = prev->rn_next;
		prev->rn_next = np;
	}

	set_times_to_now(&(dnp->rn_mtime), &(dnp->rn_ctime), NULL);

	uk_mutex_unlock(&procfs_lock);
	return np;
}

static int
procfs_remove_node(struct procfs_node *dnp, struct procfs_node *np)
{
	struct procfs_node *prev;

	if (dnp->rn_child == NULL)
		return EBUSY;

	uk_mutex_lock(&procfs_lock);

	/* Unlink from the directory list */
	if (dnp->rn_child == np) {
		dnp->rn_child = np->rn_next;
	} else {
		for (prev = dnp->rn_child; prev->rn_next != np;
			 prev = prev->rn_next) {
			if (prev->rn_next == NULL) {
				uk_mutex_unlock(&procfs_lock);
				return ENOENT;
			}
		}
		prev->rn_next = np->rn_next;
	}
	procfs_free_node(np);

	set_times_to_now(&(dnp->rn_mtime), &(dnp->rn_ctime), NULL);

	uk_mutex_unlock(&procfs_lock);
	return 0;
}


static int
procfs_lookup(struct vnode *dvp, char *name, struct vnode **vpp)
{
	struct procfs_node *np, *dnp;
	struct vnode *vp;
	size_t len;
	int found;

	*vpp = NULL;

	if (*name == '\0')
		return ENOENT;

	uk_mutex_lock(&procfs_lock);

	len = strlen(name);
	dnp = dvp->v_data;
	found = 0;
	for (np = dnp->rn_child; np != NULL; np = np->rn_next) {
		if (np->rn_namelen == len &&
			memcmp(name, np->rn_name, len) == 0) {
			found = 1;
			break;
		}
	}
	if (found == 0) {
		uk_mutex_unlock(&procfs_lock);
		return ENOENT;
	}
	if (vfscore_vget(dvp->v_mount, inode_count++, &vp)) {
		/* found in cache */
		*vpp = vp;
		uk_mutex_unlock(&procfs_lock);
		return 0;
	}
	if (!vp) {
		uk_mutex_unlock(&procfs_lock);
		return ENOMEM;
	}
	vp->v_data = np;
	vp->v_mode = UK_ALLPERMS;
	vp->v_type = np->rn_type;
	vp->v_size = np->rn_size;

	uk_mutex_unlock(&procfs_lock);

	*vpp = vp;

	return 0;
}


/* Remove a file */
static int
procfs_remove(struct vnode *dvp, struct vnode *vp, char *name __maybe_unused)
{
	uk_pr_debug("remove %s in %s\n", name,
		 PROCFS_NODE(dvp)->rn_name);
	return procfs_remove_node(dvp->v_data, vp->v_data);
}


/*
 * Create empty file.
 */
static int
procfs_create(struct vnode *dvp, char *name, mode_t mode)
{
	struct procfs_node *np;

	if (strlen(name) > NAME_MAX)
		return ENAMETOOLONG;

	uk_pr_debug("create %s in %s\n", name, PROCFS_NODE(dvp)->rn_name);
	if (!S_ISREG(mode))
		return EINVAL;

	np = procfs_add_node(dvp->v_data, name, VREG);
	if (np == NULL)
		return ENOMEM;
	return 0;
}

/* This function will be responsible for generating the meminfo data*/
static char *
read_meminfo(void)
{
	struct uk_alloc_stats *stats = malloc(sizeof(stats));
	char *to_read = malloc(1000 * sizeof(char));;

	uk_alloc_stats_get_global(stats);

	sprintf(to_read, 
            "last_alloc_size:\t%ld\n"
            "max_alloc_size:\t\t%ld\n"
            "min_alloc_size:\t\t%ld\n"
            "tot_nb_allocs:\t\t%ld\n"
            "tot_nb_frees:\t\t%ld\n"
            "cur_nb_allocs:\t\t%ld\n"
            "max_nb_allocs:\t\t%ld\n"
            "cur_mem_use:\t\t%ld\n"
            "max_mem_use:\t\t%ld\n"
            "nb_enomem:\t\t%ld\n",
			stats->last_alloc_size,
			stats->max_alloc_size,
			stats->min_alloc_size,
			stats->tot_nb_allocs,
			stats->tot_nb_frees,
			stats->cur_nb_allocs,
			stats->max_nb_allocs,
			stats->cur_mem_use,
			stats->max_mem_use,
			stats->nb_enomem
			);
	return to_read;
}

static char *
read_version(void)
{
	char *to_read = malloc(50 * sizeof(char));

	sprintf(to_read,
			"Unikraft "
			STRINGIFY(UK_CODENAME) " "
			STRINGIFY(UK_FULLVERSION) "\n"
			);
	// printf("Unikraft "
	// 	STRINGIFY(UK_CODENAME) " "
	// 	STRINGIFY(UK_FULLVERSION) "\n");

	// uk_version();

	return to_read;
}

static int
procfs_read(struct vnode *vp, struct vfscore_file *fp __unused,
	   struct uio *uio, int ioflag __unused)
{
	struct procfs_node *np =  vp->v_data;
	size_t len;
	char *return_me;

	uk_pr_debug("%s: path=%s\n", __func__, fp->f_dentry->d_path);
	

	if (vp->v_size - uio->uio_offset < uio->uio_resid)
		len = vp->v_size - uio->uio_offset;
	else
		len = uio->uio_resid;

	set_times_to_now(&(np->rn_atime), NULL, NULL);

	// switch (fp->f_dentry->d_path) {
	// case "/meminfo":
	// 	uk_pr_debug("\n\nI have identified meminfo file\n\n");
	// 	return_me = read_meminfo();
	// 	return vfscore_uiomove(return_me, strlen(return_me) + 1, uio);
	// 	break;
	// case "/version":
	// 	uk_pr_debug("\n\nI have identified version file\n\n");
	// 	return_me = read_version();
	// 	return vfscore_uiomove(return_me, strlen(return_me) + 1, uio);
	// 	break;
	// default:
	// 	return vfscore_uiomove(np->rn_buf + uio->uio_offset, len, uio);
	// 	break;
	// }	
	if (strcmp(fp->f_dentry->d_path, "/meminfo") == 0) {
		uk_pr_debug("\n\nI have identified meminfo file\n\n");
		return_me = read_meminfo();
		return vfscore_uiomove(return_me, strlen(return_me) + 1, uio);
	}

	if (strcmp(fp->f_dentry->d_path, "/procinfo") == 0) {
		uk_pr_debug("\n\nI have identified procinfo file\n\n");
		// return_me = read_meminfo();
		return vfscore_uiomove("aaa", 5, uio);
	}

	if (strcmp(fp->f_dentry->d_path, "/version") == 0) {
		uk_pr_debug("\n\nI have identified version file\n\n");
		return_me = read_version();
		return vfscore_uiomove(return_me, strlen(return_me) + 1, uio);
		// uk_version();
		// return vfscore_uiomove("", 1, uio);
	}

	return vfscore_uiomove(np->rn_buf + uio->uio_offset, len, uio);
}


static int
procfs_write(struct vnode *vp, struct uio *uio, int ioflag)
{
	struct procfs_node *np =  vp->v_data;

	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EINVAL;
	if (uio->uio_offset < 0)
		return EINVAL;
	if (uio->uio_offset >= LONG_MAX)
		return EFBIG;
	if (uio->uio_resid == 0)
		return 0;

	if (ioflag & IO_APPEND)
		uio->uio_offset = np->rn_size;

	if ((size_t) uio->uio_offset + uio->uio_resid > (size_t) vp->v_size) {
		/* Expand the file size before writing to it */
		off_t end_pos = uio->uio_offset + uio->uio_resid;

		if (end_pos > (off_t) np->rn_bufsize) {
			// XXX: this could use a page level allocator
			size_t new_size = round_pgup(end_pos);
			void *new_buf = calloc(1, new_size);

			if (!new_buf)
				return EIO;
			if (np->rn_size != 0) {
				memcpy(new_buf, np->rn_buf, vp->v_size);
				if (np->rn_owns_buf)
					free(np->rn_buf);
			}
			np->rn_buf = (char *) new_buf;
			np->rn_bufsize = new_size;
		}
		np->rn_size = end_pos;
		vp->v_size = end_pos;
		np->rn_owns_buf = true;
	}

	set_times_to_now(&(np->rn_mtime), &(np->rn_ctime), NULL);
	return vfscore_uiomove(np->rn_buf + uio->uio_offset, uio->uio_resid,
			       uio);
}

/*
 * @vp: vnode of the directory.
 */
static int
procfs_readdir(struct vnode *vp, struct vfscore_file *fp, struct dirent *dir)
{
	struct procfs_node *np, *dnp;
	int i;

	uk_mutex_lock(&procfs_lock);

	set_times_to_now(&(((struct procfs_node *) vp->v_data)->rn_atime),
			 NULL, NULL);

	if (fp->f_offset == 0) {
		dir->d_type = DT_DIR;
		strlcpy((char *) &dir->d_name, ".", sizeof(dir->d_name));
	} else if (fp->f_offset == 1) {
		dir->d_type = DT_DIR;
		strlcpy((char *) &dir->d_name, "..", sizeof(dir->d_name));
	} else {
		dnp = vp->v_data;
		np = dnp->rn_child;
		if (np == NULL) {
			uk_mutex_unlock(&procfs_lock);
			return ENOENT;
		}

		for (i = 0; i != (fp->f_offset - 2); i++) {
			np = np->rn_next;
			if (np == NULL) {
				uk_mutex_unlock(&procfs_lock);
				return ENOENT;
			}
		}
		if (np->rn_type == VDIR)
			dir->d_type = DT_DIR;
		else if (np->rn_type == VLNK)
			dir->d_type = DT_LNK;
		else
			dir->d_type = DT_REG;
		strlcpy((char *) &dir->d_name, np->rn_name,
				sizeof(dir->d_name));
	}
	dir->d_fileno = fp->f_offset;
//	dir->d_namelen = strlen(dir->d_name);

	fp->f_offset++;

	uk_mutex_unlock(&procfs_lock);
	return 0;
}

int
procfs_init(void)
{
	return 0;
}

static int
procfs_getattr(struct vnode *vnode, struct vattr *attr)
{
	struct procfs_node *np = vnode->v_data;

	attr->va_nodeid = vnode->v_ino;
	attr->va_size = vnode->v_size;

	attr->va_type = np->rn_type;

	memcpy(&(attr->va_atime), &(np->rn_atime), sizeof(struct timespec));
	memcpy(&(attr->va_ctime), &(np->rn_ctime), sizeof(struct timespec));
	memcpy(&(attr->va_mtime), &(np->rn_mtime), sizeof(struct timespec));

	attr->va_mode = np->rn_mode;

	return 0;
}

static int
procfs_open(struct vfscore_file *fp)
{
	char *path = fp->f_dentry->d_path;

	uk_pr_debug("%s: path=%s\n", __func__, path);
	uk_pr_debug("-------------------\n");

	return 0;
}

static int
procfs_close(struct vnode *vp __unused, struct vfscore_file *fp)
{

	uk_pr_debug("%s: fd=%d\n", __func__, fp->fd);

	return 0;
}


#define procfs_seek			((vnop_seek_t)vfscore_vop_nullop)
#define procfs_ioctl		((vnop_ioctl_t)vfscore_vop_einval)
#define procfs_fsync		((vnop_fsync_t)vfscore_vop_nullop)
#define procfs_inactive		((vnop_inactive_t)vfscore_vop_nullop)
#define procfs_link			((vnop_link_t)vfscore_vop_eperm)
#define procfs_fallocate	((vnop_fallocate_t)vfscore_vop_nullop)
#define procfs_truncate		((vnop_truncate_t)vfscore_vop_nullop)
#define procfs_rename		((vnop_rename_t)vfscore_vop_einval)
#define procfs_setattr		((vnop_setattr_t)vfscore_vop_nullop)
#define procfs_readlink		((vnop_readlink_t)vfscore_vop_einval)
#define procfs_mkdir		((vnop_mkdir_t)vfscore_vop_einval)
#define procfs_rmdir		((vnop_rmdir_t)vfscore_vop_einval)
#define procfs_symlink		((vnop_symlink_t)vfscore_vop_eperm)


/*
 * vnode operations
 */
struct vnops procfs_vnops = {
		procfs_open,             /* open */
		procfs_close,            /* close */
		procfs_read,             /* read */
		procfs_write,            /* write */
		procfs_seek,             /* seek */
		procfs_ioctl,            /* ioctl */
		procfs_fsync,            /* fsync */
		procfs_readdir,          /* readdir */
		procfs_lookup,           /* lookup */
		procfs_create,           /* create */
		procfs_remove,           /* remove */
		procfs_rename,           /* remame */
		procfs_mkdir,            /* mkdir */
		procfs_rmdir,            /* rmdir */
		procfs_getattr,          /* getattr */
		procfs_setattr,          /* setattr */
		procfs_inactive,         /* inactive */
		procfs_truncate,         /* truncate */
		procfs_link,             /* link */
		(vnop_cache_t) NULL,    /* arc */
		procfs_fallocate,        /* fallocate */
		procfs_readlink,         /* read link */
		procfs_symlink,          /* symbolic link */
};

