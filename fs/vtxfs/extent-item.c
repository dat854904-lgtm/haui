// SPDX-License-Identifier: GPL-2.0-only
/*
 * extent-item.c — helper dựng vtx_extent_item (block extent, CRC32C/block)
 * và chèn vào cây FS qua ctree. Dùng bởi delalloc/flush và write path.
 */
#include <linux/slab.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

int vtxfs_build_extent_item(struct vtxfs_root *root, u64 ino, u64 file_off,
			    u64 vblk, u64 alloc_id, u32 nblocks,
			    const __le32 *csums)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_path *path;
	struct vtx_key key;
	struct vtx_extent_item *ei;
	size_t item_len = sizeof(*ei) + (size_t)nblocks * sizeof(__le32);
	void *w = NULL;
	int ret;

	if (!nblocks || nblocks > VTXFS_MAX_EXTENT_BLOCKS)
		return -EINVAL;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_EXTENT, file_off);
	if (ret)
		goto out;
	ret = vtxfs_search_slot_locked(root, path, &key, item_len, 1);
	if (ret != 1) {
		ret = ret ? ret : -EEXIST;
		goto out;
	}
	ret = vtxfs_insert_item(root, path, &key, item_len, &w);
	if (ret)
		goto out;
	ei = w;
	memset(ei, 0, sizeof(*ei));
	ei->vblk = cpu_to_le64(vblk);
	ei->allocation_id = cpu_to_le64(alloc_id);
	ei->birth_xid = cpu_to_le64(fs->current_xid);
	ei->owner_subvol_id = cpu_to_le64(root->subvol_id);
	ei->physical_blocks = cpu_to_le32(nblocks);
	ei->logical_bytes = cpu_to_le32(nblocks * VTXFS_BLOCK_SIZE);
	ei->flags = 0;
	ei->csum_type = cpu_to_le16(VTX_CSUM_CRC32C);
	ei->csum_count = cpu_to_le16(nblocks);
	ei->payload_bytes = 0;
	ei->compression = VTX_COMPR_NONE;
	ei->encryption = VTX_ENC_NONE;
	ei->header_bytes = cpu_to_le16(sizeof(*ei));
	memcpy(ei->tail, csums, (size_t)nblocks * sizeof(__le32));
	if (!vtx_extent_p0_value_valid(ei, item_len))
		ret = -EUCLEAN;
out:
	vtxfs_release_path_locked(path);
	vtxfs_free_path(path);
	return ret;
}

/* Tên alias mà delalloc.c tham chiếu. */
int insert_extent_item_crc(struct vtxfs_root *root, u64 ino, u64 file_off,
			   u64 vblk, u64 alloc_id, u32 nblocks,
			   const __le32 *csums)
{
	VTXFS_TRACE_ENTER();

	return vtxfs_build_extent_item(root, ino, file_off, vblk, alloc_id,
				       nblocks, csums);
}
