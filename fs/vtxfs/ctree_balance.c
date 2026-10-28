// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctree_balance.c — hoàn thiện đường xóa: sau vtxfs_del_item/del_ptr, nếu nút
 * vơi dưới ngưỡng thì gộp (merge) hoặc san (rebalance) với anh em, rồi co gốc.
 * Dùng core slotted đã fuzz-test: vtx_leaf_merge/rebalance, vtx_internal_merge/rebalance.
 */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "ctree.h"
#include "vtxfs_trace.h"

static u32 node_used(const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h = eb->hdr;
	u32 head = (u32)vtx_cpu16(h->free_head8) * 8U;
	u32 tail = (u32)vtx_cpu16(h->free_tail8) * 8U;

	return head + (eb->node_size - tail);
}

static int node_underfull(const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	return node_used(eb) < eb->node_size / 4U;
}

/* CoW một node con của parent tại slot; trả eb mới (đã giữ ref). */
static int cow_child(struct vtxfs_root *root, struct vtxfs_path *path,
		     struct vtxfs_extent_buffer *parent, int level, int slot,
		     struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *child = NULL, *nb = NULL;
	int ret;

	(void)path;
	(void)level;
	ret = read_child_block_pub(fs, root, parent, slot, &child);
	if (ret)
		return ret;
	ret = vtxfs_cow_block(fs, root, child, parent, slot, &nb);
	if (ret) {
		vtxfs_eb_put(child);
		return ret;
	}
	*out = nb;
	return 0;
}

/* Cập nhật separator key trong parent cho child tại slot. */
static void fix_sep(struct vtxfs_extent_buffer *parent, int slot,
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

/*
 * Gộp/san nút tại path->nodes[level] nếu vơi. Đệ quy lên cha. Gọi sau xóa.
 * Trả 0 nếu không cần làm gì hoặc đã cân bằng xong.
 */
int vtxfs_balance_after_delete(struct vtxfs_root *root,
			       struct vtxfs_path *path, int level)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *eb, *parent, *sib, *keep, *gone;
	int slot_in_parent, sib_slot, merge_into_left, ret;
	void *scratch2;

	eb = path->nodes[level];
	if (!eb || level >= (int)root_level_pub(root))
		return 0;			/* gốc không cần cân bằng */
	if (!node_underfull(eb))
		return 0;

	parent = path->nodes[level + 1];
	slot_in_parent = path->slots[level + 1];
	if (!parent || slot_in_parent >= eb_nritems_pub(parent))
		return 0;

	scratch2 = kvmalloc((size_t)fs->node_size * 2, GFP_NOFS);
	if (!scratch2)
		return -ENOMEM;

	/* Chọn anh em: ưu tiên trái. */
	if (slot_in_parent > 0) {
		sib_slot = slot_in_parent - 1;
		merge_into_left = 1;	/* gộp eb vào anh em trái */
	} else {
		sib_slot = slot_in_parent + 1;
		merge_into_left = 0;	/* gộp anh em phải vào eb */
		if (sib_slot >= eb_nritems_pub(parent)) {
			kvfree(scratch2);
			return 0;
		}
	}

	ret = cow_child(root, path, parent, level, sib_slot, &sib);
	if (ret) {
		kvfree(scratch2);
		return ret;
	}

	if (merge_into_left) {
		keep = sib;
		gone = eb;
	} else {
		keep = eb;
		gone = sib;
	}

	if (level == 0)
		ret = vtx_leaf_merge(keep->hdr, gone->hdr, fs->node_size,
				     scratch2, (size_t)fs->node_size * 2);
	else
		ret = vtx_internal_merge(keep->hdr, gone->hdr, fs->node_size,
					 scratch2, (size_t)fs->node_size * 2);

	if (ret == -ENOSPC) {
		/* Không gộp được thì san bằng giữa hai anh em. */
		struct vtxfs_extent_buffer *left_eb = merge_into_left ? sib : eb;
		struct vtxfs_extent_buffer *right_eb = merge_into_left ? eb : sib;

		if (level == 0)
			ret = vtx_leaf_rebalance(left_eb->hdr, right_eb->hdr,
					fs->node_size, scratch2,
					(size_t)fs->node_size * 2);
		else
			ret = vtx_internal_rebalance(left_eb->hdr, right_eb->hdr,
					fs->node_size, scratch2,
					(size_t)fs->node_size * 2);
		kvfree(scratch2);
		if (ret) {
			vtxfs_eb_put(sib);
			return ret;
		}
		vtxfs_mark_eb_dirty(sib);
		vtxfs_mark_eb_dirty(eb);
		fix_sep(parent, sib_slot, sib);
		fix_sep(parent, slot_in_parent, eb);
		vtxfs_eb_put(sib);
		/* cha có thể vơi sau khi chỉ san bằng */
		return vtxfs_balance_after_delete(root, path, level + 1);
	}
	kvfree(scratch2);
	if (ret) {
		vtxfs_eb_put(sib);
		return ret;
	}

	/* Gộp thành công: 'gone' rỗng, gỡ con trỏ của nó khỏi parent. */
	vtxfs_mark_eb_dirty(keep);
	fix_sep(parent, merge_into_left ? sib_slot : slot_in_parent, keep);

	ret = vtxfs_del_ptr(root, path, level + 1,
			    merge_into_left ? slot_in_parent : sib_slot);
	if (ret) {
		vtxfs_eb_put(sib);
		return ret;
	}

	/* Thu hồi block 'gone': ghi delayed-ref -1 (SFQ) ở tầng commit. */
	vtxfs_ctree_delayed_ref(fs, gone->allocation_id, gone->vblk,
				fs->node_size >> VTXFS_BLOCK_SHIFT,
				VTX_REF_FLAG_META, -1);
	vtxfs_eb_put(sib);
	/* eb (path->nodes[level]) nếu là 'gone' thì path sẽ release sau. */

	/* Nếu cha giờ vơi hoặc rỗng, cân bằng/co gốc tầng trên. */
	if (eb_nritems_pub(parent) == 0)
		return vtxfs_contract_root(root, path);
	return vtxfs_balance_after_delete(root, path, level + 1);
}

/* Co gốc khi gốc nội bộ chỉ còn đúng 1 con. */
int vtxfs_contract_root(struct vtxfs_root *root, struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *old_root, *child = NULL;
	int rlevel = root_level_pub(root);
	int ret;

	if (rlevel == 0)
		return 0;
	old_root = rcu_dereference_protected(root->node,
				lockdep_is_held(&fs->trans_lock));
	if (eb_nritems_pub(old_root) != 1)
		return 0;

	ret = cow_child(root, path, old_root, rlevel, 0, &child);
	if (ret)
		return ret;
	/* phong ROOT cho con, bỏ ROOT ở gốc cũ */
	child->hdr->flags = cpu_to_le16(
			vtx_cpu16(child->hdr->flags) | VTX_NODE_FLAG_ROOT);
	vtxfs_mark_eb_dirty(child);

	rcu_assign_pointer(root->node, child);
	vtxfs_ctree_delayed_ref(fs, old_root->allocation_id, old_root->vblk,
				fs->node_size >> VTXFS_BLOCK_SHIFT,
				VTX_REF_FLAG_META, -1);
	vtxfs_eb_put(old_root);
	return 0;
}

static int node_overfull(const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	return node_used(eb) > (eb->node_size * 3U) / 4U;
}

/*
 * Gọi sau insert_with_splits thành công (path vẫn giữ write-lock top-down).
 * Nếu lá vừa chèn gần đầy mà anh/em ruột gần rỗng thì san bằng, tránh lệch
 * do insert một phía. Chỉ làm khi kết hợp không vượt dung lượng.
 */
int vtxfs_balance_after_insert(struct vtxfs_root *root,
			       struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *eb, *parent, *sib;
	int slot_in_parent, sib_slot, left_order, ret;
	void *scratch2;

	eb = path->nodes[0];
	if (!eb || !node_overfull(eb))
		return 0;			/* chưa lệch, khỏi làm */
	parent = path->nodes[1];
	if (!parent || eb_level_pub(parent) == 0)
		return 0;
	slot_in_parent = path->slots[1];

	if (slot_in_parent > 0) {
		sib_slot = slot_in_parent - 1;
		left_order = 1;		/* sibling ở bên trái eb */
	} else if (slot_in_parent + 1 < eb_nritems_pub(parent)) {
		sib_slot = slot_in_parent + 1;
		left_order = 0;		/* sibling ở bên phải eb */
	} else {
		return 0;
	}

	/* Chỉ rebalance nếu sibling dưới ngưỡng → đang lệch thật sự. */
	ret = read_child_block_pub(fs, root, parent, sib_slot, &sib);
	if (ret)
		return ret;
	down_write(&sib->lock);
	if (!node_underfull(sib)) {
		up_write(&sib->lock);
		vtxfs_eb_put(sib);
		return 0;
	}

	scratch2 = kvmalloc((size_t)fs->node_size * 2, GFP_NOFS);
	if (!scratch2) {
		up_write(&sib->lock);
		vtxfs_eb_put(sib);
		return -ENOMEM;
	}
	if (eb_level_pub(eb) == 0)
		ret = left_order
			? vtx_leaf_rebalance(sib->hdr, eb->hdr, fs->node_size,
					     scratch2, (size_t)fs->node_size * 2)
			: vtx_leaf_rebalance(eb->hdr, sib->hdr, fs->node_size,
					     scratch2, (size_t)fs->node_size * 2);
	else
		ret = left_order
			? vtx_internal_rebalance(sib->hdr, eb->hdr,
					fs->node_size, scratch2,
					(size_t)fs->node_size * 2)
			: vtx_internal_rebalance(eb->hdr, sib->hdr,
					fs->node_size, scratch2,
					(size_t)fs->node_size * 2);
	kvfree(scratch2);
	if (ret) {
		up_write(&sib->lock);
		vtxfs_eb_put(sib);
		return ret == -ENOSPC ? 0 : ret;
	}
	vtxfs_mark_eb_dirty(eb);
	vtxfs_mark_eb_dirty(sib);
	/* cập nhật separator của cả hai phía trong cha */
	fix_sep_pub(parent, sib_slot, sib);
	fix_sep_pub(parent, slot_in_parent, eb);
	up_write(&sib->lock);
	vtxfs_eb_put(sib);
	return 0;
}
