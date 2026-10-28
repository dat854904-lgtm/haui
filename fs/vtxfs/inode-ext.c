// SPDX-License-Identifier: GPL-2.0-only
/*
 * inode-ext.c — deep inode item operations, inline extents and the
 * async delalloc pipeline (P1 scaffold; compression hook is policy-only).
 */
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>

#include "vtxfs_drv.h"
#include "ctree.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"

int vtxfs_lookup_inode(struct vtxfs_root *root, u64 ino,
		       struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	struct vtx_key key;
	int ret;

	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_INODE, 0);
	if (ret)
		return ret;
	ret = vtxfs_search_slot(root, path, &key, 0, 0);
	return ret > 0 ? -ENOENT : ret;
}

int vtxfs_insert_inode(struct vtxfs_root *root, u64 ino,
		       const struct vtx_inode_item *item)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	void *w = NULL;
	int ret;

	if (!path)
		return -ENOMEM;
	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_INODE, 0);
	if (ret)
		goto out;
	ret = vtxfs_search_slot(root, path, &key, sizeof(*item), 1);
	if (ret != 1) {
		ret = ret ? ret : -EEXIST;
		goto out;
	}
	ret = vtxfs_insert_item(root, path, &key, sizeof(*item), &w);
	if (!ret && w)
		memcpy(w, item, sizeof(*item));
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_insert_inode_ref(struct vtxfs_root *root, u64 ino, u64 dir_ino,
			   const char *name, u16 namelen)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	struct vtx_inode_ref *ref;
	size_t item_len = sizeof(*ref) + namelen;
	void *w = NULL;
	int ret;

	if (!path)
		return -ENOMEM;
	if (namelen > VTXFS_NAME_MAX) {
		vtxfs_free_path(path);
		return -EINVAL;
	}
	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_INODE_REF, dir_ino);
	if (ret)
		goto out;
	ret = vtxfs_search_slot(root, path, &key, (int)item_len, 1);
	if (ret != 1) {
		ret = ret ? ret : -EEXIST;
		goto out;
	}
	ret = vtxfs_insert_item(root, path, &key, (u32)item_len, &w);
	if (!ret && w) {
		ref = w;
		memset(ref, 0, sizeof(*ref));
		ref->dir_seq = cpu_to_le64(
			(u64)atomic64_inc_return(&root->fs->next_object_id));
		ref->name_len = cpu_to_le16(namelen);
		memcpy(ref->name, name, namelen);
	}
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_lookup_inode_ref(struct vtxfs_root *root, u64 ino, u64 dir_ino,
			   struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	struct vtx_key key;
	int ret;

	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_INODE_REF, dir_ino);
	if (ret)
		return ret;
	ret = vtxfs_search_slot(root, path, &key, 0, 0);
	return ret > 0 ? -ENOENT : ret;
}

/* ---------------- inline extents ---------------- */

bool can_cow_file_range_inline(struct inode *inode, u64 size)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_inode *vinode = VTXFS_I(inode);

	if (size == 0 || size > VTXFS_MAX_INLINE_DATA)
		return false;
	if (!S_ISREG(inode->i_mode) && !S_ISLNK(inode->i_mode))
		return false;
	if (vinode->disk.flags &
	    cpu_to_le64(VTX_INODE_NODATACOW | VTX_INODE_NODATASUM))
		return false;	/* never allowed in P0 anyway */
	if (i_size_read(inode) > (loff_t)VTXFS_MAX_INLINE_DATA)
		return false;
	return true;
}

int insert_inline_extent(struct inode *inode, struct vtxfs_path *path,
			 u64 start, const void *data, size_t size)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *root = VTXFS_I(inode)->root;
	struct vtxfs_extent_buffer *leaf;
	struct vtx_key key;
	struct vtx_extent_item *ei;
	size_t item_len = sizeof(*ei) + size;
	bool put_root = false;
	int ret;

	if (start != 0 || !can_cow_file_range_inline(inode, size))
		return -EINVAL;
	if (!root) {
		root = vtxfs_root_get(inode->i_sb,
				      vtxfs_active_subvol(inode->i_sb));
		if (!root)
			return -ENOENT;
		put_root = true;
	}
	ret = vtxfs_mk_key(&key, VTXFS_I_INO(inode), VTX_TYPE_EXTENT, 0);
	if (ret)
		goto out;
	ret = vtxfs_search_slot(root, path, &key, (int)item_len, 1);
	if (ret < 0)
		goto out;
	leaf = path->nodes[0];
	if (ret == 1) {
		void *w = NULL;

		ret = vtxfs_insert_item(root, path, &key, (u32)item_len, &w);
		if (ret || !w) {
			if (!ret)
				ret = -EIO;
			goto out;
		}
		ei = w;
		memset(ei, 0, sizeof(*ei));
	} else {
		ret = vtxfs_extend_item(root, path, (u32)item_len);
		if (ret)
			goto out;
		ei = (struct vtx_extent_item *)((u8 *)leaf->hdr +
			vtx_cpu16(leaf->hdr->entries[path->slots[0]]
				.slot.val_off8) * 8U);
	}
	ei->birth_xid = cpu_to_le64(leaf->fs->current_xid);
	ei->owner_subvol_id = cpu_to_le64(vtx_cpu64(leaf->hdr->owner_id));
	ei->logical_bytes = cpu_to_le32((u32)size);
	ei->flags = cpu_to_le32(VTX_EXTENT_INLINE);
	ei->csum_type = cpu_to_le16(VTX_CSUM_NONE);
	ei->payload_bytes = cpu_to_le32((u32)size);
	ei->header_bytes = cpu_to_le16(sizeof(*ei));
	memcpy(ei->tail, data, size);
	if (!vtx_extent_p0_value_valid(ei, item_len)) {
		ret = -EUCLEAN;
		goto out;
	}
	vtxfs_mark_eb_dirty(leaf);
	ret = 0;
out:
	if (put_root)
		vtxfs_root_put(root);
	return ret;
}

/* ---------------- compression decision (P0 policy) ---------------- */

bool inode_need_compress(struct inode *inode, u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	/* VTXFS_FEAT_INCOMPAT_COMPRESS is DEFERRED: the decision hook is
	 * implemented so call sites are stable, but P0 never compresses. */
	struct vtxfs_inode *vinode = VTXFS_I(inode);
	u64 flags = vtx_cpu64(vinode->disk.flags);

	if (end < start)
		return false;
	if (flags & VTX_INODE_NOCOMPRESS)
		return false;
	if (!(flags & VTX_INODE_COMPRESS))
		return false;
	if (end - start + 1 < VTXFS_BLOCK_SIZE)
		return false;
	return false;	/* DEFERRED until compression protocol exists */
}

/* ---------------- async delalloc ---------------- */

void add_async_extent(struct vtxfs_async_cow *async_cow,
		      u64 start, u64 end, struct inode *inode)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_async_extent *ae;

	ae = kmalloc(sizeof(*ae), GFP_NOFS);
	if (!ae)
		return;
	ae->start = start;
	ae->end = end;
	ae->inode = inode;
	list_add_tail(&ae->list, &async_cow->extents);
}

/*
 * Allocate blocks, checksum each 4 KiB slice with CRC32C and insert the
 * extent item through the ctree path layer. `data` is the fully assembled
 * range content (writeback.c gathers pages before calling this).
 */
int cow_file_range(struct inode *inode, u64 start, u64 end, const u8 *data)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = VTXFS_FSI(inode->i_sb);
	struct vtxfs_root *root = VTXFS_I(inode)->root;
	struct vtxfs_path *path;
	struct vtx_key key;
	struct vtx_extent_item *ei;
	__le32 *csums;
	u8 *block_buf;
	u64 vblk = 0, alloc_id = 0;
	u64 aligned_start, aligned_end;
	u32 nblocks, b;
	size_t item_len;
	void *w = NULL;
	bool put_root = false;
	int ret;

	aligned_start = start & ~(u64)(VTXFS_BLOCK_SIZE - 1);
	aligned_end = ALIGN(end + 1, VTXFS_BLOCK_SIZE);
	nblocks = (u32)((aligned_end - aligned_start) >> VTXFS_BLOCK_SHIFT);

	if (can_cow_file_range_inline(inode, end - start + 1)) {
		path = vtxfs_alloc_path();
		if (!path)
			return -ENOMEM;
		ret = insert_inline_extent(inode, path, start,
					   data, end - start + 1);
		vtxfs_free_path(path);
		return ret;
	}

	if (!root) {
		root = vtxfs_root_get(inode->i_sb,
				      vtxfs_active_subvol(inode->i_sb));
		if (!root)
			return -ENOENT;
		put_root = true;
	}

	ret = vtxfs_alloc_extent(fs, nblocks, &vblk, &alloc_id);
	if (ret) {
		if (put_root)
			vtxfs_root_put(root);
		return ret;
	}

	csums = kmalloc_array(nblocks, sizeof(*csums), GFP_NOFS);
	block_buf = kzalloc(VTXFS_BLOCK_SIZE, GFP_NOFS);
	if (!csums || !block_buf) {
		kfree(csums);
		kfree(block_buf);
		if (put_root)
			vtxfs_root_put(root);
		return -ENOMEM;
	}
	for (b = 0; b < nblocks; b++) {
		u64 off = aligned_start + (u64)b * VTXFS_BLOCK_SIZE;
		u32 len = VTXFS_BLOCK_SIZE;

		memset(block_buf, 0, VTXFS_BLOCK_SIZE);
		if (off <= end) {
			if (off + len > end + 1)
				len = (u32)(end + 1 - off);
			memcpy(block_buf, data + (off - start), len);
		}
		csums[b] = cpu_to_le32(vtx_crc32c(block_buf, VTXFS_BLOCK_SIZE));
	}
	kfree(block_buf);

	item_len = sizeof(*ei) + (size_t)nblocks * sizeof(__le32);
	path = vtxfs_alloc_path();
	if (!path) {
		kfree(csums);
		if (put_root)
			vtxfs_root_put(root);
		return -ENOMEM;
	}
	ret = vtxfs_mk_key(&key, VTXFS_I_INO(inode), VTX_TYPE_EXTENT,
			   aligned_start);
	if (ret)
		goto out;
	ret = vtxfs_search_slot(root, path, &key, (int)item_len, 1);
	if (ret != 1) {
		ret = ret ? ret : -EEXIST;
		goto out;
	}
	ret = vtxfs_insert_item(root, path, &key, (u32)item_len, &w);
	if (ret || !w) {
		if (!ret)
			ret = -EIO;
		goto out;
	}
	ei = w;
	memset(ei, 0, sizeof(*ei));
	ei->vblk = cpu_to_le64(vblk);
	ei->allocation_id = cpu_to_le64(alloc_id);
	ei->birth_xid = cpu_to_le64(fs->current_xid);
	ei->physical_blocks = cpu_to_le32(nblocks);
	ei->logical_bytes = cpu_to_le32((u32)(end - start + 1));
	ei->csum_type = cpu_to_le16(VTX_CSUM_CRC32C);
	ei->csum_count = cpu_to_le16((u16)nblocks);
	ei->header_bytes = cpu_to_le16(sizeof(*ei));
	memcpy(ei->tail, csums, (size_t)nblocks * sizeof(__le32));

	ret = vtxfs_add_extent_ref(fs, alloc_id, vblk, nblocks,
				   VTX_REF_FLAG_DATA,
				   (u32)vtx_cpu64(root->on_disk.subvol_id));
	if (ret) {
		vtxfs_del_item(root, path);
		goto out;
	}
	vtxfs_mark_eb_dirty(path->nodes[0]);
out:
	kfree(csums);
	vtxfs_free_path(path);
	if (put_root)
		vtxfs_root_put(root);
	return ret;
}
