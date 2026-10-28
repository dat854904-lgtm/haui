// SPDX-License-Identifier: GPL-2.0-only
/*
 * VTXFS v2.1 P0 — xattr plumbing onto the portable engine.
 *
 * The engine stores attribute names with their full namespace prefix
 * (e.g. "user.author"), matching the userspace tools. Only the "user."
 * namespace is exposed by this scaffold; VFS dispatches through
 * sb->s_xattr (no per-inode getxattr/setxattr overrides needed).
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/xattr.h>

#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

static int vtxfs_xattr_full_name(const char *prefix, const char *name,
				 char *out, size_t out_size)
{
	VTXFS_TRACE_ENTER();

	size_t plen = strlen(prefix);
	size_t nlen = strlen(name);

	if (plen + nlen + 1 > out_size)
		return -EINVAL;
	memcpy(out, prefix, plen);
	memcpy(out + plen, name, nlen + 1);
	return 0;
}

static int vtxfs_generic_xattr_get(const struct xattr_handler *handler,
				   struct dentry *dentry, struct inode *inode,
				   const char *name, void *buffer, size_t size)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	char full[VTXFS_NAME_MAX + 1];
	size_t len = 0;
	int ret;

	(void)dentry;
	ret = vtxfs_xattr_full_name(handler->prefix, name,
				    full, sizeof(full));
	if (ret)
		return ret;

	vtxfs_op_begin(sbi);
	ret = vtx_getxattr(sbi->fs, sbi->subvol_id,
			   VTXFS_I(inode)->vtx_ino, full,
			   size ? buffer : NULL, size, &len);
	vtxfs_op_end(sbi);
	if (ret)
		return ret;
	return (int)len;
}

static int vtxfs_generic_xattr_set(const struct xattr_handler *handler,
				   struct mnt_idmap *idmap,
				   struct dentry *dentry, struct inode *inode,
				   const char *name, const void *value,
				   size_t size, int flags)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	char full[VTXFS_NAME_MAX + 1];
	int ret;

	(void)idmap;
	(void)dentry;
	if (sb_rdonly(inode->i_sb))
		return -EROFS;

	ret = vtxfs_xattr_full_name(handler->prefix, name,
				    full, sizeof(full));
	if (ret)
		return ret;
	if (size > 1024U)	/* engine P0 value cap */
		return -E2BIG;

	vtxfs_op_begin(sbi);
	if (flags & (XATTR_CREATE | XATTR_REPLACE)) {
		size_t existing = 0;
		int probe = vtx_getxattr(sbi->fs, sbi->subvol_id,
					 VTXFS_I(inode)->vtx_ino, full,
					 NULL, 0, &existing);

		if (probe == 0 && (flags & XATTR_CREATE))
			ret = -EEXIST;
		else if (probe == -ENOENT && (flags & XATTR_REPLACE))
			ret = -ENODATA;
		else if (probe != 0 && probe != -ENOENT)
			ret = probe;
		else
			ret = 0;
		if (ret) {
			vtxfs_op_end(sbi);
			return ret;
		}
	}

	if (!value) {
		ret = vtx_removexattr(sbi->fs, sbi->subvol_id,
				      VTXFS_I(inode)->vtx_ino, full);
	} else {
		ret = vtx_setxattr(sbi->fs, sbi->subvol_id,
				   VTXFS_I(inode)->vtx_ino, full,
				   value, size);
	}
	vtxfs_op_end(sbi);
	return ret;
}

static const struct xattr_handler vtxfs_user_xattr_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= vtxfs_generic_xattr_get,
	.set	= vtxfs_generic_xattr_set,
};

static const struct xattr_handler vtxfs_trusted_xattr_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.get	= vtxfs_generic_xattr_get,
	.set	= vtxfs_generic_xattr_set,
};

static const struct xattr_handler vtxfs_security_xattr_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= vtxfs_generic_xattr_get,
	.set	= vtxfs_generic_xattr_set,
};

const struct xattr_handler * const vtxfs_xattr_handlers[] = {
	&vtxfs_user_xattr_handler,
	&vtxfs_trusted_xattr_handler,
	&vtxfs_security_xattr_handler,
	NULL
};

/* inode_operations->listxattr: NUL-separated full names ("user.xxx"). */
ssize_t vtxfs_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = d_inode(dentry);
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	size_t len = 0;
	int ret;

	vtxfs_op_begin(sbi);
	ret = vtx_listxattr(sbi->fs, sbi->subvol_id,
			    VTXFS_I(inode)->vtx_ino,
			    size ? buffer : NULL, size, &len);
	vtxfs_op_end(sbi);
	if (ret)
		return ret;
	return (ssize_t)len;
}
