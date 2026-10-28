// SPDX-License-Identifier: GPL-2.0-only
/*
 * omap.c — per-subvolume OMAP view (VIRTUAL_OID child resolution).
 * P1 scope: single-leaf view tree operated directly on the extent buffer
 * via the portable slotted-leaf core. Multi-level view trees are P2.
 */
#include <linux/slab.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

static struct vtxfs_extent_buffer *view_leaf(struct vtxfs_omap_view *view)
{
	VTXFS_TRACE_ENTER();

	return view->root;
}

int vtxfs_omap_view_resolve(struct vtxfs_omap_view *view, u64 child_oid,
			    u64 *child_vblk)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb = view_leaf(view);
	struct vtx_key key;
	struct vtx_omap_val val;
	const struct vtx_node_hdr *h;
	u32 lo, hi, slot;
	int ret;

	if (!eb || !child_vblk)
		return -EINVAL;
	ret = vtxfs_mk_key(&key, child_oid, VTX_TYPE_OMAP_VAL, 0);
	if (ret)
		return ret;

	down_read(&eb->lock);
	h = eb->hdr;
	if (h->level != 0) {	/* multi-level view: P2 */
		up_read(&eb->lock);
		return -EOPNOTSUPP;
	}
	lo = 0;
	hi = vtx_cpu16(h->nitems);
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2U;
		int cmp = vtx_key_cmp(&h->entries[mid].key, &key);

		if (cmp < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	slot = lo;
	if (slot >= vtx_cpu16(h->nitems) ||
	    vtx_key_cmp(&h->entries[slot].key, &key) != 0) {
		up_read(&eb->lock);
		return -ENOENT;
	}
	{
		const struct vtx_slot *sl = &h->entries[slot].slot;
		u16 len = vtx_cpu16(sl->val_len);

		if (len != sizeof(val)) {
			up_read(&eb->lock);
			return -EUCLEAN;
		}
		memcpy(&val, (u8 *)h + vtx_cpu16(sl->val_off8) * 8U, len);
	}
	up_read(&eb->lock);

	if (vtx_cpu32(val.flags) & VTX_OMAP_VAL_TOMBSTONE)
		return -ENOENT;
	*child_vblk = vtx_cpu64(val.vblk);
	return VTXFS_VALID_VBLK(*child_vblk) ? 0 : -EUCLEAN;
}

static int view_upsert(struct vtxfs_omap_view *view, u64 child_oid,
		       u64 child_vblk, bool tombstone)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb = view_leaf(view);
	struct vtx_key key;
	struct vtx_omap_val val;
	void *scratch;
	int ret;

	if (!eb)
		return -EINVAL;
	ret = vtxfs_mk_key(&key, child_oid, VTX_TYPE_OMAP_VAL, 0);
	if (ret)
		return ret;
	memset(&val, 0, sizeof(val));
	val.vblk = vtx_le64(child_vblk);
	val.node_size = vtx_le32(eb->fs->node_size);
	val.flags = vtx_le32(tombstone ? VTX_OMAP_VAL_TOMBSTONE : 0);

	scratch = kvmalloc(eb->node_size, GFP_NOFS);
	if (!scratch)
		return -ENOMEM;
	down_write(&eb->lock);
	ret = vtx_leaf_replace(eb->hdr, eb->node_size, &key, &val,
			       sizeof(val), scratch, eb->node_size);
	if (ret == -ENOENT)
		ret = vtx_leaf_insert(eb->hdr, eb->node_size, &key, &val,
				      sizeof(val), scratch, eb->node_size);
	up_write(&eb->lock);
	kvfree(scratch);
	if (!ret)
		vtxfs_mark_eb_dirty(eb);
	return ret;
}

int vtxfs_omap_view_set(struct vtxfs_omap_view *view, u64 child_oid,
			u64 child_vblk)
{
	VTXFS_TRACE_ENTER();

	return view_upsert(view, child_oid, child_vblk, false);
}

int vtxfs_omap_view_invalidate(struct vtxfs_omap_view *view, u64 child_oid)
{
	VTXFS_TRACE_ENTER();

	return view_upsert(view, child_oid, 0, true);
}

struct vtxfs_omap_view *vtxfs_omap_view_alloc(struct vtxfs_fs_info *fs,
					      struct vtxfs_extent_buffer *root,
					      u64 view_xid)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_omap_view *view;

	view = kzalloc(sizeof(*view), GFP_KERNEL);
	if (!view)
		return ERR_PTR(-ENOMEM);
	view->fs = fs;
	view->root = root;
	if (root) {
		vtxfs_eb_get(root);
		view->root_vblk = root->vblk;
	}
	view->view_xid = view_xid;
	xa_init(&view->cache);
	init_rwsem(&view->lock);
	refcount_set(&view->refs, 1);
	return view;
}

void vtxfs_omap_view_put(struct vtxfs_omap_view *view)
{
	VTXFS_TRACE_ENTER();

	if (!view || !refcount_dec_and_test(&view->refs))
		return;
	if (view->root)
		vtxfs_eb_put(view->root);
	xa_destroy(&view->cache);
	kfree(view);
}

/* Công bố child_oid -> new_vblk vào cây OMAP có thẩm quyền. Cây OMAP là
 * PHYSICAL_VBLK nên thao tác này không cần phân giải OMAP (không đệ quy). */
int vtxfs_omap_publish(struct vtxfs_root *omap_root, u64 child_oid, u64 vblk)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key;
	struct vtx_omap_val val;
	void *w;
	int ret;

	if (!omap_root)
		return -EINVAL;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_mk_key(&key, child_oid, VTX_TYPE_OMAP_VAL, 0);
	if (ret)
		goto out;
	memset(&val, 0, sizeof(val));
	val.vblk = cpu_to_le64(vblk);
	val.node_size = cpu_to_le32(omap_root->fs->node_size);

	ret = vtxfs_search_slot(omap_root, path, &key, sizeof(val), 1);
	if (ret == 1) {
		ret = vtxfs_insert_item(omap_root, path, &key, sizeof(val), &w);
		if (!ret && w)
			memcpy(w, &val, sizeof(val));
	} else if (ret == 0) {
		/* đã tồn tại: thay thế tại chỗ */
		memcpy(eb_val_pub(path->nodes[0], path->slots[0]), &val,
		       sizeof(val));
		vtxfs_mark_eb_dirty(path->nodes[0]);
		ret = 0;
	}
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_omap_resolve_vblk(struct vtxfs_root *omap_root, u64 child_oid,
			    u64 *child_vblk)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key;
	struct vtx_omap_val val;
	int ret;

	if (!omap_root || !child_vblk)
		return -EINVAL;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_mk_key(&key, child_oid, VTX_TYPE_OMAP_VAL, 0);
	if (ret) {
		vtxfs_free_path(path);
		return ret;
	}
	ret = vtxfs_search_slot(omap_root, path, &key, 0, 0);
	if (ret == 0) {
		struct vtxfs_extent_buffer *leaf = path->nodes[0];
		int slot = path->slots[0];

		if (eb_val_len_pub(leaf, slot) != sizeof(val)) {
			ret = -EUCLEAN;
		} else {
			memcpy(&val, eb_val_pub(leaf, slot), sizeof(val));
			if (vtx_cpu32(val.flags) & VTX_OMAP_VAL_TOMBSTONE) {
				ret = -ENOENT;
			} else {
				*child_vblk = vtx_cpu64(val.vblk);
				ret = VTXFS_VALID_VBLK(*child_vblk) ? 0 : -EUCLEAN;
			}
		}
	} else if (ret == 1) {
		ret = -ENOENT;
	}
	vtxfs_free_path(path);
	return ret;
}

