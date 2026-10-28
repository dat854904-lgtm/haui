// SPDX-License-Identifier: GPL-2.0-only
/*
 * VTXFS v2.1 P0.6 — inode cache, iget and inode_operations.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/time64.h>
#include <linux/kdev_t.h>
#include <linux/version.h>

#include "vtxfs_drv.h"
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"


static struct kmem_cache *vtxfs_inode_cachep;

/* VFS inode embedded in a private slab object MUST be initialized once.
 * Without inode_init_once, i_io_list is uninitialized and generic writes
 * crash in inode_io_list_move_locked (reproduced in guest QEMU). */
static void vtxfs_inode_init_once(void *obj)
{
	struct vtxfs_inode_info *vi = obj;

	inode_init_once(&vi->vfs_inode);
}

int __init vtxfs_init_inode_cache(void)
{
	VTXFS_TRACE_ENTER();

	vtxfs_inode_cachep = kmem_cache_create("vtxfs_inode_cache",
			sizeof(struct vtxfs_inode_info), 0,
			SLAB_RECLAIM_ACCOUNT, vtxfs_inode_init_once);
	return vtxfs_inode_cachep ? 0 : -ENOMEM;
}

void vtxfs_destroy_inode_cache(void)
{
	VTXFS_TRACE_ENTER();

	kmem_cache_destroy(vtxfs_inode_cachep);
}

struct inode *vtxfs_alloc_inode(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_inode_info *vi;

	vi = alloc_inode_sb(sb, vtxfs_inode_cachep, GFP_KERNEL);
	if (!vi)
		return NULL;
	vi->vtx_ino = 0;
	vi->ino = 0;
	vi->disk_i_size = 0;
	vi->generation = 1;
	vi->last_xid = 0;
	vi->sync_xid = 0;
	vi->next_dir_seq = 1;
	vi->root = NULL;
	vi->delayed_node = NULL;
	vi->extent_map_tree.mappings = RB_ROOT;
	rwlock_init(&vi->extent_map_tree.lock);
	vtxfs_io_tree_init(&vi->io_tree, vi);
	vtxfs_io_tree_init(&vi->file_extent_tree, vi);
	init_rwsem(&vi->mmap_sem);
	mutex_init(&vi->log_mutex);
	spin_lock_init(&vi->lock);
	vi->runtime_flags = (1UL << VTXFS_INODE_DIRSEQ_VALID);
	return &vi->vfs_inode;
}

/* Called after the RCU grace period — safe to free immediately. */
void vtxfs_free_inode(struct inode *inode)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_inode_info *vi = VTXFS_I(inode);
	vtxfs_extent_map_tree_destroy(&vi->extent_map_tree);

	vtxfs_io_tree_destroy(&vi->file_extent_tree);
	vtxfs_io_tree_destroy(&vi->io_tree);
	kmem_cache_free(vtxfs_inode_cachep, vi);
}

/* Caller must hold sbi->op_lock. */
int vtxfs_refresh_inode(struct inode *inode)
{
	VTXFS_TRACE_ENTER();

	struct vtx_inode_item st;
	int ret;

	ret = vtxfs_inode_item_load(inode, &st);
	if (ret)
		return ret;
	vtxfs_inode_item_apply(inode, &st, true);
	return 0;
}

struct inode_data {
	u64 ino;
};

static int vtxfs_test_inode(struct inode *inode, void *data)
{
	VTXFS_TRACE_ENTER();

	return VTXFS_I(inode)->vtx_ino == ((struct inode_data *)data)->ino;
}

static int vtxfs_set_inode(struct inode *inode, void *data)
{
	VTXFS_TRACE_ENTER();

	VTXFS_I(inode)->vtx_ino = ((struct inode_data *)data)->ino;
	return 0;
}

struct inode *vtxfs_iget(struct super_block *sb, u64 ino)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	struct inode_data data = { .ino = ino };
	struct vtx_inode_item st;
	struct inode *inode;
	int ret;

	inode = iget5_locked(sb, (unsigned long)ino, vtxfs_test_inode,
			     vtxfs_set_inode, &data);
	if (!inode)
		return ERR_PTR(-ENOMEM);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	if (!(inode_state_read_once(inode) & I_NEW))
		return inode;
#else
	if (!(inode->i_state & I_NEW))
		return inode;
#endif

	inode->i_ino = (unsigned long)ino;

	vtxfs_op_begin(sbi);
	ret = vtx_stat(sbi->fs, vtxfs_active_subvol(sb), ino, &st);
	vtxfs_op_end(sbi);
	if (ret) {
		iget_failed(inode);
		return ERR_PTR(ret);
	}

	vtxfs_inode_item_apply(inode, &st, true);

	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &vtxfs_dir_iops;
		inode->i_fop = &vtxfs_dir_fops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &vtxfs_file_iops;
		inode->i_fop = &vtxfs_file_fops;
		inode->i_mapping->a_ops = &vtxfs_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &vtxfs_symlink_iops;
	} else {
		inode->i_op = &vtxfs_file_iops;
		init_special_inode(inode, inode->i_mode,
				   new_decode_dev((u32)vtx_cpu64(st.u.rdev)));
	}

	unlock_new_inode(inode);
	return inode;
}

static int vtxfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			 struct kstat *stat, u32 request_mask,
			 unsigned int query_flags)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = d_inode(path->dentry);

	(void)query_flags;
	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

static struct dentry *vtxfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	VTXFS_TRACE_ENTER();

	struct super_block *sb = dir->i_sb;
	u64 ino = 0;
	u8 ftype = 0;
	struct inode *inode;
	int ret;

	(void)flags;
	if (dentry->d_name.len > VTXFS_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	ret = vtxfs_dir_lookup(dir, &dentry->d_name, &ino, &ftype);
	if (ret == -ENOENT) {
		d_add(dentry, NULL);
		return NULL;
	}
	if (ret)
		return ERR_PTR(ret);

	inode = vtxfs_iget(sb, ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_splice_alias(inode, dentry);
}

#define VTXFS_INSTANTIATE(sb, ino)					\
({									\
	struct inode *__i = vtxfs_iget(sb, ino);			\
	if (!IS_ERR(__i))						\
		d_instantiate(dentry, __i);				\
	PTR_ERR_OR_ZERO(__i);						\
})

static int vtxfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	VTXFS_TRACE_ENTER();

	u64 ino = 0;
	int ret;

	(void)idmap;
	(void)excl;
	ret = vtxfs_dir_create(dir, &dentry->d_name, mode, &ino);
	return ret ? ret : VTXFS_INSTANTIATE(dir->i_sb, ino);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
static struct dentry *vtxfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	VTXFS_TRACE_ENTER();

	u64 ino = 0;
	int ret;

	(void)idmap;
	ret = vtxfs_dir_mkdir(dir, &dentry->d_name, mode, &ino);
	if (ret)
		return ERR_PTR(ret);
	ret = VTXFS_INSTANTIATE(dir->i_sb, ino);
	if (ret)
		return ERR_PTR(ret);
	inc_nlink(dir);
	return NULL;
}
#else
static int vtxfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode)
{
	VTXFS_TRACE_ENTER();

	u64 ino = 0;
	int ret;

	(void)idmap;
	ret = vtxfs_dir_mkdir(dir, &dentry->d_name, mode, &ino);
	if (ret)
		return ret;
	ret = VTXFS_INSTANTIATE(dir->i_sb, ino);
	if (!ret)
		inc_nlink(dir);
	return ret;
}
#endif

static int vtxfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *symname)
{
	VTXFS_TRACE_ENTER();

	u64 ino = 0;
	int ret;

	(void)idmap;
	ret = vtxfs_dir_symlink(dir, &dentry->d_name, symname, &ino);
	return ret ? ret : VTXFS_INSTANTIATE(dir->i_sb, ino);
}

static int vtxfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, dev_t rdev)
{
	VTXFS_TRACE_ENTER();

	u64 ino = 0;
	int ret;

	(void)idmap;
	ret = vtxfs_dir_mknod(dir, &dentry->d_name, mode, rdev, &ino);
	return ret ? ret : VTXFS_INSTANTIATE(dir->i_sb, ino);
}

static int vtxfs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	if (!inode || inode->i_sb != dir->i_sb)
		return -EXDEV;
	if (!vtxfs_dir_name_valid(dentry->d_name.name, dentry->d_name.len))
		return -EINVAL;
	if (!S_ISREG(inode->i_mode))
		return -EPERM;
	vtxfs_op_begin(sbi);
	ret = vtx_link(sbi->fs, sbi->subvol_id, VTXFS_I(inode)->vtx_ino,
		       VTXFS_I(dir)->vtx_ino, dentry->d_name.name);
	vtxfs_op_end(sbi);
	if (ret)
		return ret;
	ihold(inode);
	d_instantiate(dentry, inode);
	inc_nlink(inode);
	return 0;
}

static int vtxfs_unlink(struct inode *dir, struct dentry *dentry)
{
	VTXFS_TRACE_ENTER();

	int ret = vtxfs_dir_unlink(dir, &dentry->d_name);

	if (!ret)
		drop_nlink(d_inode(dentry));
	return ret;
}

static int vtxfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	VTXFS_TRACE_ENTER();

	int ret = vtxfs_dir_rmdir(dir, &dentry->d_name);

	if (!ret) {
		clear_nlink(d_inode(dentry));
		drop_nlink(dir);
	}
	return ret;
}

static int vtxfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			struct dentry *old_dentry, struct inode *new_dir,
			struct dentry *new_dentry, unsigned int flags)
{
	VTXFS_TRACE_ENTER();

	int ret;

	(void)idmap;
	ret = vtxfs_dir_rename(old_dir, old_dentry, new_dir, new_dentry,
			       flags);
	if (!ret && !(flags & RENAME_EXCHANGE) && d_inode(new_dentry) &&
	    d_inode(new_dentry) != d_inode(old_dentry))
		drop_nlink(d_inode(new_dentry));
	return ret;
}

static int vtxfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			 struct iattr *iattr)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = d_inode(dentry);
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	u64 subvol = vtxfs_active_subvol(inode->i_sb);
	int ret;

	ret = setattr_prepare(idmap, dentry, iattr);
	if (ret)
		return ret;

	vtxfs_op_begin(sbi);
	if (iattr->ia_valid & ATTR_MODE) {
		ret = vtx_chmod(sbi->fs, subvol,
				VTXFS_I(inode)->vtx_ino, iattr->ia_mode);
		if (ret)
			goto out_unlock;
	}
	if (iattr->ia_valid & (ATTR_UID | ATTR_GID)) {
		kuid_t uid = (iattr->ia_valid & ATTR_UID) ?
				iattr->ia_uid : inode->i_uid;
		kgid_t gid = (iattr->ia_valid & ATTR_GID) ?
				iattr->ia_gid : inode->i_gid;

		ret = vtx_chown(sbi->fs, subvol,
				VTXFS_I(inode)->vtx_ino,
				from_kuid(&init_user_ns, uid),
				from_kgid(&init_user_ns, gid));
		if (ret)
			goto out_unlock;
	}
	if (iattr->ia_valid & (ATTR_ATIME | ATTR_MTIME)) {
		ret = vtx_utimes(sbi->fs, subvol,
				 VTXFS_I(inode)->vtx_ino,
				 (u64)timespec64_to_ns(&iattr->ia_atime),
				 (u64)timespec64_to_ns(&iattr->ia_mtime));
		if (ret)
			goto out_unlock;
	}
	if (iattr->ia_valid & ATTR_SIZE) {
		ret = vtx_truncate(sbi->fs, subvol,
				   VTXFS_I(inode)->vtx_ino, iattr->ia_size);
		if (ret)
			goto out_unlock;
		/* Native ctree is read-only shadow state.  The engine already
		 * owns this truncate and the CPD/VAG/REF transaction. */
		truncate_setsize(inode, iattr->ia_size);
	}
out_unlock:
	vtxfs_op_end(sbi);
	if (ret)
		return ret;

	setattr_copy(idmap, inode, iattr);
	mark_inode_dirty(inode);
	return 0;
}

static void vtxfs_free_link(void *p)
{
	VTXFS_TRACE_ENTER();

	kfree(p);
}

static const char *vtxfs_get_link(struct dentry *dentry, struct inode *inode,
				  struct delayed_call *done)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	char *buf;
	size_t len = 0;
	int ret;

	/* RCU-walk symlink lookup is not supported by this scaffold. */
	if (!dentry)
		return ERR_PTR(-ECHILD);

	buf = kmalloc(VTXFS_MAX_INLINE_DATA + 1, GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	vtxfs_op_begin(sbi);
	ret = vtx_readlink(sbi->fs, vtxfs_active_subvol(inode->i_sb),
			   VTXFS_I(inode)->vtx_ino, buf,
			   VTXFS_MAX_INLINE_DATA + 1, &len);
	vtxfs_op_end(sbi);
	if (ret) {
		kfree(buf);
		return ERR_PTR(ret);
	}

	set_delayed_call(done, vtxfs_free_link, buf);
	return buf;
}

const struct inode_operations vtxfs_dir_iops = {
	.lookup		= vtxfs_lookup,
	.create		= vtxfs_create,
	.mkdir		= vtxfs_mkdir,
	.mknod		= vtxfs_mknod,
	.symlink	= vtxfs_symlink,
	.link		= vtxfs_link,
	.unlink		= vtxfs_unlink,
	.rmdir		= vtxfs_rmdir,
	.rename		= vtxfs_rename,
	.setattr	= vtxfs_setattr,
	.getattr	= vtxfs_getattr,
	.listxattr	= vtxfs_listxattr,
};

const struct inode_operations vtxfs_file_iops = {
	.setattr	= vtxfs_setattr,
	.getattr	= vtxfs_getattr,
	.listxattr	= vtxfs_listxattr,
	.fiemap		= vtxfs_fiemap,
};

const struct inode_operations vtxfs_symlink_iops = {
	.get_link	= vtxfs_get_link,
	.getattr	= vtxfs_getattr,
	.listxattr	= vtxfs_listxattr,
};
