extern size_t i_snapshots;
extern snapshot_t *p_snapshots;

/* ZFS methods for handles
 */

void clovis_handle_ops_init(struct fsal_obj_ops *ops);

/* private helpers from export
 */

libzfswrap_vfs_t *clovis_get_root_pvfs(struct fsal_export *exp_hdl);

/* method proto linkage to handle.c for export
 */

fsal_status_t clovis_lookup_path(struct fsal_export *exp_hdl,
			       const char *path,
			       struct fsal_obj_handle **handle);

fsal_status_t clovis_create_handle(struct fsal_export *exp_hdl,
				 struct gsh_buffdesc *hdl_desc,
				 struct fsal_obj_handle **handle);

/* ZFS FSAL module private storage
 */

struct clovis_fsal_module {
	struct fsal_module fsal;
	struct fsal_staticfsinfo_t fs_info;
};

/*
 * ZFS internal export
 */

struct clovis_fsal_export {
	struct fsal_export export;
	char *zpool;
	char *clovis_local_addr;
	char *clovis_ha_addr;
	char *clovis_confd_addr;
	char *clovis_prof;
	char *clovis_index_dir;
	libzfswrap_vfs_t *p_vfs;
};

/*
 * ZFS internal object handle
 * handle is a pointer because
 *  a) the last element of file_handle is a char[] meaning variable len...
 *  b) we cannot depend on it *always* being last or being the only
 *     variable sized struct here...  a pointer is safer.
 * wrt locks, should this be a lock counter??
 */

struct clovis_fsal_obj_handle {
	struct fsal_obj_handle obj_handle;
	struct attrlist attributes;
	struct clovis_file_handle *handle;
	union {
		struct {
			libzfswrap_vnode_t *p_vnode;
			fsal_openflags_t openflags;
			struct stat saved_stat;
			creden_t cred;
		} file;
		struct {
			unsigned char *link_content;
			int link_size;
		} symlink;
	} u;
};

	/* I/O management */
fsal_status_t clovis_open(struct fsal_obj_handle *obj_hdl,
			fsal_openflags_t openflags);
fsal_status_t clovis_commit(struct fsal_obj_handle *obj_hdl,	/* sync */
			  off_t offset, size_t len);
fsal_openflags_t clovis_status(struct fsal_obj_handle *obj_hdl);
fsal_status_t clovis_read(struct fsal_obj_handle *obj_hdl,
			uint64_t offset,
			size_t buffer_size, void *buffer, size_t *read_amount,
			bool *end_of_file);
fsal_status_t clovis_write(struct fsal_obj_handle *obj_hdl,
			 uint64_t offset,
			 size_t buffer_size, void *buffer,
			 size_t *write_amount, bool *fsal_stable);
fsal_status_t clovis_share_op(struct fsal_obj_handle *obj_hdl, void *p_owner,
			    fsal_share_param_t request_share);
fsal_status_t clovis_close(struct fsal_obj_handle *obj_hdl);
fsal_status_t clovis_lru_cleanup(struct fsal_obj_handle *obj_hdl,
			       lru_actions_t requests);

/* extended attributes management */
fsal_status_t clovis_list_ext_attrs(struct fsal_obj_handle *obj_hdl,
				  unsigned int cookie,
				  fsal_xattrent_t *xattrs_tab,
				  unsigned int xattrs_tabsize,
				  unsigned int *p_nb_returned,
				  int *end_of_list);
fsal_status_t clovis_getextattr_id_by_name(struct fsal_obj_handle *obj_hdl,
					 const char *xattr_name,
					 unsigned int *pxattr_id);
fsal_status_t clovis_getextattr_value_by_name(struct fsal_obj_handle *obj_hdl,
					    const char *xattr_name,
					    caddr_t buffer_addr,
					    size_t buffer_size,
					    size_t *p_output_size);
fsal_status_t clovis_getextattr_value_by_id(struct fsal_obj_handle *obj_hdl,
					  unsigned int xattr_id,
					  caddr_t buffer_addr,
					  size_t buffer_size,
					  size_t *p_output_size);
fsal_status_t clovis_setextattr_value(struct fsal_obj_handle *obj_hdl,
				    const char *xattr_name, caddr_t buffer_addr,
				    size_t buffer_size, int create);
fsal_status_t clovis_setextattr_value_by_id(struct fsal_obj_handle *obj_hdl,
					  unsigned int xattr_id,
					  caddr_t buffer_addr,
					  size_t buffer_size);
fsal_status_t clovis_getextattr_attrs(struct fsal_obj_handle *obj_hdl,
				    unsigned int xattr_id,
				    struct attrlist *p_attrs);
fsal_status_t clovis_remove_extattr_by_id(struct fsal_obj_handle *obj_hdl,
					unsigned int xattr_id);
fsal_status_t clovis_remove_extattr_by_name(struct fsal_obj_handle *obj_hdl,
					  const char *xattr_name);
fsal_status_t clovis_lock_op(struct fsal_obj_handle *obj_hdl,
			   void *p_owner,
			   fsal_lock_op_t lock_op,
			   fsal_lock_param_t *request_lock,
			   fsal_lock_param_t *conflicting_lock);

/* external storage */
int external_write(struct fsal_obj_handle *obj_hdl,
		   uint64_t offset,
		   size_t buffer_size,
		   void *buffer,
		   size_t *write_amount,
		   bool *fsal_stable,
		   struct stat *stat);

int external_read(struct fsal_obj_handle *obj_hdl,
		  uint64_t offset,
		  size_t buffer_size,
		  void *buffer,
		  size_t *read_amount,
		  bool *end_of_file);

int external_consolidate_attrs(struct fsal_obj_handle *obj_hdl,
			       struct stat *zfsstat);

int external_unlink(struct fsal_obj_handle *dir_hdl,
		    const char *name);

int external_truncate(struct fsal_obj_handle *obj_hdl,
		      off_t filesize);


