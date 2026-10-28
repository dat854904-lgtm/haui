// SPDX-License-Identifier: GPL-2.0-only
/* root-tree.c — enumerate VTX_TYPE_SUBVOL_ROOT items from VTX_TREE_ROOT. */
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

/* Caller must hold sbi->op_lock. */
int vtxfs_roots_scan(struct vtxfs_sb_info *sbi)
{
	VTXFS_TRACE_ENTER();

	struct vtx_fs *fs = sbi->fs;
	struct vtx_key cur = {0}, found;
	struct vtx_subvol_root_item item;
	size_t bytes;
	int first = 1, ret, count = 0;

	lockdep_assert_held(&sbi->op_lock);
	for (;;) {
		u64 id;

		bytes = sizeof(item);
		ret = vtx_rw_next(&fs->root_tree, &cur, first, &found,
				  &item, &bytes);
		if (ret == -ENOENT)
			return count;
		if (ret == -ENOBUFS) {
			first = 0;
			cur = found;
			continue;
		}
		if (ret)
			return ret;
		first = 0;
		cur = found;

		if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_SUBVOL_ROOT)
			continue;
		if (bytes != sizeof(item))
			return -EUCLEAN;

		id = VTX_KEY_GET_OID(vtx_cpu64(found.oid_and_type));
		ret = vtxfs_root_register(sbi, id, &item);
		if (ret && ret != -EBUSY)
			return ret;
		count++;
	}
}
