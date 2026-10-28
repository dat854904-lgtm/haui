// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctree_export.c — các accessor vốn là static trong ctree.c, được export để
 * ctree_balance.c và ctree_locking.c dùng chung. Chỉ đọc hdr/eb/root; không
 * tự khoá — caller đã giữ lock đường descend.
 */
#include <linux/rcupdate.h>
#include "ctree.h"
#include "vtxfs_trace.h"

int eb_nritems_pub(const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	return vtx_cpu16(eb->hdr->nitems);
}

void eb_key_pub(const struct vtxfs_extent_buffer *eb, int slot,
		struct vtx_key *k)
{
	VTXFS_TRACE_ENTER();

	*k = eb->hdr->entries[slot].key;
}

void *eb_val_pub(const struct vtxfs_extent_buffer *eb, int slot)
{
	VTXFS_TRACE_ENTER();

	return (u8 *)eb->hdr +
	       vtx_cpu16(eb->hdr->entries[slot].slot.val_off8) * 8U;
}

u16 eb_val_len_pub(const struct vtxfs_extent_buffer *eb, int slot)
{
	VTXFS_TRACE_ENTER();

	return vtx_cpu16(eb->hdr->entries[slot].slot.val_len);
}

int eb_level_pub(const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	return eb->hdr->level;
}

int root_level_pub(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;
	int lvl = -1;

	rcu_read_lock();
	eb = rcu_dereference(root->node);
	if (eb)
		lvl = eb->hdr->level;
	rcu_read_unlock();
	return lvl;
}

struct vtxfs_extent_buffer *root_node_get_pub(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;

	rcu_read_lock();
	eb = rcu_dereference(root->node);
	if (eb && !refcount_inc_not_zero(&eb->refs))
		eb = NULL;
	rcu_read_unlock();
	return eb ? eb : ERR_PTR(-ENOENT);
}

int root_is_physical_pub(const struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	return root->tree_type == VTX_TREE_OMAP_META ||
	       root->tree_type == VTX_TREE_OMAP_VIEW;
}

int bin_search_pub(const struct vtxfs_extent_buffer *eb,
		   const struct vtx_key *key, int *slot)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h = eb->hdr;
	u32 lo = 0, hi = vtx_cpu16(h->nitems);

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2U;

		if (vtx_key_cmp(&h->entries[mid].key, key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	*slot = (int)lo;
	if (lo < vtx_cpu16(h->nitems) &&
	    vtx_key_cmp(&h->entries[lo].key, key) == 0)
		return 0;
	return 1;
}

/* Wrapper: cập nhật separator key của parent tại slot theo min-key của child. */
void fix_sep_pub(struct vtxfs_extent_buffer *parent, int slot,
		 struct vtxfs_extent_buffer *child)
{
	VTXFS_TRACE_ENTER();

	if (slot < eb_nritems_pub(parent) && eb_nritems_pub(child) > 0) {
		struct vtx_key min_k;

		eb_key_pub(child, 0, &min_k);
		parent->hdr->entries[slot].key = min_k;
		memset(parent->hdr->checksum, 0,
		       sizeof(parent->hdr->checksum));
		vtxfs_mark_eb_dirty(parent);
	}
}
