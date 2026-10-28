// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctree.c — path-based CoW B+Tree trên extent buffer kernel (bản sửa lõi).
 *
 * Mọi thao tác đột biến chạy dưới fs->trans_lock. Đọc node con:
 *   PHYSICAL_VBLK -> dùng hint_vblk/hint_xid (cây OMAP)
 *   VIRTUAL_OID   -> phân giải qua OMAP view, KHÔNG dùng hint
 * Tách node dùng core slotted đã fuzz-test; lớp này thêm CoW, sửa con trỏ
 * cha, tăng/co gốc và quản lý refcount extent buffer.
 */
#include <linux/slab.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include "vtxfs_drv.h"
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_csum.h"
#include "vtxfs_leaf.h"
#include "vtxfs_internal.h"
#include "vtxfs_validate.h"
#include "vtxfs_trace.h"



static inline int root_is_physical(const struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	return root->tree_type == VTX_TREE_OMAP_META ||
	       root->tree_type == VTX_TREE_OMAP_VIEW;
}

static int bin_search(const struct vtxfs_extent_buffer *eb,
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

/* ---------- đọc block cây qua VAG + bio ---------- */
static int ctree_read_blocks(struct vtxfs_fs_info *fs, u64 pblk, u32 count,
			     void *buf)
{
	VTXFS_TRACE_ENTER();

	u32 i;
	int ret;

	for (i = 0; i < count; i++) {
		struct page *page = vtx_buf_to_page((u8 *)buf +
					(size_t)i * VTXFS_BLOCK_SIZE);
		struct bio *bio = bio_alloc(fs->bdev, 1, REQ_OP_READ,
					    GFP_NOFS);

		bio->bi_iter.bi_sector =
			(pblk + i) << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
		if (bio_add_page(bio, page, VTXFS_BLOCK_SIZE,
				 offset_in_page((u8 *)buf +
					(size_t)i * VTXFS_BLOCK_SIZE))
		    != VTXFS_BLOCK_SIZE) {
			bio_put(bio);
			return -EIO;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			return ret;
	}
	return 0;
}

static int ctree_read_tree_block(struct vtxfs_fs_info *fs, u64 oid, u64 vblk,
				 u8 level, u64 owner, u8 tree_type,
				 struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb, *exist;
	void *tmp;
	u64 pblk;
	int ret;

	eb = vtxfs_find_eb(fs, vblk);
	if (eb) {
		if (eb->oid != oid || eb_level(eb) != level) {
			vtxfs_eb_put(eb);
			return -EUCLEAN;
		}
		*out = eb;
		return 0;
	}

	ret = vtxfs_vag_vblk_to_pblk(fs->vag, vblk, &pblk);
	if (ret)
		return ret;
	tmp = kvzalloc(fs->node_size, GFP_NOFS);
	if (!tmp)
		return -ENOMEM;
	ret = ctree_read_blocks(fs, pblk, fs->node_size >> VTXFS_BLOCK_SHIFT,
				tmp);
	if (!ret && (!vtx_node_slots_valid(tmp, fs->node_size) ||
		     !vtx_node_checksum_verify(tmp, fs->node_size,
					       fs->metadata_uuid)))
		ret = -EUCLEAN;
	if (!ret) {
		const struct vtx_node_hdr *h = tmp;

		if (vtx_cpu64(h->oid) != oid || h->level != level ||
		    h->tree_type != tree_type ||
		    vtx_cpu64(h->owner_id) != owner ||
		    vtx_cpu64(h->xid) == 0 ||
		    vtx_cpu64(h->xid) > fs->committed_xid)
			ret = -EUCLEAN;
	}
	if (ret) {
		kvfree(tmp);
		return ret;
	}
	eb = vtxfs_eb_alloc(fs, vblk, fs->node_size);
	if (IS_ERR(eb)) {
		kvfree(tmp);
		return PTR_ERR(eb);
	}
	memcpy(eb->hdr, tmp, fs->node_size);
	kvfree(tmp);
	eb->oid = oid;
	eb->birth_xid = vtx_cpu64(eb->hdr->xid);
	set_bit(VTXFS_EB_UPTODATE, &eb->flags);
	set_bit(VTXFS_EB_VERIFIED, &eb->flags);

	exist = xa_cmpxchg(&fs->eb_cache, vblk, NULL, eb, GFP_NOFS);
	if (xa_is_err(exist)) {
		ret = xa_err(exist);
		vtxfs_eb_put(eb);
		return ret;
	}
	if (exist) {
		vtxfs_eb_put(eb);
		eb = exist;
	}
	*out = eb;
	return 0;
}

static int read_child_block(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
			    struct vtxfs_extent_buffer *parent, int slot,
			    struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h = parent->hdr;
	const struct vtx_node_entry *e = &h->entries[slot];
	struct vtx_key_ptr_val ptr;
	u64 child_oid, child_vblk = 0;
	int ret;

	if (vtx_cpu16(e->slot.val_len) != sizeof(ptr))
		return -EUCLEAN;
	memcpy(&ptr, (const u8 *)h + vtx_cpu16(e->slot.val_off8) * 8U,
	       sizeof(ptr));
	child_oid = vtx_cpu64(ptr.child_oid);
	if (!vtx_oid_valid(child_oid))
		return -EUCLEAN;

	if (vtx_cpu16(h->flags) & VTX_NODE_FLAG_PHYSICAL_VBLK) {
		child_vblk = vtx_cpu64(ptr.hint_vblk);
		if (!VTXFS_VALID_VBLK(child_vblk) || !vtx_cpu64(ptr.hint_xid))
			return -EUCLEAN;
	} else {
		if (root->view) {
			ret = vtxfs_omap_view_resolve(root->view, child_oid,
						      &child_vblk);
			if (ret)
				return ret;
		} else if (root->resolve_root) {
			ret = vtxfs_omap_resolve_vblk(root->resolve_root, child_oid,
						      &child_vblk);
			if (ret)
				return ret;
		} else {
			return -EOPNOTSUPP;
		}
	}
	return ctree_read_tree_block(fs, child_oid, child_vblk,
				     (u8)(eb_level(parent) - 1),
				     vtx_cpu64(h->owner_id), h->tree_type,
				     out);
}

int read_child_block_pub(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
			 struct vtxfs_extent_buffer *parent, int slot,
			 struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	return read_child_block(fs, root, parent, slot, out);
}

/* ---------- path ---------- */
struct vtxfs_path *vtxfs_alloc_path(void)
{
	VTXFS_TRACE_ENTER();

	return kzalloc(sizeof(struct vtxfs_path), GFP_NOFS);
}

void vtxfs_release_path(struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	int i;

	if (!path)
		return;
	for (i = 0; i < VTXFS_MAX_LEVEL; i++) {
		if (path->nodes[i]) {
			vtxfs_eb_put(path->nodes[i]);
			path->nodes[i] = NULL;
		}
		path->slots[i] = 0;
	}
}

void vtxfs_free_path(struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	vtxfs_release_path(path);
	kfree(path);
}

static struct vtxfs_extent_buffer *root_node_get(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;

	rcu_read_lock();
	eb = rcu_dereference(root->node);
	if (eb)
		vtxfs_eb_get(eb);
	rcu_read_unlock();
	return eb ? eb : ERR_PTR(-ENOENT);
}

/* ---------- cấp phát block cây mới ---------- */
static int alloc_tree_block(struct vtxfs_root *root, u8 level, u64 oid,
			    int is_root, struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *eb;
	u64 vblk = 0;
	u16 flags;
	int ret;

	if (!fs->alloc_ops || !fs->alloc_ops->alloc_blocks)
		return -EOPNOTSUPP;
	ret = fs->alloc_ops->alloc_blocks(fs,
			fs->node_size >> VTXFS_BLOCK_SHIFT, &vblk);
	if (ret)
		return ret;
	eb = vtxfs_eb_alloc(fs, vblk, fs->node_size);
	if (IS_ERR(eb)) {
		fs->alloc_ops->free_blocks(fs, vblk,
				fs->node_size >> VTXFS_BLOCK_SHIFT);
		return PTR_ERR(eb);
	}
	if (level == 0) {
		flags = VTX_NODE_FLAG_LEAF;
		if (is_root)
			flags |= VTX_NODE_FLAG_ROOT;
		ret = vtx_leaf_init(eb->hdr, fs->node_size, oid,
				    fs->current_xid, vblk, root->tree_type,
				    flags);
	} else {
		flags = root_is_physical(root) ? VTX_NODE_FLAG_PHYSICAL_VBLK
					       : VTX_NODE_FLAG_VIRTUAL_OID;
		if (is_root)
			flags |= VTX_NODE_FLAG_ROOT;
		ret = vtx_internal_init(eb->hdr, fs->node_size, oid,
					fs->current_xid, vblk, root->tree_type,
					flags, level);
	}
	if (ret) {
		vtxfs_eb_put(eb);
		fs->alloc_ops->free_blocks(fs, vblk,
				fs->node_size >> VTXFS_BLOCK_SHIFT);
		return ret;
	}
	eb->hdr->owner_id = cpu_to_le64(root->subvol_id);
	eb->oid = oid;
	eb->allocation_id = atomic64_inc_return(&root->fs->next_object_id);
	vtxfs_ctree_delayed_ref(root->fs, eb->allocation_id,
				vblk, fs->node_size >> VTXFS_BLOCK_SHIFT,
				VTX_REF_FLAG_META, 1);
	/* đưa vào cache để read_child tìm được ngay (kể cả chưa ghi đĩa) */
	{
		void *old = xa_cmpxchg(&fs->eb_cache, vblk, NULL, eb,
				       GFP_NOFS);

		if (xa_is_err(old)) {
			ret = xa_err(old);
			vtxfs_eb_put(eb);
			return ret;
		}
		if (old) {
			vtxfs_eb_put(eb);
			fs->alloc_ops->free_blocks(fs, vblk,
					fs->node_size >> VTXFS_BLOCK_SHIFT);
			return -EEXIST;
		}
	}
	vtxfs_mark_eb_dirty(eb);
	*out = eb;
	return 0;
}

int alloc_tree_block_for_test(struct vtxfs_root *root,
			      struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	return alloc_tree_block(root, 0, 1, 1, out);
}

int alloc_tree_block_pub(struct vtxfs_root *root, u8 level, u64 oid, int is_root,
			 struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	return alloc_tree_block(root, level, oid, is_root, out);
}

/* ---------- CoW ---------- */
bool vtxfs_block_can_be_shared(struct vtxfs_root *root,
			       struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	if (eb == root->committed_node)
		return true;
	if (root->subvol_id &&
	    vtx_cpu64(eb->hdr->owner_id) != root->subvol_id)
		return true;
	if (test_bit(VTXFS_ROOT_FORCE_COW, &root->state))
		return true;
	return false;
}

int vtxfs_set_node_ptr(struct vtxfs_extent_buffer *parent, int slot,
		       const struct vtx_key *sep_key,
		       const struct vtx_key_ptr_val *ptr)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = parent->fs;
	struct vtx_key old_key;
	void *scratch;
	int ret;

	(void)sep_key;
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	if (!scratch)
		return -ENOMEM;
	eb_key(parent, slot, &old_key);
	down_write(&parent->lock);
	ret = vtx_internal_replace(parent->hdr, fs->node_size, &old_key,
				   ptr, scratch, fs->node_size);
	up_write(&parent->lock);
	kvfree(scratch);
	if (!ret)
		vtxfs_mark_eb_dirty(parent);
	return ret;
}

int vtxfs_cow_block(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
		    struct vtxfs_extent_buffer *eb,
		    struct vtxfs_extent_buffer *parent, int parent_slot,
		    struct vtxfs_extent_buffer **cow_ret)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *cow;
	struct vtx_key_ptr_val ptr;
	int ret;

	lockdep_assert_held(&fs->trans_lock);
	if (!vtxfs_block_can_be_shared(root, eb)) {
		*cow_ret = eb;
		return 0;
	}
	ret = alloc_tree_block(root, (u8)eb_level(eb), eb->oid, 0, &cow);
	if (ret)
		return ret;

	down_read(&eb->lock);
	memcpy(cow->hdr, eb->hdr, fs->node_size);
	up_read(&eb->lock);
	cow->hdr->self_vblk = cpu_to_le64(cow->vblk);
	cow->hdr->xid = cpu_to_le64(fs->current_xid);
	memset(cow->hdr->checksum, 0, sizeof(cow->hdr->checksum));
	cow->oid = eb->oid;

	if (!parent) {
		rcu_assign_pointer(root->node, cow);
		/* Drop old root->node reference on eb */
		vtxfs_eb_put(eb);
		/* cow needs an extra ref for caller (*cow_ret), root->node holds allocation ref */
		vtxfs_eb_get(cow);
	} else {
		ptr.child_oid = cpu_to_le64(cow->oid);
		ptr.hint_vblk = cpu_to_le64(cow->vblk);
		ptr.hint_xid = cpu_to_le64(fs->current_xid);
		ret = vtxfs_set_node_ptr(parent, parent_slot, NULL, &ptr);
		if (ret) {
			vtxfs_eb_put(cow);
			return ret;
		}
		/* PROPAGATION: công bố vblk mới vào cây OMAP có thẩm quyền.
		 * Cây PHYSICAL_VBLK không cần; cây VIRTUAL_OID bắt buộc. */
		if (!root_is_physical(root)) {
			if (root->resolve_root) {
				ret = vtxfs_omap_publish(root->resolve_root,
							 cow->oid, cow->vblk);
				if (ret) {
					vtxfs_eb_put(cow);
					return ret;
				}
			}
			if (root->view)
				vtxfs_omap_view_set(root->view, cow->oid, cow->vblk);
		}
	}
	vtxfs_mark_eb_dirty(cow);
	vtxfs_eb_put(eb);	/* chuyển giao ref của caller sang cow */
	*cow_ret = cow;
	return 0;
}

int vtxfs_copy_root(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
		    struct vtxfs_extent_buffer **new_root_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *src, *dst;
	int ret;

	lockdep_assert_held(&fs->trans_lock);
	src = root_node_get(root);
	if (IS_ERR(src))
		return PTR_ERR(src);
	ret = alloc_tree_block(root, (u8)eb_level(src), src->oid, 1, &dst);
	if (ret) {
		vtxfs_eb_put(src);
		return ret;
	}
	down_read(&src->lock);
	memcpy(dst->hdr, src->hdr, fs->node_size);
	up_read(&src->lock);
	dst->hdr->self_vblk = cpu_to_le64(dst->vblk);
	dst->hdr->xid = cpu_to_le64(fs->current_xid);
	memset(dst->hdr->checksum, 0, sizeof(dst->hdr->checksum));
	dst->oid = src->oid;
	vtxfs_mark_eb_dirty(dst);
	rcu_assign_pointer(root->node, dst);
	/* Drop old root->node reference on src */
	vtxfs_eb_put(src);
	/* Drop caller temporary reference from root_node_get */
	vtxfs_eb_put(src);
	if (new_root_out) {
		vtxfs_eb_get(dst);
		*new_root_out = dst;
	}
	return 0;
}

/* ---------- search ---------- */
int vtxfs_search_slot(struct vtxfs_root *root, struct vtxfs_path *path,
		      const struct vtx_key *key, int ins_len, int cow)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *b, *parent = NULL;
	int level, slot, parent_slot = 0, ret;

	if (fs->shadow_only && (cow || ins_len > 0))
		return -EROFS;
	lockdep_assert_held(&fs->trans_lock);
	b = root_node_get(root);
	if (IS_ERR(b))
		return PTR_ERR(b);

	for (;;) {
		level = eb_level(b);
		if (cow && ins_len > 0) {
			struct vtxfs_extent_buffer *nb = NULL;

			ret = vtxfs_cow_block(fs, root, b, parent, parent_slot,
					      &nb);
			if (ret) {
				vtxfs_eb_put(b);
				vtxfs_release_path(path);
				return ret;
			}
			b = nb;
		}
		path->nodes[level] = b;
		ret = bin_search(b, key, &slot);
		if (level == 0) {
			path->slots[0] = slot;
			return ret;
		}
		if (ret > 0 && slot > 0)
			slot--;
		path->slots[level] = slot;
		parent = b;
		parent_slot = slot;
		ret = read_child_block(fs, root, b, slot, &b);
		if (ret) {
			vtxfs_release_path(path);
			return ret;
		}
	}
}

int vtxfs_search_old_slot(struct vtxfs_root *root, struct vtxfs_path *path,
			  const struct vtx_key *key, int ins_len)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *b = root->committed_node;
	int slot, ret;

	if (!b || ins_len > 0)
		return -EINVAL;
	vtxfs_eb_get(b);
	for (;;) {
		int level = eb_level(b);

		path->nodes[level] = b;
		ret = bin_search(b, key, &slot);
		if (level == 0) {
			path->slots[0] = slot;
			return ret;
		}
		if (ret > 0 && slot > 0)
			slot--;
		path->slots[level] = slot;
		ret = read_child_block(root->fs, root, b, slot, &b);
		if (ret) {
			vtxfs_release_path(path);
			return ret;
		}
	}
}

int vtxfs_find_item(struct vtxfs_root *root, const struct vtx_key *key,
		    struct vtxfs_path **path_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	int ret;

	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot(root, path, key, 0, 0);
	if (ret != 0) {
		vtxfs_free_path(path);
		return ret > 0 ? -ENOENT : ret;
	}
	*path_out = path;
	return 0;
}

int vtxfs_next_key(struct vtxfs_root *root, struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	int level = 0, ret;

	for (;;) {
		struct vtxfs_extent_buffer *eb = path->nodes[level];

		if (!eb)
			return -ENOENT;
		if (path->slots[level] + 1 < eb_nritems(eb)) {
			path->slots[level]++;
			while (level > 0) {
				struct vtxfs_extent_buffer *child;

				ret = read_child_block(fs, root, eb,
						path->slots[level], &child);
				if (ret)
					return ret;
				vtxfs_eb_put(eb);
				level--;
				path->nodes[level] = child;
				path->slots[level] = 0;
				eb = child;
			}
			return 0;
		}
		if (level >= VTXFS_MAX_LEVEL - 1 || !path->nodes[level + 1])
			return 1;
		vtxfs_eb_put(eb);
		path->nodes[level] = NULL;
		level++;
	}
}

int vtxfs_previous_item(struct vtxfs_root *root, struct vtxfs_path *path,
			u64 min_oid, int type_filter)
{
	VTXFS_TRACE_ENTER();

	int ret;

	for (;;) {
		struct vtx_key k;

		ret = vtxfs_next_key(root, path);
		if (ret)
			return ret;
		eb_key(path->nodes[0], path->slots[0], &k);
		if (min_oid && vtxfs_key_oid(&k) < min_oid)
			return 1;
		if (!type_filter || vtxfs_key_type(&k) == type_filter)
			return 0;
	}
}

int vtxfs_previous_extent_item(struct vtxfs_root *root,
			       struct vtxfs_path *path, u64 min_objectid)
{
	VTXFS_TRACE_ENTER();

	return vtxfs_previous_item(root, path, min_objectid, VTX_TYPE_EXTENT);
}

int vtxfs_search_slot_for_read(struct vtxfs_root *root,
			       struct vtxfs_path *path,
			       const struct vtx_key *find_higher,
			       int return_any)
{
	VTXFS_TRACE_ENTER();

	struct vtx_key key = *find_higher;
	int ret;

	vtxfs_release_path(path);
	ret = vtxfs_search_slot(root, path, &key, 0, 0);
	if (ret < 0)
		return ret;
	if (ret == 0)
		return 0;
	if (path->slots[0] < eb_nritems(path->nodes[0]))
		return 0;
	if (!return_any)
		return 1;
	return vtxfs_next_key(root, path);
}

int vtxfs_search_forward(struct vtxfs_root *root, struct vtxfs_path *path,
			 u64 *min_oid, u8 *min_type, u64 *min_offset)
{
	VTXFS_TRACE_ENTER();

	struct vtx_key key, found;
	int ret;

	memset(&key, 0, sizeof(key));
	ret = vtxfs_mk_key(&key, min_oid ? *min_oid : 1,
			   min_type ? *min_type : 0,
			   min_offset ? *min_offset : 0);
	if (ret)
		return ret;
	vtxfs_release_path(path);
	ret = vtxfs_search_slot(root, path, &key, 0, 0);
	if (ret < 0)
		return ret;
	if (ret > 0 && path->slots[0] >= eb_nritems(path->nodes[0])) {
		ret = vtxfs_next_key(root, path);
		if (ret)
			return ret;
	}
	eb_key(path->nodes[0], path->slots[0], &found);
	if (min_oid)
		*min_oid = vtxfs_key_oid(&found);
	if (min_type)
		*min_type = vtxfs_key_type(&found);
	if (min_offset)
		*min_offset = vtxfs_key_off(&found);
	return 0;
}

/* ---------- chèn con trỏ lên cha, tách đệ quy, tăng gốc ---------- */
static void fill_ptr(struct vtxfs_fs_info *fs, struct vtx_key_ptr_val *ptr,
		     struct vtxfs_extent_buffer *child)
{
	VTXFS_TRACE_ENTER();

	ptr->child_oid = cpu_to_le64(child->oid);
	ptr->hint_vblk = cpu_to_le64(child->vblk);
	ptr->hint_xid = cpu_to_le64(fs->current_xid);
}

static int grow_root(struct vtxfs_root *root, struct vtxfs_path *path,
		     int rlevel, struct vtxfs_extent_buffer *right)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *old_root = path->nodes[rlevel];
	struct vtxfs_extent_buffer *new_root;
	struct vtx_key left_sep, right_sep;
	struct vtx_key_ptr_val lptr, rptr;
	void *scratch;
	u64 new_oid = atomic64_inc_return(&fs->next_object_id);
	int ret;

	if (!right)
		return -EINVAL;
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	if (!scratch)
		return -ENOMEM;
	ret = alloc_tree_block(root, (u8)(rlevel + 1), new_oid, 1, &new_root);
	if (ret) {
		kvfree(scratch);
		return ret;
	}
	eb_key(old_root, 0, &left_sep);
	eb_key(right, 0, &right_sep);
	fill_ptr(fs, &lptr, old_root);
	fill_ptr(fs, &rptr, right);
	down_write(&new_root->lock);
	ret = vtx_internal_insert(new_root->hdr, fs->node_size, &left_sep,
				  &lptr, scratch, fs->node_size);
	if (!ret)
		ret = vtx_internal_insert(new_root->hdr, fs->node_size,
					  &right_sep, &rptr, scratch,
					  fs->node_size);
	up_write(&new_root->lock);
	kvfree(scratch);
	if (ret) {
		vtxfs_eb_put(new_root);
		return ret;
	}
	vtxfs_mark_eb_dirty(new_root);
	vtxfs_ctree_set_root_flag(new_root, 1);
	vtxfs_ctree_set_root_flag(old_root, 0);  /* gốc cũ hết ROOT */
	if (!root_is_physical(root)) {
		if (root->resolve_root) {
			vtxfs_omap_publish(root->resolve_root, old_root->oid, old_root->vblk);
			vtxfs_omap_publish(root->resolve_root, right->oid, right->vblk);
		}
		if (root->view) {
			vtxfs_omap_view_set(root->view, old_root->oid, old_root->vblk);
			vtxfs_omap_view_set(root->view, right->oid, right->vblk);
		}
	}
	rcu_assign_pointer(root->node, new_root);
	/* Drop old root->node reference on old_root; path retains caller reference */
	vtxfs_eb_put(old_root);
	return 0;
}

/* Đẩy (sep,ptr) lên các tầng cha; tách khi đầy; tăng gốc nếu vượt đỉnh. */
static int insert_ptr_up(struct vtxfs_root *root, struct vtxfs_path *path,
			 int start_level, struct vtx_key sep,
			 struct vtx_key_ptr_val ptr,
			 struct vtxfs_extent_buffer *first_right)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *root_eb = rcu_dereference_protected(
			root->node, lockdep_is_held(&fs->trans_lock));
	int rlevel = eb_level(root_eb);
	int level = start_level, ret = 0;
	void *scratch, *scratch2;
	struct vtxfs_extent_buffer *last_right = first_right;

	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	scratch2 = kvmalloc((size_t)fs->node_size * 2, GFP_NOFS);
	if (!scratch || !scratch2) {
		kvfree(scratch);
		kvfree(scratch2);
		return -ENOMEM;
	}
	for (;;) {
		struct vtxfs_extent_buffer *parent, *right;
		u64 right_oid;

		if (level > rlevel) {
			ret = grow_root(root, path, rlevel, last_right);
			break;
		}
		parent = path->nodes[level];
		ret = vtx_internal_insert(parent->hdr, fs->node_size, &sep,
					  &ptr, scratch, fs->node_size);
		if (ret == 0) {
			vtxfs_mark_eb_dirty(parent);
			ret = 0;
			break;
		}
		if (ret != -ENOSPC)
			break;
		/* tách parent thành (parent, right) rồi đẩy right lên */
		right_oid = atomic64_inc_return(&fs->next_object_id);
		ret = alloc_tree_block(root, (u8)level, right_oid, 0, &right);
		if (ret)
			break;
		ret = vtx_internal_split_insert(parent->hdr, right->hdr,
				fs->node_size, &sep, &ptr, scratch2,
				(size_t)fs->node_size * 2);
		if (ret) {
			vtxfs_eb_put(right);
			break;
		}
		vtxfs_mark_eb_dirty(parent);
		vtxfs_mark_eb_dirty(right);
		if (!root_is_physical(root)) {
			if (root->resolve_root)
				vtxfs_omap_publish(root->resolve_root, right->oid, right->vblk);
			if (root->view)
				vtxfs_omap_view_set(root->view, right->oid, right->vblk);
		}
		eb_key(right, 0, &sep);
		fill_ptr(fs, &ptr, right);
		last_right = right;
		level++;
	}
	kvfree(scratch);
	kvfree(scratch2);
	return ret;
}

/* ---------- item ops ---------- */
int vtxfs_insert_item(struct vtxfs_root *root, struct vtxfs_path *path,
		      const struct vtx_key *key, u32 data_size,
		      void **write_ptr_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = root->fs;
	struct vtxfs_extent_buffer *leaf = path->nodes[0];
	struct vtx_key_ptr_val ptr;
	struct vtx_key sep;
	struct vtxfs_extent_buffer *right = NULL;
	u64 right_oid;
	void *scratch, *scratch2, *tmp = NULL;
	int ret;

	if (data_size > U16_MAX)
		return -E2BIG;
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	scratch2 = kvmalloc((size_t)fs->node_size * 2, GFP_NOFS);
	if (!scratch || !scratch2) {
		ret = -ENOMEM;
		goto out;
	}
	if (data_size) {
		tmp = kvzalloc(data_size, GFP_NOFS);
		if (!tmp) {
			ret = -ENOMEM;
			goto out;
		}
	}
	/* thử chèn trực tiếp vào lá */
	down_write(&leaf->lock);
	ret = vtx_leaf_insert(leaf->hdr, fs->node_size, key, tmp,
			      (u16)data_size, scratch, fs->node_size);
	up_write(&leaf->lock);
	if (ret == 0) {
		vtxfs_mark_eb_dirty(leaf);
		goto find_ptr;
	}
	if (ret != -ENOSPC)
		goto out;

	/* lá đầy: cấp phát lá phải, split_insert, rồi đẩy con trỏ lên cha */
	right_oid = atomic64_inc_return(&fs->next_object_id);
	ret = alloc_tree_block(root, 0, right_oid, 0, &right);
	if (ret)
		goto out;
	down_write(&leaf->lock);
	ret = vtx_leaf_split_insert(leaf->hdr, right->hdr, fs->node_size,
				    key, tmp, (u16)data_size, scratch2,
				    (size_t)fs->node_size * 2);
	up_write(&leaf->lock);
	if (ret) {
		vtxfs_eb_put(right);
		goto out;
	}
	vtxfs_mark_eb_dirty(leaf);
	vtxfs_mark_eb_dirty(right);
	if (!root_is_physical(root)) {
		if (root->resolve_root)
			vtxfs_omap_publish(root->resolve_root, right->oid, right->vblk);
		if (root->view)
			vtxfs_omap_view_set(root->view, right->oid, right->vblk);
	}
	eb_key(right, 0, &sep);
	fill_ptr(fs, &ptr, right);
	ret = insert_ptr_up(root, path, 1, sep, ptr, right);
	if (ret)
		goto out;
find_ptr:
	if (write_ptr_out) {
		u32 slot;

		if (vtx_leaf_find(leaf->hdr, fs->node_size, key, &slot) == 0)
			*write_ptr_out = eb_val(leaf, (int)slot);
		else if (right &&
			 vtx_leaf_find(right->hdr, fs->node_size, key, &slot) == 0)
			*write_ptr_out = eb_val(right, (int)slot);
	}
	ret = 0;
out:
	kvfree(tmp);
	kvfree(scratch);
	kvfree(scratch2);
	return ret;
}

int vtxfs_del_ptr(struct vtxfs_root *root, struct vtxfs_path *path,
		  int level, int slot)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb = path->nodes[level];
	struct vtxfs_fs_info *fs = root->fs;
	struct vtx_key key;
	void *scratch;
	int ret;

	if (!eb || slot >= eb_nritems(eb))
		return -EINVAL;
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	if (!scratch)
		return -ENOMEM;
	eb_key(eb, slot, &key);
	down_write(&eb->lock);
	ret = vtx_internal_delete(eb->hdr, fs->node_size, &key, scratch,
				  fs->node_size);
	up_write(&eb->lock);
	kvfree(scratch);
	if (!ret)
		vtxfs_mark_eb_dirty(eb);
	return ret;
}

int vtxfs_del_item(struct vtxfs_root *root, struct vtxfs_path *path)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *leaf = path->nodes[0];
	struct vtxfs_fs_info *fs = root->fs;
	struct vtx_key key;
	void *scratch;
	int ret;

	eb_key(leaf, path->slots[0], &key);
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	if (!scratch)
		return -ENOMEM;
	down_write(&leaf->lock);
	ret = vtx_leaf_delete(leaf->hdr, fs->node_size, &key, scratch,
			      fs->node_size);
	up_write(&leaf->lock);
	kvfree(scratch);
	if (ret)
		return ret;
	vtxfs_mark_eb_dirty(leaf);

	/* Cân bằng sau khi xóa: gộp/san với anh em và co gốc nếu cần */
	ret = vtxfs_balance_after_delete(root, path, 0);
	return ret;
}

int vtxfs_truncate_item(struct vtxfs_path *path, u32 new_size, int from_end)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *leaf = path->nodes[0];
	struct vtxfs_fs_info *fs = leaf->fs;
	struct vtx_key key;
	u32 slot = path->slots[0], old_len;
	u8 *tmp, *src;
	void *scratch;
	int ret;

	eb_key(leaf, slot, &key);
	old_len = eb_val_len(leaf, slot);
	if (new_size >= old_len || new_size > U16_MAX)
		return -EINVAL;
	tmp = kvmalloc(new_size, GFP_NOFS);
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	if (!tmp || !scratch) {
		kvfree(tmp);
		kvfree(scratch);
		return -ENOMEM;
	}
	src = eb_val(leaf, slot);
	if (from_end)
		memcpy(tmp, src, new_size);
	else
		memcpy(tmp, src + (old_len - new_size), new_size);
	down_write(&leaf->lock);
	ret = vtx_leaf_replace(leaf->hdr, fs->node_size, &key, tmp,
			       (u16)new_size, scratch, fs->node_size);
	up_write(&leaf->lock);
	if (!ret)
		vtxfs_mark_eb_dirty(leaf);
	kvfree(tmp);
	kvfree(scratch);
	return ret;
}

int vtxfs_extend_item(struct vtxfs_root *root, struct vtxfs_path *path,
		      u32 new_size)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *leaf = path->nodes[0];
	struct vtxfs_fs_info *fs = root->fs;
	struct vtx_key key;
	u32 slot = path->slots[0], old_len;
	u8 *tmp;
	void *scratch;
	int ret;

	eb_key(leaf, slot, &key);
	old_len = eb_val_len(leaf, slot);
	if (new_size <= old_len || new_size > U16_MAX)
		return -EINVAL;
	tmp = kvzalloc(new_size, GFP_NOFS);
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	if (!tmp || !scratch) {
		kvfree(tmp);
		kvfree(scratch);
		return -ENOMEM;
	}
	memcpy(tmp, eb_val(leaf, slot), old_len);
	down_write(&leaf->lock);
	ret = vtx_leaf_replace(leaf->hdr, fs->node_size, &key, tmp,
			       (u16)new_size, scratch, fs->node_size);
	up_write(&leaf->lock);
	if (ret == -ENOSPC) {
		/* hết chỗ: xóa rồi chèn lại qua insert_item (có tách lá) */
		vtxfs_release_path(path);
		ret = vtxfs_search_slot(root, path, &key, new_size, 1);
		if (ret == 1) {
			void *w;

			ret = vtxfs_insert_item(root, path, &key, new_size, &w);
			if (!ret && w)
				memcpy(w, tmp, new_size);
		} else if (ret == 0) {
			ret = -EEXIST;
		}
	} else if (!ret) {
		vtxfs_mark_eb_dirty(leaf);
	}
	kvfree(tmp);
	kvfree(scratch);
	return ret;
}

int vtxfs_split_item(struct vtxfs_root *root, struct vtxfs_path *path,
		     const struct vtx_key *new_key, u32 split_offset)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *leaf = path->nodes[0];
	struct vtxfs_fs_info *fs = root->fs;
	struct vtx_key orig_key;
	u32 slot = path->slots[0], old_len;
	u8 *left_val, *right_val, *old_value;
	void *scratch;
	int ret;

	eb_key(leaf, slot, &orig_key);
	old_len = eb_val_len(leaf, slot);
	if (split_offset == 0 || split_offset >= old_len)
		return -EINVAL;
	old_value = eb_val(leaf, slot);
	left_val = kvmalloc(split_offset, GFP_NOFS);
	right_val = kvmalloc(old_len - split_offset, GFP_NOFS);
	scratch = kvmalloc(fs->node_size, GFP_NOFS);
	if (!left_val || !right_val || !scratch) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(left_val, old_value, split_offset);
	memcpy(right_val, old_value + split_offset, old_len - split_offset);
	down_write(&leaf->lock);
	ret = vtx_leaf_delete(leaf->hdr, fs->node_size, &orig_key, scratch,
			      fs->node_size);
	if (!ret)
		ret = vtx_leaf_insert(leaf->hdr, fs->node_size, &orig_key,
				      left_val, (u16)split_offset, scratch,
				      fs->node_size);
	if (!ret)
		ret = vtx_leaf_insert(leaf->hdr, fs->node_size, new_key,
				      right_val, (u16)(old_len - split_offset),
				      scratch, fs->node_size);
	up_write(&leaf->lock);
	if (!ret)
		vtxfs_mark_eb_dirty(leaf);
out:
	kvfree(left_val);
	kvfree(right_val);
	kvfree(scratch);
	return ret;
}
EXPORT_SYMBOL_GPL(vtxfs_alloc_path);
EXPORT_SYMBOL_GPL(vtxfs_free_path);
EXPORT_SYMBOL_GPL(vtxfs_search_slot);
EXPORT_SYMBOL_GPL(vtxfs_insert_item);
EXPORT_SYMBOL_GPL(vtxfs_del_item);
