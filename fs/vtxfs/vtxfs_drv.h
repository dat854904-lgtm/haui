/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VTXFS v2.1 — kernel VFS adapter core header (P0.6 / P1).
 */
#ifndef _VTXFS_DRV_H
#define _VTXFS_DRV_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <linux/refcount.h>
#include <linux/pagemap.h>
#include <linux/blk_types.h>

#include "vtxfs_mem.h"
#include "vtxfs_engine.h"
#include "vtxfs_trace.h"

#define VTXFS_SB_MAGIC        0x56545821U	/* "VTX1" */
#define VTXFS_DEFAULT_SUBVOL  VTXFS_OID_FIRST_USER_SUBVOL

/*
 * Lock hierarchy (outer -> inner):
 *   1. VFS locks        (inode->i_rwsem, folio/page lock)
 *   2. commit_sem       (writer: checkpoint / freeze / remount-ro)
 *   3. sbi->op_lock     (engine serialization — engine is single-threaded)
 */
struct vtxfs_sb_info {
	struct super_block *sb;
	struct block_device *bdev;
	struct vtx_bdev engine_bdev;	/* engine block-device adapter */
	struct vtx_fs *fs;		/* portable P0 engine instance */
	struct mutex op_lock;		/* engine serialization (class 3) */
	unsigned int op_nofs;		/* saved NOFS token under op_lock */
	atomic64_t op_seq;		/* conservative potential mutation counter */
	u64 durable_seq;		/* covered by successful fsync checkpoint */
	struct vtxfs_fs_info *fs_info;
	u64 subvol_id;			/* mounted subvolume (mount -o subvol=) */
	bool policy_ro;			/* feature policy forced RO at mount */
};

#define vtxfs_inode_info vtxfs_inode

static inline struct vtxfs_sb_info *VTXFS_SB(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	return sb->s_fs_info;
}

static inline struct vtxfs_fs_info *VTXFS_FSI(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	return VTXFS_SB(sb)->fs_info;
}

static inline u64 vtxfs_active_subvol(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	return VTXFS_SB(sb)->subvol_id;
}

static inline struct vtxfs_inode *VTXFS_I(struct inode *inode)
{
	VTXFS_TRACE_ENTER();

	return container_of(inode, struct vtxfs_inode, vfs_inode);
}

#define VTXFS_I_INO(inode) (VTXFS_I(inode)->vtx_ino)

static inline struct page *vtx_buf_to_page(const void *addr)
{
	VTXFS_TRACE_ENTER();

	if (is_vmalloc_addr(addr))
		return vmalloc_to_page(addr);
	return virt_to_page(addr);
}

/* locking.c */
void vtxfs_lock_init(struct vtxfs_fs_info *fsi);
void vtxfs_op_begin(struct vtxfs_sb_info *sbi);
void vtxfs_op_end(struct vtxfs_sb_info *sbi);
int vtxfs_commit_begin(struct vtxfs_sb_info *sbi);
void vtxfs_commit_end(struct vtxfs_sb_info *sbi);

/* lru_cache.c */
struct vtxfs_lru_entry {
	struct list_head lru;
	u64 key;
};
struct vtxfs_lru_cache {
	struct list_head lru_list;
	unsigned long nr_items, max_items;
	void (*free_entry)(struct vtxfs_lru_cache *, struct vtxfs_lru_entry *);
};
void vtxfs_lru_init(struct vtxfs_lru_cache *c, unsigned long max_items,
		    void (*free_entry)(struct vtxfs_lru_cache *,
				       struct vtxfs_lru_entry *));
void vtxfs_lru_add(struct vtxfs_lru_cache *c, struct vtxfs_lru_entry *e);
void vtxfs_lru_remove(struct vtxfs_lru_cache *c, struct vtxfs_lru_entry *e);
void vtxfs_lru_prune_all(struct vtxfs_lru_cache *c);

/* root-tree.c / root.c */
int vtxfs_roots_scan(struct vtxfs_sb_info *sbi);
int vtxfs_root_register(struct vtxfs_sb_info *sbi, u64 subvol_id,
			const struct vtx_subvol_root_item *item);
struct vtxfs_root *vtxfs_root_get(struct super_block *sb, u64 subvol_id);
void vtxfs_root_put(struct vtxfs_root *root);
void vtxfs_roots_destroy(struct super_block *sb);

/* dir-item.c */
int vtxfs_dir_name_valid(const char *name, size_t len);
int vtxfs_dir_lookup(struct inode *dir, const struct qstr *name,
		     u64 *ino_out, u8 *ftype_out);
int vtxfs_dir_create(struct inode *dir, const struct qstr *name,
		     umode_t mode, u64 *ino_out);
int vtxfs_dir_mkdir(struct inode *dir, const struct qstr *name,
		    umode_t mode, u64 *ino_out);
int vtxfs_dir_symlink(struct inode *dir, const struct qstr *name,
		      const char *target, u64 *ino_out);
int vtxfs_dir_mknod(struct inode *dir, const struct qstr *name,
		    umode_t mode, dev_t rdev, u64 *ino_out);
int vtxfs_dir_unlink(struct inode *dir, const struct qstr *name);
int vtxfs_dir_rmdir(struct inode *dir, const struct qstr *name);
int vtxfs_dir_rename(struct inode *old_dir, struct dentry *old_dentry,
		     struct inode *new_dir, struct dentry *new_dentry,
		     unsigned int flags);

/* inode-item.c */
void vtxfs_inode_item_apply(struct inode *inode,
			    const struct vtx_inode_item *st, bool with_times);
int vtxfs_inode_item_load(struct inode *inode, struct vtx_inode_item *st);
int vtxfs_inode_item_parent(struct inode *inode, u64 *parent_ino);

/* disk-io.c */
int vtxfs_disk_init_cache(struct super_block *sb);
void vtxfs_disk_destroy_cache(struct super_block *sb);
int vtxfs_disk_read_meta(struct vtxfs_sb_info *sbi, u64 vblk, void *buf_out);
int vtxfs_disk_flush(struct vtxfs_sb_info *sbi);

/* direct-io.c */
ssize_t vtxfs_direct_read(struct kiocb *iocb, struct iov_iter *to);
ssize_t vtxfs_direct_write(struct kiocb *iocb, struct iov_iter *from);

/* fiemap.c */
int vtxfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		 u64 start, u64 len);

/* ioctl.c */
long vtxfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* export.c */
extern const struct export_operations vtxfs_export_ops;

/* writeback.c */
int vtxfs_writepages(struct address_space *mapping,
		     struct writeback_control *wbc);

/* inode.c */
struct inode *vtxfs_alloc_inode(struct super_block *sb);
void vtxfs_free_inode(struct inode *inode);
struct inode *vtxfs_iget(struct super_block *sb, u64 ino);
int vtxfs_refresh_inode(struct inode *inode);
int __init vtxfs_init_inode_cache(void);
void vtxfs_destroy_inode_cache(void);
extern const struct inode_operations vtxfs_dir_iops;
extern const struct inode_operations vtxfs_file_iops;
extern const struct inode_operations vtxfs_symlink_iops;

/* file.c */
extern const struct file_operations vtxfs_dir_fops;
extern const struct file_operations vtxfs_file_fops;
extern const struct address_space_operations vtxfs_aops;

/* xattr.c */
extern const struct xattr_handler * const vtxfs_xattr_handlers[];
ssize_t vtxfs_listxattr(struct dentry *dentry, char *buffer, size_t size);

#endif /* _VTXFS_DRV_H */
