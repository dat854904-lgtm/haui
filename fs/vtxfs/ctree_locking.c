// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctree_locking.c — descending lock coupling trên eb->lock (rw_semaphore).
 * Thay thế trans_lock toàn cục cho thao tác cấu trúc cây:
 *   - READ  : read-lock từng node, nhả cha sau khi khoá con (crabbing).
 *   - WRITE : write-lock top-down giữ suốt đường descend để split/merge
 *             lan lên dùng chính các lock đã giữ (không cần lock lại).
 */
#include <linux/slab.h>
#include "ctree.h"
#include "vtxfs_trace.h"

static void path_unlock_node(struct vtxfs_path *path, int level)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb = path->nodes[level];

	if (!eb)
		return;
	if (path->locks[level] == VTXFS_LOCK_READ)
		up_read(&eb->lock);
	else if (path->locks[level] == VTXFS_LOCK_WRITE)
		up_write(&eb->lock);
	path->locks[level] = VTXFS_LOCK_NONE;
}

void vtxfs_release_path_locked(struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	int i;

	if (!path)
		return;
	for (i = VTXFS_MAX_LEVEL - 1; i >= 0; i--) {
		path_unlock_node(path, i);
		if (path->nodes[i]) {
			vtxfs_eb_put(path->nodes[i]);
			path->nodes[i] = NULL;
		}
		path->slots[i] = 0;
	}
}

/* CoW node đang giữ write-lock; trả về node mới đã write-lock (hoặc chính nó
 * nếu đã độc quyền). Thả lock+ref của node cũ khi thay thế. */
static struct vtxfs_extent_buffer *
cow_node_locked(struct vtxfs_root *root, struct vtxfs_extent_buffer *eb,
		struct vtxfs_extent_buffer *parent, int parent_slot)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *nb;
	struct vtx_key_ptr_val ptr;
	int ret;

	lockdep_assert_held_write(&eb->lock);
	if (!vtxfs_block_can_be_shared(root, eb))
		return eb;	/* đã độc quyền, giữ nguyên lock */

	ret = alloc_tree_block_pub(root, (u8)eb_level_pub(eb), eb->oid, 0, &nb);
	if (ret)
		return ERR_PTR(ret);
	down_write(&nb->lock);		/* block mới: khoá luôn */
	memcpy(nb->hdr, eb->hdr, fs->node_size);
	nb->hdr->self_vblk = cpu_to_le64(nb->vblk);
	nb->hdr->xid = cpu_to_le64(fs->current_xid);
	memset(nb->hdr->checksum, 0, sizeof(nb->hdr->checksum));
	nb->oid = eb->oid;

	if (!parent) {
		rcu_assign_pointer(root->node, nb);
	} else {
		ptr.child_oid = cpu_to_le64(nb->oid);
		ptr.hint_vblk = cpu_to_le64(nb->vblk);
		ptr.hint_xid  = cpu_to_le64(fs->current_xid);
		ret = vtxfs_set_node_ptr(parent, parent_slot, NULL, &ptr);
		if (ret) {
			up_write(&nb->lock);
			vtxfs_eb_put(nb);
			return ERR_PTR(ret);
		}
		if (!root_is_physical_pub(root) && root->resolve_root)
			vtxfs_omap_publish(root->resolve_root, nb->oid,
					   nb->vblk);
	}
	vtxfs_mark_eb_dirty(nb);
	up_write(&eb->lock);
	vtxfs_eb_put(eb);
	return nb;
}

/*
 * Search với lock-coupling. Trả path đã khoá theo path->locks[].
 * cow=1 → giữ write-lock suốt đường descend (cho insert/delete/split).
 * cow=0 → read-lock, nhả dần khi descend (trừ khi keep_locks).
 */
int vtxfs_search_slot_locked(struct vtxfs_root *root,
			     struct vtxfs_path *path,
			     const struct vtx_key *key, int ins_len, int cow)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *b, *child, *nb;
	int level, slot, parent_slot, ret;

	memset(path->nodes, 0, sizeof(path->nodes));
	memset(path->slots, 0, sizeof(path->slots));
	memset(path->locks, 0, sizeof(path->locks));

	b = root_node_get_pub(root);
	if (IS_ERR(b))
		return PTR_ERR(b);
	level = eb_level_pub(b);
	if (cow)
		down_write(&b->lock);
	else
		down_read(&b->lock);
	path->nodes[level] = b;
	path->locks[level] = cow ? VTXFS_LOCK_WRITE : VTXFS_LOCK_READ;

	if (cow) {
		nb = cow_node_locked(root, b, NULL, 0);
		if (IS_ERR(nb)) {
			vtxfs_release_path_locked(path);
			return PTR_ERR(nb);
		}
		b = nb;
		path->nodes[level] = b;	/* cow_node_locked đã giữ lock+ref */
	}

	while (level > 0) {
		ret = bin_search_pub(b, key, &slot);
		path->slots[level] = slot;
		if (ret > 0 && slot > 0)
			slot--;
		parent_slot = slot;
		ret = read_child_block_pub(fs, root, b, slot, &child);
		if (ret) {
			vtxfs_release_path_locked(path);
			return ret;
		}
		if (cow) {
			down_write(&child->lock);
			nb = cow_node_locked(root, child, b, parent_slot);
			if (IS_ERR(nb)) {
				vtxfs_release_path_locked(path);
				return PTR_ERR(nb);
			}
			child = nb;
			path->locks[level - 1] = VTXFS_LOCK_WRITE;
		} else {
			down_read(&child->lock);
			path->locks[level - 1] = VTXFS_LOCK_READ;
			/* crabbing: nhả cha sau khi đã khoá con */
			if (!path->keep_locks) {
				path_unlock_node(path, level);
				vtxfs_eb_put(path->nodes[level]);
				path->nodes[level] = NULL;
			}
		}
		path->nodes[level - 1] = child;
		b = child;
		level--;
	}

	ret = bin_search_pub(b, key, &slot);
	path->slots[0] = slot;
	return ret;
}
