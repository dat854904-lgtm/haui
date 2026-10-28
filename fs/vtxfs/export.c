// SPDX-License-Identifier: GPL-2.0-only
/* export.c — NFS export_operations (FILEID_INO32_GEN, gen ignored in P0). */
#include <linux/exportfs.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

static int vtxfs_encode_fh(struct inode *inode, __u32 *fh, int *max_len,
			   struct inode *parent)
{
	VTXFS_TRACE_ENTER();

	u64 ino = VTXFS_I(inode)->vtx_ino;

	(void)parent;
	if (*max_len < 3)
		return FILEID_INVALID;
	fh[0] = (u32)(ino & 0xFFFFFFFFULL);
	fh[1] = (u32)(ino >> 32);
	fh[2] = 0;	/* P0 inode item has no generation field */
	*max_len = 3;
	return FILEID_INO32_GEN;
}

static struct dentry *vtxfs_fh_to_dentry(struct super_block *sb,
					 struct fid *fid, int fh_len,
					 int fh_type)
{
	VTXFS_TRACE_ENTER();

	u64 ino;
	struct inode *inode;

	if (fh_len < 3 || fh_type != FILEID_INO32_GEN)
		return NULL;
	ino = (u64)fid->raw[0] | ((u64)fid->raw[1] << 32);
	if (ino < VTXFS_INO_ROOT_DIR)
		return NULL;
	inode = vtxfs_iget(sb, ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_obtain_alias(inode);
}

static struct dentry *vtxfs_fh_to_parent(struct super_block *sb,
					 struct fid *fid, int fh_len,
					 int fh_type)
{
	VTXFS_TRACE_ENTER();

	return vtxfs_fh_to_dentry(sb, fid, fh_len, fh_type);
}

static struct dentry *vtxfs_get_parent(struct dentry *child)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = d_inode(child);
	u64 pino = 0;
	struct inode *parent;
	int ret;

	ret = vtxfs_inode_item_parent(inode, &pino);
	if (ret)
		return ERR_PTR(ret);
	if (pino < VTXFS_INO_ROOT_DIR)
		return ERR_PTR(-EUCLEAN);
	parent = vtxfs_iget(inode->i_sb, pino);
	if (IS_ERR(parent))
		return ERR_CAST(parent);
	return d_obtain_alias(parent);
}

const struct export_operations vtxfs_export_ops = {
	.encode_fh	= vtxfs_encode_fh,
	.fh_to_dentry	= vtxfs_fh_to_dentry,
	.fh_to_parent	= vtxfs_fh_to_parent,
	.get_parent	= vtxfs_get_parent,
};
