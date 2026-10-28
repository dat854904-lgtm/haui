// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_validate.h"
#include "vtxfs_trace.h"

static int build_block_extent(struct vtxfs_root *root, u64 ino, u64 file_off,
			      u64 vblk, u64 alloc_id, u32 blocks, u32 flags,
			      const __le32 *csums)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key;
	struct vtx_extent_item *ei;
	size_t item_len = sizeof(*ei) + (size_t)blocks * sizeof(__le32);
	void *w;
	int ret;

	if (!blocks || blocks > 256U)
		return -EINVAL;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_EXTENT, file_off);
	if (ret)
		goto out;
	ret = vtxfs_search_slot(root, path, &key, item_len, 1);
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
	ei->birth_xid = cpu_to_le64(root->fs->current_xid);
	ei->owner_subvol_id = cpu_to_le64(root->subvol_id);
	ei->physical_blocks = cpu_to_le32(blocks);
	ei->logical_bytes = cpu_to_le32(blocks * VTXFS_BLOCK_SIZE);
	ei->flags = cpu_to_le32(flags);
	ei->csum_type = cpu_to_le16(VTX_CSUM_CRC32C);
	ei->csum_count = cpu_to_le16(blocks);
	ei->header_bytes = cpu_to_le16(sizeof(*ei));
	if (csums)
		memcpy(ei->tail, csums, (size_t)blocks * sizeof(__le32));
	vtxfs_mark_eb_dirty(path->nodes[0]);
out:
	vtxfs_free_path(path);
	return ret;
}

/* Xoá/cắt extent gối lên nửa-mở [start,end). Gọi dưới trans_lock. */
int vtxfs_drop_extents(struct vtxfs_root *root,
		       struct vtxfs_drop_extents_args *args)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_path *path;
	struct vtx_key cur, found;
	u64 ino;
	void *ebuf;
	size_t bytes;
	int ret;

	if (args->end <= args->start)
		return 0;
	path = vtxfs_alloc_path();
	ebuf = kvmalloc(fs->node_size, GFP_NOFS);
	if (!path || !ebuf) {
		ret = -ENOMEM;
		goto out;
	}
	ino = vtxfs_key_oid(&args->resume_cursor);
	cur = args->resume_cursor;

	for (;;) {
		const struct vtx_extent_item *ext;
		u64 ext_start, ext_end, vblk, alloc_id;
		u32 flags, phys;
		const __le32 *csums;

		ret = vtxfs_search_slot(root, path, &cur, 0, 1);
		if (ret < 0)
			goto out;
		if (ret > 0 || path->slots[0] >= eb_nritems(path->nodes[0])) {
			ret = 0;
			break;
		}
		eb_key(path->nodes[0], path->slots[0], &found);
		if (vtxfs_key_oid(&found) != ino ||
		    vtxfs_key_type(&found) != VTX_TYPE_EXTENT)
			break;
		bytes = eb_val_len(path->nodes[0], path->slots[0]);
		if (bytes > fs->node_size || bytes < sizeof(*ext)) {
			ret = -EUCLEAN;
			goto out;
		}
		memcpy(ebuf, eb_val(path->nodes[0], path->slots[0]), bytes);
		ext = ebuf;
		if (!vtx_extent_p0_value_valid(ext, bytes)) {
			ret = -EUCLEAN;
			goto out;
		}
		ext_start = vtxfs_key_off(&found);
		flags = vtx_cpu32(ext->flags);
		if (flags & VTX_EXTENT_INLINE) {
			if (args->start == 0)
				ret = vtxfs_del_item(root, path);
			if (ret)
				goto out;
			goto next;
		}
		phys = vtx_cpu32(ext->physical_blocks);
		ext_end = ext_start + (u64)phys * VTXFS_BLOCK_SIZE;
		if (ext_end <= args->start)
			goto next;
		if (ext_start >= args->end)
			break;
		vblk = vtx_cpu64(ext->vblk);
		alloc_id = vtx_cpu64(ext->allocation_id);
		csums = (const __le32 *)ext->tail;

		if (ext_start >= args->start && ext_end <= args->end) {
			/* phủ toàn bộ: xoá + giảm ref */
			ret = vtxfs_del_item(root, path);
			if (ret)
				goto out;
			args->refs_dropped++;
			args->bytes_found += ext_end - ext_start;
		} else if (ext_start < args->start && ext_end <= args->end) {
			/* cắt đuôi phải, giữ [ext_start, args->start) */
			u32 keep = (args->start - ext_start) >>
				   VTXFS_BLOCK_SHIFT;

			ret = vtxfs_del_item(root, path);
			vtxfs_release_path(path);
			if (ret)
				goto out;
			ret = build_block_extent(root, ino, ext_start, vblk,
					alloc_id, keep, flags, csums);
			if (ret)
				goto out;
			goto loop_continue;
		} else if (ext_start >= args->start && ext_end > args->end) {
			/* cắt đầu trái, giữ [args->end, ext_end) */
			u32 skip = (args->end - ext_start) >>
				   VTXFS_BLOCK_SHIFT;

			ret = vtxfs_del_item(root, path);
			vtxfs_release_path(path);
			if (ret)
				goto out;
			ret = build_block_extent(root, ino, args->end,
					vblk + skip, alloc_id, phys - skip,
					flags, csums + skip);
			if (ret)
				goto out;
			goto loop_continue;
		} else {
			/* tách đôi: giữ trái + phải, đánh dấu SHARED */
			u32 left = (args->start - ext_start) >>
				   VTXFS_BLOCK_SHIFT;
			u32 roff = (args->end - ext_start) >>
				   VTXFS_BLOCK_SHIFT;

			ret = vtxfs_del_item(root, path);
			vtxfs_release_path(path);
			if (ret)
				goto out;
			ret = build_block_extent(root, ino, ext_start, vblk,
					alloc_id, left,
					flags | VTX_EXTENT_SHARED, csums);
			if (ret)
				goto out;
			ret = build_block_extent(root, ino, args->end,
					vblk + roff, alloc_id, phys - roff,
					flags | VTX_EXTENT_SHARED,
					csums + roff);
			if (ret)
				goto out;
			goto loop_continue;
		}
next:
		vtxfs_release_path(path);
loop_continue:
		cur = found;
		cur.offset = cpu_to_le64(vtxfs_key_off(&found) + 1);
	}
	ret = 0;
out:
	kvfree(ebuf);
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_truncate_inode(struct vtxfs_root *root, struct vtxfs_inode *vinode,
			 u64 new_size)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_truncate_control tc = { };
	struct vtxfs_drop_extents_args args = { };
	u64 old_size = vinode->disk_i_size;
	u64 al_new, al_old;
	int ret;

	if (new_size >= old_size)
		return 0;
	tc.inode = vinode;
	tc.new_size = new_size;
	tc.last_size = old_size;
	tc.max_items_per_transaction = 64;

	if (new_size == 0) {
		al_old = (old_size + VTXFS_BLOCK_SIZE - 1) &
			 ~((u64)VTXFS_BLOCK_SIZE - 1);
		if (al_old < VTXFS_BLOCK_SIZE)
			al_old = VTXFS_BLOCK_SIZE;
		args.start = 0;
		args.end = al_old;
	} else {
		al_new = (new_size + VTXFS_BLOCK_SIZE - 1) &
			 ~((u64)VTXFS_BLOCK_SIZE - 1);
		al_old = (old_size + VTXFS_BLOCK_SIZE - 1) &
			 ~((u64)VTXFS_BLOCK_SIZE - 1);
		args.start = al_new;
		args.end = al_old > al_new ? al_old : al_new;
	}
	vtxfs_mk_key(&args.resume_cursor, vinode->ino ? vinode->ino : vinode->vtx_ino,
		     VTX_TYPE_EXTENT, 0);
	ret = vtxfs_drop_extents(root, &args);
	if (ret)
		return ret;

	vinode->disk.size_bytes = cpu_to_le64(new_size);
	vinode->disk_i_size = new_size;
	return vtxfs_insert_inode(root, vinode->ino ? vinode->ino : vinode->vtx_ino,
				  &vinode->disk);
}
