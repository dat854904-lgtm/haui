// SPDX-License-Identifier: GPL-2.0-only
/*
 * sfq.c — promote SFQ entries (retired_xid <= safe_xid) thành block tái sử
 * dụng: xác nhận KHÔNG còn tham chiếu trong REF, xóa entry SFQ, xóa bit VAG.
 * Gọi tại commit sau khi drain delayed-ref.
 */
#include <linux/slab.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_trace.h"

int vtxfs_sfq_promote(struct vtxfs_fs_info *fs, u64 safe_xid,
		      u32 *promoted_out)
{
	VTXFS_TRACE_ENTER();

	if (fs->shadow_only)
		return -EROFS;
	struct vtxfs_root *sfq = fs->sfq_root;
	struct vtxfs_root *ref = fs->ref_root;
	struct vtxfs_path *path, *rpath;
	struct vtx_key cur, found, rkey;
	u32 promoted = 0;
	int ret;

	if (fs->sb && sb_rdonly(fs->sb))
		return -EROFS;
	if (!sfq || !ref)
		return 0;
	path = vtxfs_alloc_path();
	rpath = vtxfs_alloc_path();
	if (!path || !rpath) {
		ret = -ENOMEM;
		goto out;
	}
	ret = vtxfs_mk_key(&cur, 1, VTX_TYPE_SFQ, 0);
	if (ret)
		goto out;

	for (;;) {
		struct vtx_sfq_entry sfq_e;
		u64 sv, retired;
		u32 blocks;

		ret = vtxfs_search_slot(sfq, path, &cur, 0, 1);
		if (ret < 0)
			goto out;
		if (ret > 0 || path->slots[0] >= eb_nritems(path->nodes[0])) {
			ret = 0;
			break;
		}
		eb_key(path->nodes[0], path->slots[0], &found);
		if (vtxfs_key_type(&found) != VTX_TYPE_SFQ)
			break;
		if (eb_val_len(path->nodes[0], path->slots[0])
		    != sizeof(sfq_e)) {
			ret = -EUCLEAN;
			goto out;
		}
		memcpy(&sfq_e, eb_val(path->nodes[0], path->slots[0]),
		       sizeof(sfq_e));
		sv = vtx_cpu64(sfq_e.start_vblk);
		blocks = (__u32)vtx_cpu64(sfq_e.block_count);
		retired = vtx_cpu64(sfq_e.retired_xid);

		if (retired > safe_xid)
			goto advance;

		/* Xác nhận không còn ALLOC_BY_VBLK (refcount đã về 0). */
		ret = vtxfs_mk_key(&rkey, sv, VTX_TYPE_ALLOC_BY_VBLK, 0);
		if (ret)
			goto out;
		vtxfs_release_path(rpath);
		ret = vtxfs_search_slot(ref, rpath, &rkey, 0, 0);
		if (ret == 0) {
			ret = -EUCLEAN;	/* vẫn còn tham chiếu */
			goto out;
		}
		if (ret < 0)
			goto out;
		ret = 0;

		/* Xóa entry SFQ rồi xóa bit VAG. */
		ret = vtxfs_del_item(sfq, path);
		if (ret)
			goto out;
		if (fs->alloc_ops && fs->alloc_ops->free_blocks) {
			ret = fs->alloc_ops->free_blocks(fs, sv, blocks);
			if (ret)
				goto out;
		}
		promoted++;
		vtxfs_release_path(path);
		cur = found;
		cur.offset = cpu_to_le64(vtxfs_key_off(&found) + 1);
		continue;
advance:
		cur = found;
		cur.offset = cpu_to_le64(vtxfs_key_off(&found) + 1);
		vtxfs_release_path(path);
	}
	ret = 0;
	if (promoted_out)
		*promoted_out = promoted;
out:
	vtxfs_free_path(path);
	vtxfs_free_path(rpath);
	return ret;
}

/* Flush mọi delalloc còn lại thành ordered extent (gọi trước wait_ordered). */
int vtxfs_flush_all_delalloc(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *root;
	unsigned long subvol_id, ino_id;
	int ret = 0;

	xa_for_each(&fs->roots, subvol_id, root) {
		struct vtxfs_inode *vinode;

		xa_for_each(&root->inodes, ino_id, vinode) {
			struct inode *inode = &vinode->vfs_inode;
			u64 start = 0, end = (u64)-1;

			if (!vinode->io_tree.dirty_bytes)
				continue;
			ret = vtxfs_run_delalloc_range(inode, start, end);
			if (ret)
				return ret;
		}
	}
	return 0;
}
