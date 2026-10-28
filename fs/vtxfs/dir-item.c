// SPDX-License-Identifier: GPL-2.0-only
/*
 * dir-item.c — directory policy layer: name validation and engine calls.
 * All calls run under (commit_sem read, op_lock) — atomic per operation.
 */
#include <linux/string.h>
#include <linux/kdev_t.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

int vtxfs_dir_name_valid(const char *name, size_t len)
{
	VTXFS_TRACE_ENTER();

	size_t i;

	if (!name || len == 0 || len > VTXFS_NAME_MAX)
		return 0;
	for (i = 0; i < len; i++)
		if (name[i] == '\0' || name[i] == '/')
			return 0;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return 0;
	return 1;
}

#define VTXFS_DIR_PRE(sbi, name)					\
	do {								\
		if (!vtxfs_dir_name_valid((name)->name, (name)->len))	\
			return -EINVAL;					\
		vtxfs_op_begin(sbi);					\
	} while (0)

int vtxfs_dir_lookup(struct inode *dir, const struct qstr *name,
		     u64 *ino_out, u8 *ftype_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	if (name->len > VTXFS_NAME_MAX)
		return -ENAMETOOLONG;
	VTXFS_DIR_PRE(sbi, name);
	ret = vtx_lookup(sbi->fs, sbi->subvol_id, VTXFS_I(dir)->vtx_ino,
			 name->name, ino_out, ftype_out);
	vtxfs_op_end(sbi);
	return ret;
}

int vtxfs_dir_create(struct inode *dir, const struct qstr *name,
		     umode_t mode, u64 *ino_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	VTXFS_DIR_PRE(sbi, name);
	ret = vtx_create(sbi->fs, sbi->subvol_id, VTXFS_I(dir)->vtx_ino,
			 name->name, (u32)mode, ino_out);
	vtxfs_op_end(sbi);
	return ret;
}

int vtxfs_dir_mkdir(struct inode *dir, const struct qstr *name,
		    umode_t mode, u64 *ino_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	VTXFS_DIR_PRE(sbi, name);
	ret = vtx_mkdir(sbi->fs, sbi->subvol_id, VTXFS_I(dir)->vtx_ino,
			name->name, (u32)mode, ino_out);
	vtxfs_op_end(sbi);
	return ret;
}

int vtxfs_dir_symlink(struct inode *dir, const struct qstr *name,
		      const char *target, u64 *ino_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	if (!target || !*target)
		return -EINVAL;
	VTXFS_DIR_PRE(sbi, name);
	ret = vtx_symlink(sbi->fs, sbi->subvol_id, VTXFS_I(dir)->vtx_ino,
			  name->name, target, ino_out);
	vtxfs_op_end(sbi);
	return ret;
}

int vtxfs_dir_mknod(struct inode *dir, const struct qstr *name,
		    umode_t mode, dev_t rdev, u64 *ino_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	VTXFS_DIR_PRE(sbi, name);
	ret = vtx_mknod(sbi->fs, sbi->subvol_id, VTXFS_I(dir)->vtx_ino,
			name->name, (u32)mode, (u64)new_encode_dev(rdev),
			ino_out);
	vtxfs_op_end(sbi);
	return ret;
}

int vtxfs_dir_unlink(struct inode *dir, const struct qstr *name)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	VTXFS_DIR_PRE(sbi, name);
	ret = vtx_unlink(sbi->fs, sbi->subvol_id, VTXFS_I(dir)->vtx_ino,
			 name->name);
	vtxfs_op_end(sbi);
	return ret;
}

int vtxfs_dir_rmdir(struct inode *dir, const struct qstr *name)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(dir->i_sb);
	int ret;

	VTXFS_DIR_PRE(sbi, name);
	ret = vtx_rmdir(sbi->fs, sbi->subvol_id, VTXFS_I(dir)->vtx_ino,
			name->name);
	vtxfs_op_end(sbi);
	return ret;
}

int vtxfs_dir_rename(struct inode *old_dir, struct dentry *old_dentry,
		     struct inode *new_dir, struct dentry *new_dentry,
		     unsigned int flags)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(old_dir->i_sb);
	int ret;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;
	if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE))
		return -EINVAL;
	if (!vtxfs_dir_name_valid(old_dentry->d_name.name,
				  old_dentry->d_name.len) ||
	    !vtxfs_dir_name_valid(new_dentry->d_name.name,
				  new_dentry->d_name.len))
		return -EINVAL;

	vtxfs_op_begin(sbi);
	if (flags & RENAME_EXCHANGE) {
		ret = vtx_rename_exchange(sbi->fs, sbi->subvol_id,
					  VTXFS_I(old_dir)->vtx_ino,
					  old_dentry->d_name.name,
					  VTXFS_I(new_dir)->vtx_ino,
					  new_dentry->d_name.name);
		goto out;
	}
	if (flags & RENAME_NOREPLACE) {
		u64 tmp_ino = 0;
		u8 tmp_ft = 0;

		ret = vtx_lookup(sbi->fs, sbi->subvol_id,
				 VTXFS_I(new_dir)->vtx_ino,
				 new_dentry->d_name.name, &tmp_ino, &tmp_ft);
		if (ret == 0)
			ret = -EEXIST;
		else if (ret == -ENOENT)
			ret = 0;
		if (ret)
			goto out;
	}
	ret = vtx_rename(sbi->fs, sbi->subvol_id,
			 VTXFS_I(old_dir)->vtx_ino,
			 old_dentry->d_name.name,
			 VTXFS_I(new_dir)->vtx_ino,
			 new_dentry->d_name.name);
out:
	vtxfs_op_end(sbi);
	return ret;
}
