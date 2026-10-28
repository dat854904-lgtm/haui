// SPDX-License-Identifier: GPL-2.0-only
/* inode-item.c — mapping between engine vtx_inode_item and VFS inode. */
#include <linux/time64.h>
#include <linux/kdev_t.h>
#include <linux/version.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

void vtxfs_inode_item_apply(struct inode *inode,
			    const struct vtx_inode_item *st, bool with_times)
{
	VTXFS_TRACE_ENTER();

	bool is_dirty;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	is_dirty = (inode_state_read_once(inode) & I_DIRTY) ||
		   mapping_tagged(inode->i_mapping, PAGECACHE_TAG_DIRTY);
#else
	is_dirty = (inode->i_state & I_DIRTY) ||
		   mapping_tagged(inode->i_mapping, PAGECACHE_TAG_DIRTY);
#endif

	inode->i_mode = vtx_cpu32(st->mode);
	i_uid_write(inode, vtx_cpu32(st->uid));
	i_gid_write(inode, vtx_cpu32(st->gid));
	if (!is_dirty)
		inode->i_size = vtx_cpu64(st->size_bytes);
	else if (vtx_cpu64(st->size_bytes) > inode->i_size)
		inode->i_size = vtx_cpu64(st->size_bytes);
	set_nlink(inode, vtx_cpu32(st->nlink));
	inode->i_blocks = vtx_cpu64(st->alloc_bytes) >> SECTOR_SHIFT;
	if (with_times) {
		inode_set_atime_to_ts(inode,
			ns_to_timespec64((s64)vtx_cpu64(st->atime_ns)));
		inode_set_mtime_to_ts(inode,
			ns_to_timespec64((s64)vtx_cpu64(st->mtime_ns)));
		inode_set_ctime_to_ts(inode,
			ns_to_timespec64((s64)vtx_cpu64(st->ctime_ns)));
	}
	VTXFS_I(inode)->disk = *st;
}

/* Caller must hold sbi->op_lock. */
int vtxfs_inode_item_load(struct inode *inode, struct vtx_inode_item *st)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);

	lockdep_assert_held(&sbi->op_lock);
	return vtx_stat(sbi->fs, sbi->subvol_id, VTXFS_I(inode)->vtx_ino, st);
}

int vtxfs_inode_item_parent(struct inode *inode, u64 *parent_ino)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	struct vtx_inode_item st;
	int ret;

	vtxfs_op_begin(sbi);
	ret = vtxfs_inode_item_load(inode, &st);
	if (ret == 0)
		*parent_ino = vtx_cpu64(st.parent_ino);
	vtxfs_op_end(sbi);
	return ret;
}
