/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

/* handle.c
 * ZFS object (file|dir) handle object
 */

#include "config.h"

#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <mntent.h>
#include "gsh_list.h"
#include "fsal.h"
#include "fsal_internal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_config.h"
#include "FSAL/fsal_commonlib.h"
#include "clovis_methods.h"
#include <stdbool.h>

/* helpers
 */

/* alloc_handle
 * allocate and fill in a handle
 * this uses malloc/free for the time being.
 */
libzfswrap_vfs_t *ZFSFSAL_GetVFS(clovis_file_handle_t *handle)
{
	/* Check for the zpool (index == 0) */
	if (handle->i_snap == 0)
		return p_snapshots[0].p_vfs;

	/* Handle the indirection */
	int i;

	for (i = 1; i < i_snapshots + 1; i++) {
		if (p_snapshots[i].index == handle->i_snap) {
			LogFullDebug(COMPONENT_FSAL,
				     "Looking up inside the snapshot n°%d",
				     handle->i_snap);
			return p_snapshots[i].p_vfs;
		}
	}

	LogMajor(COMPONENT_FSAL, "Unable to get the right VFS");
	return NULL;
}

static struct clovis_fsal_obj_handle *alloc_handle(struct clovis_file_handle *fh,
						struct stat *stat,
						const char *link_content,
						struct fsal_export *exp_hdl)
{
	struct clovis_fsal_obj_handle *hdl;

	hdl = gsh_malloc(sizeof(struct clovis_fsal_obj_handle) +
			 sizeof(struct clovis_file_handle));

	memset(hdl, 0,
	       (sizeof(struct clovis_fsal_obj_handle) +
		sizeof(struct clovis_file_handle)));
	hdl->handle = (struct clovis_file_handle *)&hdl[1];
	memcpy(hdl->handle, fh, sizeof(struct clovis_file_handle));

	hdl->obj_handle.attrs = &hdl->attributes;
	hdl->obj_handle.type = posix2fsal_type(stat->st_mode);

	if ((hdl->obj_handle.type == SYMBOLIC_LINK) &&
	    (link_content != NULL)) {
		size_t len = strlen(link_content) + 1;

		hdl->u.symlink.link_content = gsh_malloc(len);
		memcpy(hdl->u.symlink.link_content, link_content, len);
		hdl->u.symlink.link_size = len;
	}

	hdl->attributes.mask = exp_hdl->exp_ops.fs_supported_attrs(exp_hdl);

	posix2fsal_attributes(stat, &hdl->attributes);

	fsal_obj_handle_init(&hdl->obj_handle,
			     exp_hdl,
			     posix2fsal_type(stat->st_mode));
	clovis_handle_ops_init(&hdl->obj_handle.obj_ops);
	return hdl;
}

/* handle methods
 */

/* lookup
 * deprecated NULL parent && NULL path implies root handle
 */

static fsal_status_t clovis_lookup(struct fsal_obj_handle *parent,
				 const char *path,
				 struct fsal_obj_handle **handle)
{
	struct clovis_fsal_obj_handle *parent_hdl, *hdl;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval;
	struct stat stat;
	struct clovis_file_handle fh;
	creden_t cred;
	libzfswrap_vfs_t *p_vfs = NULL;
	inogen_t object;
	int type;

	if (!path)
		return fsalstat(ERR_FSAL_FAULT, 0);
	memset(&fh, 0, sizeof(struct clovis_file_handle));
	parent_hdl =
	    container_of(parent, struct clovis_fsal_obj_handle, obj_handle);
	if (!parent->obj_ops.handle_is(parent, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p", parent);
		return fsalstat(ERR_FSAL_NOTDIR, 0);
	}

	/* >> Call your filesystem lookup function here << */
	/* >> Be carefull you don't traverse junction nor follow symlinks << */

	p_vfs = ZFSFSAL_GetVFS(parent_hdl->handle);

	/* Hook to add the hability to go inside a .zfs directory
	 * inside the root dir */
	if (parent_hdl->handle->clovis_handle.inode == 3
	    && !strcmp(path, ZFS_SNAP_DIR)) {

		LogDebug(COMPONENT_FSAL,
			 "Lookup for the .zfs/ pseudo-directory");

		object.inode = ZFS_SNAP_DIR_INODE;
		object.generation = 0;
		type = S_IFDIR;
		retval = 0;
	}

	/* Hook for the files inside the .zfs directory */
	else if (parent_hdl->handle->clovis_handle.inode == ZFS_SNAP_DIR_INODE) {
		LogDebug(COMPONENT_FSAL,
			 "Lookup inside the .zfs/ pseudo-directory");

		int i;

		for (i = 1; i < i_snapshots + 1; i++)
			if (!strcmp(p_snapshots[i].psz_name, path) &&
			    (i == i_snapshots + 1))
				return fsalstat(ERR_FSAL_NOTDIR, 0);

		libzfswrap_getroot(p_snapshots[i].p_vfs, &object);
		p_vfs = p_snapshots[i].p_vfs;

		type = S_IFDIR;
		retval = 0;
	} else {
		/* Get the right VFS */
		if (!p_vfs) {
			retval = ENOENT;
			goto errout;
		} else {

			cred.uid = op_ctx->creds->caller_uid;
			cred.gid = op_ctx->creds->caller_gid;
			retval =
			    libzfswrap_lookup(p_vfs, &cred,
					      parent_hdl->handle->clovis_handle,
					      path, &object, &type);
			if (retval) {
				fsal_error = posix2fsal_error(retval);
				goto errout;
			}

		}

	}
	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	retval = libzfswrap_getattr(p_vfs, &cred, object, &stat, &type);

	if (retval) {
		fsal_error = posix2fsal_error(retval);
		goto errout;
	}

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(&fh, &stat, NULL, op_ctx->fsal_export);

	*handle = &hdl->obj_handle;

	hdl->handle->clovis_handle = object;
	hdl->handle->i_snap = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);

 errout:
	return fsalstat(fsal_error, retval);
}

/* lookup_path
 * should not be used for "/" only is exported */

fsal_status_t clovis_lookup_path(struct fsal_export *exp_hdl,
			       const char *path,
			       struct fsal_obj_handle **handle)
{
	inogen_t object;
	int rc = 0;
	struct clovis_fsal_obj_handle *hdl;
	struct clovis_file_handle fh;
	struct stat stat;
	int type;
	creden_t cred;

	if (strcmp(path, "/"))
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	rc = libzfswrap_getroot(clovis_get_root_pvfs(exp_hdl), &object);
	if (rc != 0)
		return fsalstat(posix2fsal_error(rc), rc);
	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	rc = libzfswrap_getattr(clovis_get_root_pvfs(exp_hdl), &cred,
				object, &stat, &type);
	if (rc != 0)
		return fsalstat(posix2fsal_error(rc), rc);

	fh.clovis_handle = object;
	fh.i_snap = 0;

	hdl = alloc_handle(&fh, &stat, NULL, exp_hdl);

	*handle = &hdl->obj_handle;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* create
 * create a regular file and set its attributes
 */

static fsal_status_t clovis_create(struct fsal_obj_handle *dir_hdl,
				 const char *name, struct attrlist *attrib,
				 struct fsal_obj_handle **handle)
{
	struct clovis_fsal_obj_handle *myself, *hdl;
	int retval = 0;
	struct clovis_file_handle fh;
	creden_t cred;
	inogen_t object;
	struct stat stat;
	int type;

	*handle = NULL;		/* poison it */
	if (!dir_hdl->obj_ops.handle_is(dir_hdl, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			dir_hdl);
		return fsalstat(ERR_FSAL_NOTDIR, 0);
	}
	memset(&fh, 0, sizeof(struct clovis_file_handle));
	myself = container_of(dir_hdl, struct clovis_fsal_obj_handle, obj_handle);

	cred.uid = attrib->owner;
	cred.gid = attrib->group;

	retval = libzfswrap_create(clovis_get_root_pvfs(op_ctx->fsal_export),
				   &cred, myself->handle->clovis_handle, name,
				   fsal2unix_mode(attrib->mode), &object);
	if (retval)
		goto fileerr;
	retval = libzfswrap_getattr(clovis_get_root_pvfs(op_ctx->fsal_export),
				    &cred, object, &stat, &type);
	if (retval)
		goto fileerr;

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(&fh, &stat, NULL, op_ctx->fsal_export);

	/* >> set output handle << */
	hdl->handle->clovis_handle = object;
	hdl->handle->i_snap = 0;
	*handle = &hdl->obj_handle;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);

 fileerr:

	return fsalstat(posix2fsal_error(retval), retval);
}

static fsal_status_t clovis_mkdir(struct fsal_obj_handle *dir_hdl,
				const char *name, struct attrlist *attrib,
				struct fsal_obj_handle **handle)
{
	struct clovis_fsal_obj_handle *myself, *hdl;
	int retval = 0;
	struct clovis_file_handle fh;
	creden_t cred;
	inogen_t object;
	struct stat stat;
	int type;

	*handle = NULL;		/* poison it */
	if (!dir_hdl->obj_ops.handle_is(dir_hdl, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			dir_hdl);
		return fsalstat(ERR_FSAL_NOTDIR, 0);
	}
	memset(&fh, 0, sizeof(struct clovis_file_handle));
	myself = container_of(dir_hdl, struct clovis_fsal_obj_handle, obj_handle);

	cred.uid = attrib->owner;
	cred.gid = attrib->group;

	retval = libzfswrap_mkdir(clovis_get_root_pvfs(op_ctx->fsal_export),
				  &cred, myself->handle->clovis_handle, name,
				  fsal2unix_mode(attrib->mode), &object);
	if (retval)
		goto fileerr;
	retval = libzfswrap_getattr(clovis_get_root_pvfs(op_ctx->fsal_export),
				    &cred, object, &stat, &type);
	if (retval)
		goto fileerr;

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(&fh, &stat, NULL, op_ctx->fsal_export);

	/* >> set output handle << */
	hdl->handle->clovis_handle = object;
	hdl->handle->i_snap = 0;
	*handle = &hdl->obj_handle;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);

 fileerr:

	return fsalstat(posix2fsal_error(retval), retval);
}

static fsal_status_t clovis_makenode(struct fsal_obj_handle *dir_hdl,
				   const char *name,
				   object_file_type_t nodetype,	/* IN */
				   fsal_dev_t *dev,	/* IN */
				   struct attrlist *attrib,
				   struct fsal_obj_handle **handle)
{
	return fsalstat(ERR_FSAL_NOTSUPP, 0);
}

/** makesymlink
 *  Note that we do not set mode bits on symlinks for Linux/POSIX
 *  They are not really settable in the kernel and are not checked
 *  anyway (default is 0777) because open uses that target's mode
 */

static fsal_status_t clovis_makesymlink(struct fsal_obj_handle *dir_hdl,
				      const char *name, const char *link_path,
				      struct attrlist *attrib,
				      struct fsal_obj_handle **handle)
{
	struct clovis_fsal_obj_handle *myself, *hdl;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	creden_t cred;
	inogen_t object;
	struct stat stat;
	int type;

	struct clovis_file_handle fh;

	*handle = NULL;		/* poison it first */
	if (!dir_hdl->obj_ops.handle_is(dir_hdl, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			dir_hdl);
		return fsalstat(ERR_FSAL_NOTDIR, 0);
	}
	memset(&fh, 0, sizeof(struct clovis_file_handle));
	myself = container_of(dir_hdl, struct clovis_fsal_obj_handle, obj_handle);
	cred.uid = attrib->owner;
	cred.gid = attrib->group;

	retval = libzfswrap_symlink(clovis_get_root_pvfs(op_ctx->fsal_export),
				    &cred, myself->handle->clovis_handle, name,
				    link_path, &object);
	if (retval)
		goto err;

	retval = libzfswrap_getattr(clovis_get_root_pvfs(op_ctx->fsal_export),
				    &cred, object, &stat, &type);
	if (retval)
		goto err;

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(&fh, &stat, link_path, op_ctx->fsal_export);

	*handle = &hdl->obj_handle;
	hdl->handle->clovis_handle = object;
	hdl->handle->i_snap = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);

 err:
	if (retval == ENOENT)
		fsal_error = ERR_FSAL_STALE;
	else
		fsal_error = posix2fsal_error(retval);
	return fsalstat(fsal_error, retval);
}

static fsal_status_t clovis_readsymlink(struct fsal_obj_handle *obj_hdl,
				      struct gsh_buffdesc *link_content,
				      bool refresh)
{
	struct clovis_fsal_obj_handle *myself = NULL;
	int retval = 0;
	int retlink = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	creden_t cred;

	if (obj_hdl->type != SYMBOLIC_LINK) {
		fsal_error = ERR_FSAL_FAULT;
		goto out;
	}
	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	/* The link length should be cached in the file handle */

	link_content->len =
	    myself->attributes.filesize ? (myself->attributes.filesize +
					   1) : fsal_default_linksize;
	link_content->addr = gsh_malloc(link_content->len);

	retlink = libzfswrap_readlink(clovis_get_root_pvfs(op_ctx->fsal_export),
				      &cred,
				      myself->handle->clovis_handle,
				      link_content->addr,
				      link_content->len);

	if (retlink) {
		fsal_error = posix2fsal_error(retlink);
		gsh_free(link_content->addr);
		link_content->addr = NULL;
		link_content->len = 0;
		goto out;
	}

	link_content->len = strlen(link_content->addr) + 1;
 out:
	return fsalstat(fsal_error, retval);
}

static fsal_status_t clovis_linkfile(struct fsal_obj_handle *obj_hdl,
				   struct fsal_obj_handle *destdir_hdl,
				   const char *name)
{
	struct clovis_fsal_obj_handle *myself, *destdir;
	int retval = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	creden_t cred;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	destdir =
	    container_of(destdir_hdl, struct clovis_fsal_obj_handle, obj_handle);

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	retval = libzfswrap_link(clovis_get_root_pvfs(op_ctx->fsal_export),
				 &cred,
				 destdir->handle->clovis_handle,
				 myself->handle->clovis_handle, name);
	if (retval)
		fsal_error = posix2fsal_error(retval);

	return fsalstat(fsal_error, retval);
}

#define MAX_ENTRIES 256
/**
 * read_dirents
 * read the directory and call through the callback function for
 * each entry.
 * @param dir_hdl [IN] the directory to read
 * @param entry_cnt [IN] limit of entries. 0 implies no limit
 * @param whence [IN] where to start (next)
 * @param dir_state [IN] pass thru of state to callback
 * @param cb [IN] callback function
 * @param eof [OUT] eof marker true == end of dir
 */
static fsal_status_t clovis_readdir(struct fsal_obj_handle *dir_hdl,
				  fsal_cookie_t *whence, void *dir_state,
				  fsal_readdir_cb cb, bool *eof)
{
	struct clovis_fsal_obj_handle *myself;
	int retval = 0;
	off_t seekloc = 0;
	creden_t cred;
	libzfswrap_vfs_t *p_vfs = NULL;
	libzfswrap_vnode_t *pvnode = NULL;
	libzfswrap_entry_t dirents[MAX_ENTRIES];
	unsigned int index = 0;

	if (whence != NULL)
		seekloc = (off_t) *whence;
	myself = container_of(dir_hdl, struct clovis_fsal_obj_handle, obj_handle);

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	p_vfs = ZFSFSAL_GetVFS(myself->handle);
	if (!p_vfs) {
		retval = 0;
		goto out;
	}

	/* Open the directory */
	retval = libzfswrap_opendir(p_vfs, &cred,
				    myself->handle->clovis_handle,
				    &pvnode);
	if (retval)
		goto out;
	*eof = false;
	do {
		retval = libzfswrap_readdir(p_vfs, &cred, pvnode, dirents,
					    MAX_ENTRIES, &seekloc);
		if (retval)
			goto out;
		for (index = 0; index < MAX_ENTRIES; index++) {
			/* If psz_filename is NULL,
			 * that's the end of the list */
			if (dirents[index].psz_filename[0] == '\0') {
				*eof = true;
				break;
			}

			/* Skip '.' and '..' */
			if (!strcmp(dirents[index].psz_filename, ".")
			    || !strcmp(dirents[index].psz_filename, ".."))
				continue;

			/* callback to cache inode */
			if (!cb(dirents[index].psz_filename,
				dir_state,
				(fsal_cookie_t) index))
				goto done;
		}

		seekloc += MAX_ENTRIES;

	} while (*eof == false);

 done:
	/* Close the directory */
	retval = libzfswrap_closedir(p_vfs, &cred, pvnode);
	if (retval)
		goto out;

	/* read the directory */

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
 out:
	return fsalstat(posix2fsal_error(retval), retval);
}

static fsal_status_t clovis_rename(struct fsal_obj_handle *obj_hdl,
				 struct fsal_obj_handle *olddir_hdl,
				 const char *old_name,
				 struct fsal_obj_handle *newdir_hdl,
				 const char *new_name)
{
	struct clovis_fsal_obj_handle *olddir, *newdir;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	creden_t cred;

	olddir =
	    container_of(olddir_hdl, struct clovis_fsal_obj_handle, obj_handle);
	newdir =
	    container_of(newdir_hdl, struct clovis_fsal_obj_handle, obj_handle);

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	retval = libzfswrap_rename(clovis_get_root_pvfs(op_ctx->fsal_export),
				   &cred,
				   olddir->handle->clovis_handle,
				   old_name,
				   newdir->handle->clovis_handle,
				   new_name);

	if (retval)
		fsal_error = posix2fsal_error(retval);
	return fsalstat(fsal_error, retval);
}

/* FIXME: attributes are now merged into fsal_obj_handle.  This
 * spreads everywhere these methods are used.  eventually deprecate
 * everywhere except where we explicitly want to to refresh them.
 * NOTE: this is done under protection of the attributes rwlock in the
 * cache entry.
 */

static fsal_status_t clovis_getattrs(struct fsal_obj_handle *obj_hdl)
{
	struct clovis_fsal_obj_handle *myself;
	struct stat stat;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	int type = 0;
	creden_t cred;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	if (myself->handle->clovis_handle.inode == ZFS_SNAP_DIR_INODE
	    && myself->handle->clovis_handle.generation == 0) {
		memset(&stat, 0, sizeof(stat));
		stat.st_mode = S_IFDIR | 0755;
		stat.st_ino = ZFS_SNAP_DIR_INODE;
		stat.st_nlink = 2;
		stat.st_ctime = time(NULL);
		stat.st_atime = stat.st_ctime;
		stat.st_mtime = stat.st_ctime;
		retval = 0;
	} else {
		retval = libzfswrap_getattr(
			clovis_get_root_pvfs(op_ctx->fsal_export), &cred,
					   myself->handle->clovis_handle, &stat,
					   &type);

		external_consolidate_attrs(obj_hdl, &stat);

		/* An explanation is required here.
		 * This is an exception management.
		 * when a file is opened, then deleted without being closed,
		 * FSAL_VFS can still getattr on it, because it uses fstat
		 * on a cached FD. This is not possible
		 * to do this with ZFS, because you can't fstat on a vnode.
		 * To handle this, stat are
		 * cached as the file is opened and used here,
		 * to emulate a successful fstat */
		if ((retval == ENOENT)
		    && (myself->u.file.openflags != FSAL_O_CLOSED)
		    && (S_ISREG(myself->u.file.saved_stat.st_mode))) {
			memcpy(&stat, &myself->u.file.saved_stat,
			       sizeof(struct stat));
			retval = 0;	/* remove the error */
			goto ok_file_opened_and_deleted;
		}

		if (retval)
			goto errout;
	}

	/* convert attributes */
 ok_file_opened_and_deleted:
	posix2fsal_attributes(&stat, &myself->attributes);
	goto out;

 errout:
	if (retval == ENOENT)
		fsal_error = ERR_FSAL_STALE;
	else
		fsal_error = posix2fsal_error(retval);
 out:
	return fsalstat(fsal_error, retval);
}

/*
 * NOTE: this is done under protection of the attributes rwlock
 * in the cache entry.
 */

static fsal_status_t clovis_setattrs(struct fsal_obj_handle *obj_hdl,
				   struct attrlist *attrs)
{
	struct clovis_fsal_obj_handle *myself;
	struct stat stats = { 0 };
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	int flags = 0;
	creden_t cred;
	struct stat new_stat = { 0 };

	/* apply umask, if mode attribute is to be changed */
	if (FSAL_TEST_MASK(attrs->mask, ATTR_MODE))
		attrs->mode &= ~op_ctx->fsal_export->exp_ops.
				fs_umask(op_ctx->fsal_export);
	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	if (myself->handle->i_snap != 0) {
		LogDebug(COMPONENT_FSAL,
			 "Trying to change the attributes of an object inside a snapshot");
		return fsalstat(ERR_FSAL_ROFS, 0);
	}

	/* First, check that FSAL attributes */
	if (FSAL_TEST_MASK(attrs->mask, ATTR_SIZE)) {
		if (obj_hdl->type != REGULAR_FILE) {
			fsal_error = ERR_FSAL_INVAL;
			return fsalstat(fsal_error, retval);
		}
		retval =
		    libzfswrap_truncate(clovis_get_root_pvfs(op_ctx->fsal_export),
					&cred, myself->handle->clovis_handle,
					attrs->filesize);

		if (retval == 0)
			retval = external_truncate(obj_hdl,
						   attrs->filesize);

		if (retval != 0)
			goto out;
	}
	if (FSAL_TEST_MASK(attrs->mask, ATTR_MODE)) {
		flags |= LZFSW_ATTR_MODE;
		stats.st_mode = fsal2unix_mode(attrs->mode);
	}
	if (FSAL_TEST_MASK(attrs->mask, ATTR_OWNER)) {
		flags |= LZFSW_ATTR_UID;
		stats.st_uid = attrs->owner;
	}
	if (FSAL_TEST_MASK(attrs->mask, ATTR_GROUP)) {
		flags |= LZFSW_ATTR_GID;
		stats.st_gid = attrs->group;
	}
	if (FSAL_TEST_MASK(attrs->mask, ATTR_ATIME)) {
		flags |= LZFSW_ATTR_ATIME;
		stats.st_atime = attrs->atime.tv_sec;
	}
	if (FSAL_TEST_MASK(attrs->mask, ATTR_ATIME_SERVER)) {
		flags |= LZFSW_ATTR_ATIME;
		struct timespec timestamp;

		retval = clock_gettime(CLOCK_REALTIME, &timestamp);
		if (retval != 0)
			goto out;
		stats.st_atim = timestamp;
	}
	if (FSAL_TEST_MASK(attrs->mask, ATTR_MTIME)) {
		flags |= LZFSW_ATTR_MTIME;
		stats.st_mtime = attrs->mtime.tv_sec;
	}
	if (FSAL_TEST_MASK(attrs->mask, ATTR_MTIME_SERVER)) {
		flags |= LZFSW_ATTR_MTIME;
		struct timespec timestamp;

		retval = clock_gettime(CLOCK_REALTIME, &timestamp);
		if (retval != 0)
			goto out;
		stats.st_mtim = timestamp;
	}
	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	retval =
	    libzfswrap_setattr(clovis_get_root_pvfs(op_ctx->fsal_export), &cred,
			       myself->handle->clovis_handle, &stats, flags,
			       &new_stat);
 out:
	if (retval == 0)
		return fsalstat(ERR_FSAL_NO_ERROR, 0);

	/* Exit with an error */
	fsal_error = posix2fsal_error(retval);
	return fsalstat(fsal_error, retval);
}

/* file_unlink
 * unlink the named file in the directory
 */
static fsal_status_t clovis_unlink(struct fsal_obj_handle *dir_hdl,
				 const char *name)
{
	struct clovis_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	creden_t cred;
	inogen_t object;
	int type = 0;

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	myself = container_of(dir_hdl, struct clovis_fsal_obj_handle, obj_handle);

	/* check for presence of file and get its type */
	retval = libzfswrap_lookup(clovis_get_root_pvfs(op_ctx->fsal_export),
				   &cred,
				   myself->handle->clovis_handle,
				   name, &object,
				   &type);
	if (retval == 0) {
		if (type == S_IFDIR)
			retval = libzfswrap_rmdir(clovis_get_root_pvfs(
							  op_ctx->fsal_export),
						  &cred,
						  myself->handle->clovis_handle,
						  name);
		else {
			retval = external_unlink(dir_hdl, name);
			if (!retval)
				retval = libzfswrap_unlink(
						clovis_get_root_pvfs(
							op_ctx->fsal_export),
						&cred,
						myself->handle->clovis_handle,
						name);
		}
	}

	if (retval)
		fsal_error = posix2fsal_error(retval);

	return fsalstat(fsal_error, retval);
}

/* handle_digest
 * fill in the opaque f/s file handle part.
 * we zero the buffer to length first.  This MAY already be done above
 * at which point, remove memset here because the caller is zeroing
 * the whole struct.
 */

static fsal_status_t clovis_handle_digest(const struct fsal_obj_handle *obj_hdl,
					fsal_digesttype_t output_type,
					struct gsh_buffdesc *fh_desc)
{
	const struct clovis_fsal_obj_handle *myself;
	struct clovis_file_handle *fh;
	size_t fh_size;

	/* sanity checks */
	if (!fh_desc)
		return fsalstat(ERR_FSAL_FAULT, 0);
	myself =
	    container_of(obj_hdl, const struct clovis_fsal_obj_handle, obj_handle);
	fh = myself->handle;

	switch (output_type) {
	case FSAL_DIGEST_NFSV3:
	case FSAL_DIGEST_NFSV4:
		fh_size = clovis_sizeof_handle(fh);
		if (fh_desc->len < fh_size)
			goto errout;
		memcpy(fh_desc->addr, fh, fh_size);
		break;
	default:
		return fsalstat(ERR_FSAL_SERVERFAULT, 0);
	}
	fh_desc->len = fh_size;
	return fsalstat(ERR_FSAL_NO_ERROR, 0);

 errout:
	LogMajor(COMPONENT_FSAL,
		 "Space too small for handle.  need %lu, have %lu", fh_size,
		 fh_desc->len);
	return fsalstat(ERR_FSAL_TOOSMALL, 0);
}

/**
 * handle_to_key
 * return a handle descriptor into the handle in this object handle
 * @TODO reminder.  make sure things like hash keys don't point here
 * after the handle is released.
 */

static void clovis_handle_to_key(struct fsal_obj_handle *obj_hdl,
			       struct gsh_buffdesc *fh_desc)
{
	struct clovis_fsal_obj_handle *myself;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);
	fh_desc->addr = myself->handle;
	fh_desc->len = clovis_sizeof_handle(myself->handle);
}

/*
 * release
 * release our export first so they know we are gone
 */

static void release(struct fsal_obj_handle *obj_hdl)
{
	struct clovis_fsal_obj_handle *myself;
	object_file_type_t type = obj_hdl->type;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	if (type == REGULAR_FILE &&
	    myself->u.file.openflags != FSAL_O_CLOSED) {
		fsal_status_t st = clovis_close(obj_hdl);

		if (FSAL_IS_ERROR(st)) {
			LogCrit(COMPONENT_FSAL,
				"Could not close, error %s(%d)",
				strerror(st.minor), st.minor);
		}
	}

	fsal_obj_handle_fini(obj_hdl);

	if (type == SYMBOLIC_LINK) {
		if (myself->u.symlink.link_content != NULL)
			gsh_free(myself->u.symlink.link_content);
	}
	gsh_free(myself);
}

void clovis_handle_ops_init(struct fsal_obj_ops *ops)
{
	ops->release = release;
	ops->lookup = clovis_lookup;
	ops->readdir = clovis_readdir;
	ops->create = clovis_create;
	ops->mkdir = clovis_mkdir;
	ops->mknode = clovis_makenode;
	ops->symlink = clovis_makesymlink;
	ops->readlink = clovis_readsymlink;
	ops->test_access = fsal_test_access;
	ops->getattrs = clovis_getattrs;
	ops->setattrs = clovis_setattrs;
	ops->link = clovis_linkfile;
	ops->rename = clovis_rename;
	ops->unlink = clovis_unlink;
	ops->open = clovis_open;
	ops->status = clovis_status;
	ops->read = clovis_read;
	ops->write = clovis_write;
	ops->commit = clovis_commit;
	ops->lock_op = clovis_lock_op;
	ops->close = clovis_close;
	ops->lru_cleanup = clovis_lru_cleanup;
	ops->handle_digest = clovis_handle_digest;
	ops->handle_to_key = clovis_handle_to_key;

	/* xattr related functions */
	ops->list_ext_attrs = clovis_list_ext_attrs;
	ops->getextattr_id_by_name = clovis_getextattr_id_by_name;
	ops->getextattr_value_by_name = clovis_getextattr_value_by_name;
	ops->getextattr_value_by_id = clovis_getextattr_value_by_id;
	ops->setextattr_value = clovis_setextattr_value;
	ops->setextattr_value_by_id = clovis_setextattr_value_by_id;
	ops->getextattr_attrs = clovis_getextattr_attrs;
	ops->remove_extattr_by_id = clovis_remove_extattr_by_id;
	ops->remove_extattr_by_name = clovis_remove_extattr_by_name;
}

/* export methods that create object handles
 */

/* create_handle
 * Does what original FSAL_ExpandHandle did (sort of)
 * returns a ref counted handle to be later used in cache_inode etc.
 * NOTE! you must release this thing when done with it!
 * BEWARE! Thanks to some holes in the *AT syscalls implementation,
 * we cannot get an fd on an AF_UNIX socket.  Sorry, it just doesn't...
 * we could if we had the handle of the dir it is in, but this method
 * is for getting handles off the wire for cache entries that have LRU'd.
 * Ideas and/or clever hacks are welcome...
 */

fsal_status_t clovis_create_handle(struct fsal_export *exp_hdl,
				 struct gsh_buffdesc *hdl_desc,
				 struct fsal_obj_handle **handle)
{
	struct clovis_fsal_obj_handle *hdl;
	struct clovis_file_handle fh;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	char link_buff[PATH_MAX];
	char *link_content = NULL;
	struct stat stat;
	creden_t cred;
	int type;
	int retval;

	*handle = NULL;		/* poison it first */
	if (hdl_desc->len > sizeof(struct clovis_file_handle))
		return fsalstat(ERR_FSAL_FAULT, 0);

	memcpy(&fh, hdl_desc->addr, hdl_desc->len);  /* struct aligned copy */

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	retval = libzfswrap_getattr(clovis_get_root_pvfs(exp_hdl), &cred,
				    fh.clovis_handle, &stat, &type);
	if (retval)
		return fsalstat(posix2fsal_error(retval), retval);

	link_content = NULL;
	if (S_ISLNK(stat.st_mode)) {
		retval = libzfswrap_readlink(clovis_get_root_pvfs(exp_hdl),
					     &cred,
					     fh.clovis_handle,
					     link_buff,
					     PATH_MAX);
		if (retval)
			return fsalstat(posix2fsal_error(retval), retval);
		link_content = link_buff;
	}
	hdl = alloc_handle(&fh, &stat, link_content, exp_hdl);

	*handle = &hdl->obj_handle;

	return fsalstat(fsal_error, 0);
}