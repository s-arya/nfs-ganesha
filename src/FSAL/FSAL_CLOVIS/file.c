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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * -------------
 */

/* file.c
 * File I/O methods for ZFS module
 */

#include "config.h"

#include <assert.h>
#include "fsal.h"
#include "fsal_internal.h"
#include "FSAL/access_check.h"
#include "fsal_convert.h"
#include <unistd.h>
#include <fcntl.h>
#include "FSAL/fsal_commonlib.h"
#include "clovis_methods.h"
#include <stdbool.h>

libzfswrap_vfs_t *ZFSFSAL_GetVFS(clovis_file_handle_t *handle);

/** clovis_open
 * called with appropriate locks taken at the cache inode level
 */

fsal_status_t clovis_open(struct fsal_obj_handle *obj_hdl,
			fsal_openflags_t openflags)
{
	struct clovis_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int rc = 0;
	int type = 0;
	libzfswrap_vnode_t *p_vnode;
	creden_t cred;

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	assert(myself->u.file.openflags == FSAL_O_CLOSED);

	rc = libzfswrap_open(ZFSFSAL_GetVFS(myself->handle), &cred,
			     myself->handle->clovis_handle, O_RDWR, &p_vnode);

	if (rc) {
		fsal_error = posix2fsal_error(rc);
		return fsalstat(fsal_error, rc);
	}

	/* >> fill output struct << */
	myself->u.file.openflags = openflags;
	myself->u.file.p_vnode = p_vnode;

	/* save the stat */
	rc = libzfswrap_getattr(ZFSFSAL_GetVFS(myself->handle), &cred,
				myself->handle->clovis_handle,
				&myself->u.file.saved_stat, &type);

	if (rc) {
		fsal_error = posix2fsal_error(rc);
		return fsalstat(fsal_error, rc);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* clovis_status
 * Let the caller peek into the file's open/close state.
 */

fsal_openflags_t clovis_status(struct fsal_obj_handle *obj_hdl)
{
	struct clovis_fsal_obj_handle *myself;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);
	return myself->u.file.openflags;
}

/* clovis_read
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t clovis_read(struct fsal_obj_handle *obj_hdl,
			uint64_t offset,
			size_t buffer_size, void *buffer, size_t *read_amount,
			bool *end_of_file)
{
	struct clovis_fsal_obj_handle *myself;
	int rc = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	assert(myself->u.file.openflags != FSAL_O_CLOSED);

	rc = external_read(obj_hdl, offset, buffer_size, buffer,
		      read_amount, end_of_file);

	/* With FSAL_ZFS, "end of file" is always returned via a last call,
	 * once every data is read. The result is a last,
	 * empty call which set end_of_file to true */
	if (rc < 0) {
		fsal_error = posix2fsal_error(-rc);
		return fsalstat(fsal_error, -rc);
	} else if (rc == 0) {
		*end_of_file = true;
		*read_amount = 0;
	} else {
		*end_of_file = false;
		*read_amount = buffer_size;
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* clovis_write
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t clovis_write(struct fsal_obj_handle *obj_hdl,
			 uint64_t offset,
			 size_t buffer_size, void *buffer,
			 size_t *write_amount, bool *fsal_stable)
{
	struct clovis_fsal_obj_handle *myself;
	int retval = 0;
	int retstat = 0;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	assert(myself->u.file.openflags != FSAL_O_CLOSED);

	printf("======> clovis_write: off=%lld  size=%lld\n",
		(long long int)offset, 
		(long long int)buffer_size);
	retval = external_write(obj_hdl, offset, buffer_size, buffer,
		       write_amount, fsal_stable,
		       &myself->u.file.saved_stat);

	/* Try to consolidate attrs */
	retstat = external_consolidate_attrs(obj_hdl,
					     &myself->u.file.saved_stat); 
	printf("======> clovis_write/consolidate_attrs : retstat=%d size=%lld\n",
		 retstat, (long long int)myself->u.file.saved_stat.st_size);
	obj_hdl->attrs->filesize = myself->u.file.saved_stat.st_size;
	obj_hdl->attrs->mtime.tv_sec = myself->u.file.saved_stat.st_mtime;

	if (retval < 0)
		return fsalstat(posix2fsal_error(-retval), -retval);
	*write_amount = buffer_size;
	*fsal_stable = false;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* clovis_commit
 * Commit a file range to storage.
 * for right now, fsync will have to do.
 */

fsal_status_t clovis_commit(struct fsal_obj_handle *obj_hdl,	/* sync */
			  off_t offset, size_t len)
{
	struct clovis_fsal_obj_handle *myself;
	int retval = 0;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	/* Try to consolidate attrs */
	retval = external_consolidate_attrs(obj_hdl,
					     &myself->u.file.saved_stat); 
	printf("======> clovis_commit/consolidate_attrs : retstat=%d size=%lld\n", 
		retval, (long long int)myself->u.file.saved_stat.st_size);
	obj_hdl->attrs->filesize = myself->u.file.saved_stat.st_size;
	obj_hdl->attrs->mtime.tv_sec = myself->u.file.saved_stat.st_mtime;
	
	
	/* ZFS is a COW based FS, commit are not needed */
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* clovis_close
 * Close the file if it is still open.
 * Yes, we ignor lock status.  Closing a file in POSIX
 * releases all locks but that is state and cache inode's problem.
 */

fsal_status_t clovis_close(struct fsal_obj_handle *obj_hdl)
{
	struct clovis_fsal_obj_handle *myself;
	int retval = 0;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);

	retval =
	    libzfswrap_close(ZFSFSAL_GetVFS(myself->handle),
			     &myself->u.file.cred, myself->u.file.p_vnode,
			     myself->u.file.openflags);
	if (retval)
		return fsalstat(posix2fsal_error(retval), retval);

	retval = external_consolidate_attrs(obj_hdl,
					    &myself->u.file.saved_stat);
	if (retval == ENOENT)
		retval = 0; /* The file may be a whole with no data */
	else {
		obj_hdl->attrs->filesize = myself->u.file.saved_stat.st_size;
		obj_hdl->attrs->mtime.tv_sec =
			myself->u.file.saved_stat.st_mtime;
	}

	if (retval != 0)
		return fsalstat(posix2fsal_error(retval), retval);

	myself->u.file.openflags = FSAL_O_CLOSED;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* clovis_lru_cleanup
 * free non-essential resources at the request of cache inode's
 * LRU processing identifying this handle as stale enough for resource
 * trimming.
 */

fsal_status_t clovis_lru_cleanup(struct fsal_obj_handle *obj_hdl,
			       lru_actions_t requests)
{
	struct clovis_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct clovis_fsal_obj_handle, obj_handle);
	myself->u.file.openflags = FSAL_O_CLOSED;

	return fsalstat(fsal_error, retval);
}

fsal_status_t clovis_lock_op(struct fsal_obj_handle *obj_hdl,
			   void *p_owner,
			   fsal_lock_op_t lock_op,
			   fsal_lock_param_t *request_lock,
			   fsal_lock_param_t *conflicting_lock)
{
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}