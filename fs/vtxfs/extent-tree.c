// SPDX-License-Identifier: GPL-2.0-only
/*
 * extent-tree.c — allocation hook, REF/SFQ tree items and delayed refs,
 * operating through the ctree path layer.
 */
#include <linux/slab.h>

#include "ctree.h"
#include "vtxfs_leaf.h"
#include "vtxfs_internal.h"
#include "vtxfs_trace.h"

int vtxfs_alloc_tree_block(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
			   u8 level, u64 owner_oid,
			   struct vtxfs_extent_buffer **eb_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;
	u64 vblk = 0;
	int ret;

	if (!fs->alloc_ops || !fs->alloc_ops->alloc_blocks)
		return -ENOTSUPP;
	ret = fs->alloc_ops->alloc_blocks(fs, fs->node_size >>
			VTXFS_BLOCK_SHIFT, &vblk);
	if (ret)
		return ret;
	eb = vtxfs_eb_alloc(fs, vblk, fs->node_size);
	if (IS_ERR(eb)) {
		fs->alloc_ops->free_blocks(fs, vblk,
				fs->node_size >> VTXFS_BLOCK_SHIFT);
		return PTR_ERR(eb);
	}
	if (level == 0) {
		ret = vtx_leaf_init(eb->hdr, fs->node_size, owner_oid,
				    fs->current_xid, vblk,
				    root ? root->tree_type : VTX_TREE_FS,
				    VTX_NODE_FLAG_LEAF);
	} else {
		ret = vtx_internal_init(eb->hdr, fs->node_size, owner_oid,
					fs->current_xid, vblk,
					root ? root->tree_type : VTX_TREE_FS,
					VTX_NODE_FLAG_PHYSICAL_VBLK, level);
	}
	if (ret) {
		vtxfs_eb_put(eb);
		fs->alloc_ops->free_blocks(fs, vblk,
				fs->node_size >> VTXFS_BLOCK_SHIFT);
		return ret;
	}
	eb->oid = owner_oid;
	vtxfs_mark_eb_dirty(eb);
	*eb_out = eb;
	return 0;
}

int vtxfs_alloc_extent(struct vtxfs_fs_info *fs, u32 blocks,
		       u64 *start_vblk, u64 *allocation_id)
{
	VTXFS_TRACE_ENTER();

	int ret;

	if (!fs->alloc_ops || !fs->alloc_ops->alloc_blocks)
		return -ENOTSUPP;
	ret = fs->alloc_ops->alloc_blocks(fs, blocks, start_vblk);
	if (ret)
		return ret;
	*allocation_id = (u64)atomic64_inc_return(&fs->next_object_id);
	return 0;
}

int vtxfs_add_extent_ref(struct vtxfs_fs_info *fs, u64 allocation_id,
			 u64 start_vblk, u32 blocks, u32 flags, u32 owner)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key = {
		.oid_and_type = cpu_to_le64(
			VTX_MAKE_OID_TYPE(allocation_id, VTX_TYPE_REF)),
		.offset = 0,
	};
	struct vtx_ref_item item, found;
	void *w = NULL;
	int ret;

	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot(fs->ref_root, path, &key, sizeof(item), 1);
	if (ret < 0)
		goto out;
	if (ret == 0) {
		/* bump refcount */
		memcpy(&found, (u8 *)path->nodes[0]->hdr +
		       vtx_cpu16(path->nodes[0]->hdr->entries
				[path->slots[0]].slot.val_off8) * 8U,
		       sizeof(found));
		found.refcount = cpu_to_le64(vtx_cpu64(found.refcount) + 1);
		memcpy((u8 *)path->nodes[0]->hdr +
		       vtx_cpu16(path->nodes[0]->hdr->entries
				[path->slots[0]].slot.val_off8) * 8U,
		       &found, sizeof(found));
		vtxfs_mark_eb_dirty(path->nodes[0]);
		goto out;
	}
	memset(&item, 0, sizeof(item));
	item.allocation_id = cpu_to_le64(allocation_id);
	item.start_vblk = cpu_to_le64(start_vblk);
	item.refcount = cpu_to_le64(1);
	item.birth_xid = cpu_to_le64(fs->current_xid);
	item.block_count = cpu_to_le32(blocks);
	item.flags = cpu_to_le32(flags);
	item.owner_hint = cpu_to_le32(owner);
	ret = vtxfs_insert_item(fs->ref_root, path, &key, sizeof(item), &w);
	if (!ret && w)
		memcpy(w, &item, sizeof(item));
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_drop_extent_ref(struct vtxfs_fs_info *fs, u64 allocation_id,
			  u64 start_vblk, u32 blocks)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key = {
		.oid_and_type = cpu_to_le64(
			VTX_MAKE_OID_TYPE(allocation_id, VTX_TYPE_REF)),
		.offset = 0,
	};
	struct vtx_ref_item found;
	u64 refcount;
	int ret;

	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot(fs->ref_root, path, &key, 0, 1);
	if (ret != 0) {
		ret = ret > 0 ? -ENOENT : ret;
		goto out;
	}
	memcpy(&found, (u8 *)path->nodes[0]->hdr +
	       vtx_cpu16(path->nodes[0]->hdr->entries
			[path->slots[0]].slot.val_off8) * 8U,
	       sizeof(found));
	refcount = vtx_cpu64(found.refcount);
	if (refcount == 0) {
		ret = -EUCLEAN;
		goto out;
	}
	if (refcount > 1) {
		found.refcount = cpu_to_le64(refcount - 1);
		memcpy((u8 *)path->nodes[0]->hdr +
		       vtx_cpu16(path->nodes[0]->hdr->entries
				[path->slots[0]].slot.val_off8) * 8U,
		       &found, sizeof(found));
		vtxfs_mark_eb_dirty(path->nodes[0]);
	} else {
		/* refcount -> 0: delete REF item, enroll SFQ entry */
		struct vtx_key sfq_key = {
			.oid_and_type = cpu_to_le64(VTX_MAKE_OID_TYPE(
				start_vblk, VTX_TYPE_SFQ)),
			.offset = cpu_to_le64(fs->current_xid),
		};
		struct vtx_sfq_entry sfq = {
			.start_vblk = cpu_to_le64(start_vblk),
			.block_count = cpu_to_le64(blocks),
			.retired_xid = cpu_to_le64(fs->current_xid),
			.flags = cpu_to_le32(
				(vtx_cpu32(found.flags) & VTX_REF_FLAG_META)
					? VTX_SFQ_FLAG_META : 0),
		};
		void *w = NULL;

		ret = vtxfs_del_item(fs->ref_root, path);
		vtxfs_release_path(path);
		if (ret)
			goto out;
		ret = vtxfs_search_slot(fs->sfq_root, path, &sfq_key,
					sizeof(sfq), 1);
		if (ret != 1) {
			ret = ret ? ret : -EEXIST;
			goto out;
		}
		ret = vtxfs_insert_item(fs->sfq_root, path, &sfq_key,
					sizeof(sfq), &w);
		if (!ret && w)
			memcpy(w, &sfq, sizeof(sfq));
	}
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_lookup_extent_ref(struct vtxfs_fs_info *fs, u64 start_vblk,
			    struct vtx_ref_item *out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key = {
		.oid_and_type = cpu_to_le64(
			VTX_MAKE_OID_TYPE(start_vblk, VTX_TYPE_ALLOC_BY_VBLK)),
		.offset = 0,
	};
	struct vtx_alloc_by_vblk_item rev;
	struct vtx_key ref_key;
	int ret;

	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot(fs->ref_root, path, &key, 0, 0);
	if (ret != 0) {
		ret = ret > 0 ? -ENOENT : ret;
		goto out;
	}
	memcpy(&rev, (u8 *)path->nodes[0]->hdr +
	       vtx_cpu16(path->nodes[0]->hdr->entries
			[path->slots[0]].slot.val_off8) * 8U, sizeof(rev));
	vtxfs_release_path(path);
	ref_key.oid_and_type = cpu_to_le64(VTX_MAKE_OID_TYPE(
			vtx_cpu64(rev.allocation_id), VTX_TYPE_REF));
	ref_key.offset = 0;
	ret = vtxfs_search_slot(fs->ref_root, path, &ref_key, 0, 0);
	if (ret != 0) {
		ret = ret > 0 ? -EUCLEAN : ret;
		goto out;
	}
	memcpy(out, (u8 *)path->nodes[0]->hdr +
	       vtx_cpu16(path->nodes[0]->hdr->entries
			[path->slots[0]].slot.val_off8) * 8U, sizeof(*out));
	ret = 0;
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_pin_extent(struct vtxfs_fs_info *fs, u64 start_vblk, u32 blocks)
{
	VTXFS_TRACE_ENTER();

	/* Pinned extents stay allocated until the checkpoint that retired
	 * them is committed; the SFQ promotion path unpins them. */
	return vtxfs_add_extent_ref(fs, 0, start_vblk, blocks,
				    VTX_REF_FLAG_DATA, 0);
}

