// SPDX-License-Identifier: GPL-2.0-only
/*
 * VTXFS backreference query/aggregation. Based on VTXFS EXTENT/REF/INODE_REF
 * items only; Btrfs backref formats must never be parsed as VTXFS items.
 *
 * Locking: callers MUST hold fs->trans_lock while walking a ctree. The
 * functions here do not mutate persistent reference accounting.
 */
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/rbtree.h>
#include <linux/lockdep.h>

#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

#define VTXFS_BACKREF_MAX_DEPTH 64

static int slot_value(const struct vtxfs_extent_buffer *eb, int slot,
		      const void **ptr, u16 *len)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h;
	const struct vtx_slot *s;
	u32 offset, bytes;

	if (!eb || !eb->hdr || !ptr || !len)
		return -EINVAL;
	h = eb->hdr;
	if (h->level || slot < 0 || slot >= vtx_cpu16(h->nitems))
		return -EINVAL;
	/* Reject corrupted leaf geometry BEFORE dereferencing the slot. */
	if (!vtx_node_slots_valid(h, eb->node_size))
		return -EUCLEAN;
	s = &h->entries[slot].slot;
	offset = (u32)vtx_cpu16(s->val_off8) * 8U;
	bytes = vtx_cpu16(s->val_len);
	if (offset > eb->node_size || bytes > eb->node_size - offset)
		return -EUCLEAN;
	*ptr = (const u8 *)h + offset;
	*len = (u16)bytes;
	return 0;
}

int vtxfs_check_extent_in_eb(const struct vtxfs_extent_buffer *eb,
			     int slot, u64 allocation_id)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_extent_item *e;
	const void *value;
	u16 len;
	int ret;

	VTXFS_TRACE_ENTER();
	if (!allocation_id)
		return -EINVAL;
	ret = slot_value(eb, slot, &value, &len);
	if (ret)
		return ret;
	if (vtxfs_key_type(&eb->hdr->entries[slot].key) != VTX_TYPE_EXTENT)
		return 0;
	if (len < sizeof(*e))
		return -EUCLEAN;
	e = value;
	return vtx_cpu64(e->allocation_id) == allocation_id ? 1 : 0;
}

int vtxfs_find_extent_in_eb(const struct vtxfs_extent_buffer *eb,
			    u64 allocation_id, int *slot_out)
{
	VTXFS_TRACE_ENTER();

	int i, ret;

	VTXFS_TRACE_ENTER();
	if (!eb || !eb->hdr || !slot_out)
		return -EINVAL;
	for (i = 0; i < vtx_cpu16(eb->hdr->nitems); i++) {
		ret = vtxfs_check_extent_in_eb(eb, i, allocation_id);
		if (ret < 0)
			return ret;
		if (ret) {
			*slot_out = i;
			return 0;
		}
	}
	return -ENOENT;
}

/*
 * Scan FS tree items by key, not raw leaf memory. Search starts at the
 * smallest valid inode object. A non-EXTENT key is never cast to an extent.
 * vtxfs_next_key() returns 1 when iteration ends and <0 on I/O errors.
 */
int vtxfs_iterate_leaf_refs(struct vtxfs_root *root, vtxfs_extent_visit_t cb,
			    void *private)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key = {
		.oid_and_type = cpu_to_le64(VTX_MAKE_OID_TYPE(1, VTX_TYPE_ANY)),
		.offset = 0,
	};
	int ret;

	VTXFS_TRACE_ENTER();
	if (!root || !root->fs || !cb || root->tree_type != VTX_TREE_FS)
		return -EINVAL;
	lockdep_assert_held(&root->fs->trans_lock);
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot_for_read(root, path, &key, 1);
	while (ret == 0) {
		const struct vtx_node_entry *entry;
		const struct vtx_extent_item *extent;
		const void *raw;
		u16 bytes;
		int slot = path->slots[0];

		if (!path->nodes[0] || slot < 0 ||
		    slot >= eb_nritems(path->nodes[0])) {
			ret = -EUCLEAN;
			break;
		}
		entry = &path->nodes[0]->hdr->entries[slot];
		if (vtxfs_key_type(&entry->key) == VTX_TYPE_EXTENT) {
			ret = slot_value(path->nodes[0], slot, &raw, &bytes);
			if (ret)
				break;
			if (bytes < sizeof(*extent)) {
				ret = -EUCLEAN;
				break;
			}
			extent = raw;
			ret = cb(root->subvol_id, vtxfs_key_oid(&entry->key),
				 vtxfs_key_off(&entry->key), extent, private);
			if (ret)
				break;
		}
		ret = vtxfs_next_key(root, path);
	}
	vtxfs_free_path(path);
	return ret == 1 ? 0 : ret;
}

struct extent_collect {
	u64 allocation_id;
	struct list_head *out;
};

static int collect_extent(u64 subvol, u64 ino, u64 offset,
			  const struct vtx_extent_item *e, void *private)
{
	VTXFS_TRACE_ENTER();

	struct extent_collect *ctx = private;
	struct vtxfs_backref *ref;

	if (vtx_cpu64(e->allocation_id) != ctx->allocation_id)
		return 0;
	ref = kzalloc(sizeof(*ref), GFP_NOFS);
	if (!ref)
		return -ENOMEM;
	ref->owner_subvol = subvol;
	ref->owner_ino = ino;
	ref->file_offset = offset;
	ref->allocation_id = ctx->allocation_id;
	ref->type = VTX_TYPE_EXTENT;
	list_add_tail(&ref->list, ctx->out);
	return 0;
}

void vtxfs_free_backrefs(struct list_head *list)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_backref *br, *tmp;

	VTXFS_TRACE_ENTER();
	if (!list)
		return;
	list_for_each_entry_safe(br, tmp, list, list) {
		list_del(&br->list);
		kfree(br);
	}
}

int vtxfs_find_extent_backrefs(struct vtxfs_fs_info *fs, u64 allocation_id,
			       struct list_head *out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *subvol;
	struct extent_collect ctx = {
		.allocation_id = allocation_id,
		.out = out,
	};
	unsigned long index;
	int ret = 0;

	VTXFS_TRACE_ENTER();
	if (!fs || !out || !allocation_id)
		return -EINVAL;
	lockdep_assert_held(&fs->trans_lock);
	INIT_LIST_HEAD(out);
	xa_for_each(&fs->roots, index, subvol) {
		if (!subvol || subvol->tree_type != VTX_TREE_FS)
			continue;
		ret = vtxfs_iterate_leaf_refs(subvol, collect_extent, &ctx);
		if (ret)
			break;
	}
	if (ret)
		vtxfs_free_backrefs(out);
	return ret;
}

/* The authoritative REF tree, not an inferred snapshot age or inode link
 * count, decides whether a data allocation has multiple references. */
int vtxfs_extent_is_shared(struct vtxfs_fs_info *fs, u64 allocation_id,
			   bool *shared_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key;
	const struct vtx_ref_item *ref;
	const void *raw;
	u16 len;
	int ret;

	VTXFS_TRACE_ENTER();
	if (!fs || !fs->ref_root || !shared_out || !allocation_id)
		return -EINVAL;
	lockdep_assert_held(&fs->trans_lock);
	ret = vtxfs_mk_key(&key, allocation_id, VTX_TYPE_REF, 0);
	if (ret)
		return ret;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot(fs->ref_root, path, &key, 0, 0);
	if (ret != 0) {
		ret = ret > 0 ? -ENOENT : ret;
		goto out;
	}
	ret = slot_value(path->nodes[0], path->slots[0], &raw, &len);
	if (ret)
		goto out;
	if (len != sizeof(*ref)) {
		ret = -EUCLEAN;
		goto out;
	}
	ref = raw;
	if (vtx_cpu64(ref->allocation_id) != allocation_id ||
	    !vtx_cpu64(ref->refcount)) {
		ret = -EUCLEAN;
		goto out;
	}
	*shared_out = vtx_cpu64(ref->refcount) > 1;
	ret = 0;
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_is_data_extent_shared(struct vtxfs_fs_info *fs, u64 allocation_id,
				bool *shared_out)
{
	VTXFS_TRACE_ENTER();
	return vtxfs_extent_is_shared(fs, allocation_id, shared_out);
}

int vtxfs_extent_from_logical(struct vtxfs_root *root, u64 ino,
			      u64 file_offset, struct vtx_extent_item *out,
			      u64 *extent_start)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key;
	int ret;

	VTXFS_TRACE_ENTER();
	if (!root || !root->fs || !out || !extent_start)
		return -EINVAL;
	lockdep_assert_held(&root->fs->trans_lock);
	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_EXTENT, 0);
	if (ret)
		return ret;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot_for_read(root, path, &key, 1);
	while (ret == 0) {
		const struct vtx_node_entry *entry;
		const void *raw;
		u64 start, length;
		u16 bytes;
		struct vtx_extent_item ext;

		entry = &path->nodes[0]->hdr->entries[path->slots[0]];
		if (vtxfs_key_oid(&entry->key) != ino ||
		    vtxfs_key_type(&entry->key) != VTX_TYPE_EXTENT) {
			ret = -ENOENT;
			break;
		}
		ret = slot_value(path->nodes[0], path->slots[0], &raw, &bytes);
		if (ret)
			break;
		if (bytes < sizeof(ext)) {
			ret = -EUCLEAN;
			break;
		}
		memcpy(&ext, raw, sizeof(ext));
		start = vtxfs_key_off(&entry->key);
		length = vtx_cpu32(ext.logical_bytes);
		if (file_offset >= start && file_offset - start < length) {
			*out = ext;
			*extent_start = start;
			ret = 0;
			break;
		}
		if (start > file_offset) {
			ret = -ENOENT;
			break;
		}
		ret = vtxfs_next_key(root, path);
	}
	vtxfs_free_path(path);
	return ret == 1 ? -ENOENT : ret;
}

/* Transient preliminary references: safe aggregation by explicit VTXFS
 * (allocation, subvolume, inode, offset, type), not Btrfs extent hashes. */
void vtxfs_prelim_ref_init(struct vtxfs_backref_context *ctx, u64 allocation_id,
			   u64 view_xid)
{
	VTXFS_TRACE_ENTER();
	memset(ctx, 0, sizeof(*ctx));
	ctx->refs = RB_ROOT;
	INIT_LIST_HEAD(&ctx->pending);
	ctx->allocation_id = allocation_id;
	ctx->view_xid = view_xid;
}

static int compare_u64(u64 a, u64 b)
{
	VTXFS_TRACE_ENTER();

	return (a > b) - (a < b);
}

int vtxfs_prelim_ref_compare(const struct vtxfs_prelim_ref *a,
			    const struct vtxfs_prelim_ref *b)
{
	VTXFS_TRACE_ENTER();

	int ret;

	VTXFS_TRACE_ENTER();
	ret = compare_u64(a->allocation_id, b->allocation_id);
	if (!ret)
		ret = compare_u64(a->owner_subvol, b->owner_subvol);
	if (!ret)
		ret = compare_u64(a->owner_ino, b->owner_ino);
	if (!ret)
		ret = compare_u64(a->file_offset, b->file_offset);
	if (!ret)
		ret = (a->indirect > b->indirect) -
		      (a->indirect < b->indirect);
	return ret;
}

int vtxfs_prelim_ref_rb_add_cmp(const struct vtxfs_prelim_ref *a,
			       const struct vtxfs_prelim_ref *b)
{
	VTXFS_TRACE_ENTER();
	return vtxfs_prelim_ref_compare(a, b);
}

static struct vtxfs_prelim_ref *prelim_find(
		const struct vtxfs_backref_context *ctx,
		const struct vtxfs_prelim_ref *key)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n = ctx->refs.rb_node;

	while (n) {
		struct vtxfs_prelim_ref *entry =
			rb_entry(n, struct vtxfs_prelim_ref, rb);
		int cmp = vtxfs_prelim_ref_compare(key, entry);

		if (!cmp)
			return entry;
		n = cmp < 0 ? n->rb_left : n->rb_right;
	}
	return NULL;
}

int vtxfs_update_share_count(struct vtxfs_backref_context *ctx,
			     const struct vtxfs_prelim_ref *key,
			     s64 delta)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_prelim_ref *found;
	s64 new_count;

	VTXFS_TRACE_ENTER();
	if (!ctx || !key || key->allocation_id != ctx->allocation_id)
		return -EINVAL;
	found = prelim_find(ctx, key);
	if (!found)
		return -ENOENT;
	if (check_add_overflow(found->count, delta, &new_count))
		return -EOVERFLOW;
	found->count = new_count;
	return 0;
}

int vtxfs_prelim_ref_insert(struct vtxfs_backref_context *ctx,
			    const struct vtxfs_prelim_ref *ref)
{
	VTXFS_TRACE_ENTER();

	struct rb_node **link, *parent = NULL;
	struct vtxfs_prelim_ref *new;

	VTXFS_TRACE_ENTER();
	if (!ctx || !ref || ref->allocation_id != ctx->allocation_id ||
	    !ctx->allocation_id || !ref->count)
		return -EINVAL;
	link = &ctx->refs.rb_node;
	while (*link) {
		struct vtxfs_prelim_ref *entry =
			rb_entry(*link, struct vtxfs_prelim_ref, rb);
		int cmp = vtxfs_prelim_ref_compare(ref, entry);

		if (!cmp)
			return vtxfs_update_share_count(ctx, ref, ref->count);
		parent = *link;
		link = cmp < 0 ? &(*link)->rb_left : &(*link)->rb_right;
	}
	new = kzalloc(sizeof(*new), GFP_NOFS);
	if (!new)
		return -ENOMEM;
	new->allocation_id = ref->allocation_id;
	new->owner_subvol = ref->owner_subvol;
	new->owner_ino = ref->owner_ino;
	new->file_offset = ref->file_offset;
	new->indirect = ref->indirect;
	new->count = ref->count;
	INIT_LIST_HEAD(&new->list);
	rb_link_node(&new->rb, parent, link);
	rb_insert_color(&new->rb, &ctx->refs);
	list_add_tail(&new->list, &ctx->pending);
	return 0;
}

int vtxfs_add_prelim_ref(struct vtxfs_backref_context *ctx, u64 subvol,
			 u64 ino, u64 file_offset, s64 count, bool indirect)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_prelim_ref key = {
		.allocation_id = ctx ? ctx->allocation_id : 0,
		.owner_subvol = subvol, .owner_ino = ino,
		.file_offset = file_offset, .count = count,
		.indirect = indirect,
	};

	VTXFS_TRACE_ENTER();
	return vtxfs_prelim_ref_insert(ctx, &key);
}

int vtxfs_add_direct_ref(struct vtxfs_backref_context *ctx, u64 subvol,
			 u64 ino, u64 file_offset, s64 count)
{
	VTXFS_TRACE_ENTER();
	return vtxfs_add_prelim_ref(ctx, subvol, ino, file_offset, count, false);
}

int vtxfs_add_indirect_ref(struct vtxfs_backref_context *ctx, u64 subvol,
			   u64 ino, u64 file_offset, s64 count)
{
	VTXFS_TRACE_ENTER();
	return vtxfs_add_prelim_ref(ctx, subvol, ino, file_offset, count, true);
}

int vtxfs_is_shared_data_backref(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_prelim_ref *entry;
	u64 total = 0;

	VTXFS_TRACE_ENTER();
	if (!ctx)
		return -EINVAL;
	list_for_each_entry(entry, &ctx->pending, list) {
		if (entry->count <= 0)
			continue;
		if ((u64)entry->count > U64_MAX - total)
			return -EOVERFLOW;
		total += (u64)entry->count;
	}
	/* Diagnostic only: transient refs may omit snapshots and pending refs.
	 * For block-reuse decisions query the authoritative REF tree instead. */
	return total > 1;
}

int vtxfs_lookup_backref_shared_cache(const struct vtxfs_backref_context *ctx,
				      u64 subvol, u64 ino, u64 offset)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_prelim_ref key = {
		.allocation_id = ctx ? ctx->allocation_id : 0,
		.owner_subvol = subvol, .owner_ino = ino,
		.file_offset = offset,
	};
	struct vtxfs_prelim_ref *found;

	VTXFS_TRACE_ENTER();
	if (!ctx)
		return -EINVAL;
	found = prelim_find(ctx, &key);
	return found ? (found->count > 1) : -ENOENT;
}

int vtxfs_store_backref_shared_cache(struct vtxfs_backref_context *ctx,
				     u64 subvol, u64 ino, u64 offset)
{
	VTXFS_TRACE_ENTER();
	/* This is a per-query cache: never persist into a future XID. */
	return vtxfs_add_direct_ref(ctx, subvol, ino, offset, 1);
}

void vtxfs_prelim_release(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_prelim_ref *entry, *tmp;

	VTXFS_TRACE_ENTER();
	if (!ctx)
		return;
	list_for_each_entry_safe(entry, tmp, &ctx->pending, list) {
		rb_erase(&entry->rb, &ctx->refs);
		list_del(&entry->list);
		kfree(entry);
	}
	ctx->refs = RB_ROOT;
	ctx->observed_references = 0;
}

/* Iterate owners in every live subvolume registered in the kernel cache.
 * A missing/unloaded subvolume is NOT evidence of zero references. */
int vtxfs_iterate_extent_inodes(struct vtxfs_fs_info *fs, u64 allocation_id,
				vtxfs_backref_visit_t cb, void *private)
{
	VTXFS_TRACE_ENTER();

	LIST_HEAD(found);
	struct vtxfs_backref *ref;
	int ret;

	VTXFS_TRACE_ENTER();
	if (!cb)
		return -EINVAL;
	ret = vtxfs_find_extent_backrefs(fs, allocation_id, &found);
	if (ret)
		return ret;
	list_for_each_entry(ref, &found, list) {
		ret = cb(ref, private);
		if (ret)
			break;
	}
	vtxfs_free_backrefs(&found);
	return ret;
}

int vtxfs_iterate_inodes_from_logical(struct vtxfs_fs_info *fs,
				     u64 allocation_id,
				     vtxfs_backref_visit_t cb, void *private)
{
	VTXFS_TRACE_ENTER();
	/* VTXFS uses immutable allocation_id, not Btrfs physical-bytenr. */
	return vtxfs_iterate_extent_inodes(fs, allocation_id, cb, private);
}

struct inode_list_ctx {
	struct list_head *list;
};

static int add_inode_elem(const struct vtxfs_backref *br, void *private)
{
	VTXFS_TRACE_ENTER();

	struct inode_list_ctx *ctx = private;
	struct vtxfs_inode_elem *entry;

	entry = kzalloc(sizeof(*entry), GFP_NOFS);
	if (!entry)
		return -ENOMEM;
	entry->subvol_id = br->owner_subvol;
	entry->ino = br->owner_ino;
	entry->offset = br->file_offset;
	list_add_tail(&entry->list, ctx->list);
	return 0;
}

void vtxfs_free_inode_elem_list(struct list_head *list)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_inode_elem *entry, *tmp;

	VTXFS_TRACE_ENTER();
	if (!list)
		return;
	list_for_each_entry_safe(entry, tmp, list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
}

int vtxfs_build_ino_list(struct vtxfs_fs_info *fs, u64 allocation_id,
			 struct list_head *out)
{
	VTXFS_TRACE_ENTER();

	struct inode_list_ctx ctx = { .list = out };
	int ret;

	VTXFS_TRACE_ENTER();
	if (!out)
		return -EINVAL;
	INIT_LIST_HEAD(out);
	ret = vtxfs_iterate_extent_inodes(fs, allocation_id,
					 add_inode_elem, &ctx);
	if (ret)
		vtxfs_free_inode_elem_list(out);
	return ret;
}

int vtxfs_backref_iter_init(struct vtxfs_backref_iter *it,
			    struct vtxfs_fs_info *fs, u64 allocation_id)
{
	VTXFS_TRACE_ENTER();
	if (!it || !fs || !allocation_id)
		return -EINVAL;
	memset(it, 0, sizeof(*it));
	it->fs = fs;
	it->allocation_id = allocation_id;
	INIT_LIST_HEAD(&it->references);
	it->cursor = &it->references;
	return 0;
}

int vtxfs_backref_iter_start(struct vtxfs_backref_iter *it)
{
	VTXFS_TRACE_ENTER();

	int ret;

	VTXFS_TRACE_ENTER();
	if (!it || !it->fs || it->started)
		return -EINVAL;
	ret = vtxfs_find_extent_backrefs(it->fs, it->allocation_id,
					&it->references);
	if (ret)
		return ret;
	it->started = true;
	it->cursor = it->references.next;
	return 0;
}

const struct vtxfs_backref *vtxfs_backref_iter_next(struct vtxfs_backref_iter *it)
{
	VTXFS_TRACE_ENTER();

	const struct vtxfs_backref *ref;

	VTXFS_TRACE_ENTER();
	if (!it || !it->started || it->cursor == &it->references)
		return NULL;
	ref = list_entry(it->cursor, const struct vtxfs_backref, list);
	it->cursor = it->cursor->next;
	return ref;
}

bool vtxfs_backref_iter_is_inline_ref(const struct vtxfs_backref_iter *it)
{
	VTXFS_TRACE_ENTER();
	/* Inline data is in INODE/EXTENT payload, not a backref record. */
	return false;
}

void vtxfs_backref_iter_release(struct vtxfs_backref_iter *it)
{
	VTXFS_TRACE_ENTER();
	if (!it)
		return;
	vtxfs_free_backrefs(&it->references);
	it->cursor = &it->references;
	it->started = false;
}

/* VTXFS INODE_REF key offset is parent inode, value contains the name.
 * Traverse only references for the requested inode, never interpret Btrfs
 * INODE_EXTREF keys (which are not part of VTXFS v2.1). */
int vtxfs_iterate_inode_refs(struct vtxfs_root *root, u64 ino,
			    vtxfs_backref_visit_t cb, void *private)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key;
	int ret;

	VTXFS_TRACE_ENTER();
	if (!root || !root->fs || !cb)
		return -EINVAL;
	lockdep_assert_held(&root->fs->trans_lock);
	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_INODE_REF, 0);
	if (ret)
		return ret;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot_for_read(root, path, &key, 1);
	while (!ret) {
		const struct vtx_node_entry *entry;
		const struct vtx_inode_ref *iref;
		const void *raw;
		u16 len;
		struct vtxfs_backref item;

		entry = &path->nodes[0]->hdr->entries[path->slots[0]];
		if (vtxfs_key_oid(&entry->key) != ino ||
		    vtxfs_key_type(&entry->key) != VTX_TYPE_INODE_REF) {
			ret = 0;
			break;
		}
		ret = slot_value(path->nodes[0], path->slots[0], &raw, &len);
		if (ret)
			break;
		iref = raw;
		if (len < sizeof(*iref) ||
		    vtx_cpu16(iref->name_len) > VTXFS_NAME_MAX ||
		    vtx_cpu16(iref->name_len) > len - sizeof(*iref)) {
			ret = -EUCLEAN;
			break;
		}
		memset(&item, 0, sizeof(item));
		item.owner_subvol = root->subvol_id;
		item.owner_ino = ino;
		item.file_offset = vtxfs_key_off(&entry->key); /* parent ino */
		item.type = VTX_TYPE_INODE_REF;
		ret = cb(&item, private);
		if (ret)
			break;
		ret = vtxfs_next_key(root, path);
	}
	vtxfs_free_path(path);
	return ret == 1 ? 0 : ret;
}

int vtxfs_iterate_inode_extrefs(struct vtxfs_root *root, u64 ino,
			       vtxfs_backref_visit_t cb, void *private)
{
	VTXFS_TRACE_ENTER();
	/* VTXFS has INODE_REF and DIR_ITEM. No Btrfs-style INODE_EXTREF. */
	return -EOPNOTSUPP;
}

/* Read one parent and name from the INODE_REF. Caller may not release the
 * trans_lock during reconstruction: paths can be invalidated by rename. */
static int parent_name_from_ref(struct vtxfs_root *root, u64 ino,
				u64 *parent_ino, char *name, size_t cap)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key key;
	const struct vtx_node_entry *entry;
	const struct vtx_inode_ref *iref;
	const void *raw;
	u16 len, nlen;
	int ret;

	ret = vtxfs_mk_key(&key, ino, VTX_TYPE_INODE_REF, 0);
	if (ret)
		return ret;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot_for_read(root, path, &key, 1);
	if (ret) {
		ret = ret == 1 ? -ENOENT : ret;
		goto out;
	}
	entry = &path->nodes[0]->hdr->entries[path->slots[0]];
	if (vtxfs_key_oid(&entry->key) != ino ||
	    vtxfs_key_type(&entry->key) != VTX_TYPE_INODE_REF) {
		ret = -ENOENT;
		goto out;
	}
	ret = slot_value(path->nodes[0], path->slots[0], &raw, &len);
	if (ret)
		goto out;
	iref = raw;
	if (len < sizeof(*iref)) {
		ret = -EUCLEAN;
		goto out;
	}
	nlen = vtx_cpu16(iref->name_len);
	if (!nlen || nlen > VTXFS_NAME_MAX ||
	    nlen >= cap || nlen > len - sizeof(*iref) ||
	    memchr(iref->name, '/', nlen) ||
	    memchr(iref->name, '\0', nlen) ||
	    (nlen == 1 && iref->name[0] == '.') ||
	    (nlen == 2 && iref->name[0] == '.' &&
	     iref->name[1] == '.')) {
		ret = -EUCLEAN;
		goto out;
	}
	memcpy(name, iref->name, nlen);
	name[nlen] = '\0';
	*parent_ino = vtxfs_key_off(&entry->key);
	ret = 0;
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_inode_to_path(struct vtxfs_root *root, u64 ino, char **path_out)
{
	VTXFS_TRACE_ENTER();

	char *buf, name[VTXFS_NAME_MAX + 1];
	size_t end = PATH_MAX - 1;
	unsigned int depth;
	u64 parent;
	int ret = 0;

	VTXFS_TRACE_ENTER();
	if (!root || !root->fs || !path_out)
		return -EINVAL;
	lockdep_assert_held(&root->fs->trans_lock);
	*path_out = NULL;
	if (ino < VTXFS_INO_ROOT_DIR)
		return -EINVAL;
	buf = kmalloc(PATH_MAX, GFP_NOFS);
	if (!buf)
		return -ENOMEM;
	buf[end] = '\0';
	for (depth = 0; ino != VTXFS_INO_ROOT_DIR &&
	     depth < VTXFS_BACKREF_MAX_DEPTH; depth++) {
		size_t n;

		ret = parent_name_from_ref(root, ino, &parent,
					   name, sizeof(name));
		if (ret)
			goto out;
		if (parent == ino || parent < VTXFS_INO_ROOT_DIR) {
			ret = -EUCLEAN;
			goto out;
		}
		n = strlen(name);
		if (end < n + 1) {
			ret = -ENAMETOOLONG;
			goto out;
		}
		end -= n;
		memcpy(buf + end, name, n);
		buf[--end] = '/';
		ino = parent;
	}
	if (ino != VTXFS_INO_ROOT_DIR) {
		ret = -ELOOP;
		goto out;
	}
	if (end == PATH_MAX - 1)
		buf[--end] = '/';
	*path_out = kstrdup(buf + end, GFP_NOFS);
	if (!*path_out)
		ret = -ENOMEM;
out:
	kfree(buf);
	return ret;
}

int vtxfs_paths_from_inode(struct vtxfs_root *root, u64 ino,
			   struct vtxfs_inode_path *result)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path_record *record;
	char *path = NULL;
	int ret;

	VTXFS_TRACE_ENTER();
	if (!result)
		return -EINVAL;
	INIT_LIST_HEAD(&result->records);
	result->count = 0;
	ret = vtxfs_inode_to_path(root, ino, &path);
	if (ret)
		return ret;
	record = kzalloc(sizeof(*record), GFP_NOFS);
	if (!record) {
		kfree(path);
		return -ENOMEM;
	}
	record->path = path;
	record->ino = ino;
	record->subvol_id = root->subvol_id;
	list_add_tail(&record->list, &result->records);
	result->count = 1;
	return 0;
}

void vtxfs_free_path_records(struct list_head *list)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path_record *r, *tmp;

	VTXFS_TRACE_ENTER();
	if (!list)
		return;
	list_for_each_entry_safe(r, tmp, list, list) {
		list_del(&r->list);
		kfree(r->path);
		kfree(r);
	}
}

void vtxfs_free_leaf_list(struct list_head *list)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_leaf_record *record, *tmp;

	VTXFS_TRACE_ENTER();
	if (!list)
		return;
	list_for_each_entry_safe(record, tmp, list, list) {
		list_del(&record->list);
		vtxfs_eb_put(record->eb);
		kfree(record);
	}
}

/* VTXFS has no inline/keyed Btrfs backref records, no shared-block parent
 * chain encoded by (bytenr,level,root), and no INODE_EXTREF key type.
 * Explicit errors are safer than false ownership assertions. */
int vtxfs_get_extent_inline_ref(void)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_add_inline_refs(void)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_add_keyed_refs(void)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_resolve_indirect_ref(void)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_resolve_indirect_refs(void)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_add_missing_keys(void)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_find_parent_nodes(void)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_init_data_container(struct vtxfs_data_container *container,
			      u64 allocation_id)
{
	VTXFS_TRACE_ENTER();
	if (!container || !allocation_id)
		return -EINVAL;
	container->allocation_id = allocation_id;
	INIT_LIST_HEAD(&container->inode_elems);
	return 0;
}

int vtxfs_init_ipath(struct vtxfs_inode_path *ipath)
{
	VTXFS_TRACE_ENTER();
	if (!ipath)
		return -EINVAL;
	INIT_LIST_HEAD(&ipath->records);
	ipath->count = 0;
	return 0;
}

int vtxfs_free_pref(struct vtxfs_backref_context *ctx,
		    const struct vtxfs_prelim_ref *key)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_prelim_ref *ref;

	VTXFS_TRACE_ENTER();
	if (!ctx || !key)
		return -EINVAL;
	ref = prelim_find(ctx, key);
	if (!ref)
		return -ENOENT;
	rb_erase(&ref->rb, &ctx->refs);
	list_del(&ref->list);
	kfree(ref);
	return 0;
}

int vtxfs_unode_aux_to_inode_list(struct vtxfs_backref_context *ctx,
				  struct list_head *out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_prelim_ref *pref;
	int ret = 0;

	VTXFS_TRACE_ENTER();
	if (!ctx || !out)
		return -EINVAL;
	INIT_LIST_HEAD(out);
	list_for_each_entry(pref, &ctx->pending, list) {
		struct vtxfs_inode_elem *elem;

		if (pref->count <= 0 || pref->indirect)
			continue;
		elem = kzalloc(sizeof(*elem), GFP_NOFS);
		if (!elem) {
			ret = -ENOMEM;
			break;
		}
		elem->subvol_id = pref->owner_subvol;
		elem->ino = pref->owner_ino;
		elem->offset = pref->file_offset;
		list_add_tail(&elem->list, out);
	}
	if (ret)
		vtxfs_free_inode_elem_list(out);
	return ret;
}

int vtxfs_add_all_parents(struct vtxfs_root *root, u64 ino,
			  vtxfs_backref_visit_t cb, void *private)
{
	VTXFS_TRACE_ENTER();
	/* No inode extrefs; parent_ino is INODE_REF key.offset. */
	return vtxfs_iterate_inode_refs(root, ino, cb, private);
}

int vtxfs_ref_to_path(struct vtxfs_root *root, u64 ino, char **path_out)
{
	VTXFS_TRACE_ENTER();
	return vtxfs_inode_to_path(root, ino, path_out);
}

int vtxfs_tree_backref_for_extent(struct vtxfs_fs_info *fs,
				  u64 allocation_id, struct vtx_ref_item *out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	const struct vtx_ref_item *found;
	const void *raw;
	struct vtx_key key;
	u16 len;
	int ret;

	VTXFS_TRACE_ENTER();
	/* VTXFS REF is authoritative refcount, not Btrfs "tree backref". */
	if (!fs || !fs->ref_root || !out || !allocation_id)
		return -EINVAL;
	lockdep_assert_held(&fs->trans_lock);
	ret = vtxfs_mk_key(&key, allocation_id, VTX_TYPE_REF, 0);
	if (ret)
		return ret;
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot(fs->ref_root, path, &key, 0, 0);
	if (ret) {
		ret = ret > 0 ? -ENOENT : ret;
		goto out;
	}
	ret = slot_value(path->nodes[0], path->slots[0], &raw, &len);
	if (ret)
		goto out;
	if (len != sizeof(*out)) {
		ret = -EUCLEAN;
		goto out;
	}
	found = raw;
	if (vtx_cpu64(found->allocation_id) != allocation_id ||
	    !vtx_cpu64(found->refcount)) {
		ret = -EUCLEAN;
		goto out;
	}
	memcpy(out, found, sizeof(*out));
	ret = 0;
out:
	vtxfs_free_path(path);
	return ret;
}

/* Each leaf buffer is held exactly once even when it contains multiple
 * EXTENT items matching allocation_id. Caller releases with free_leaf_list. */
int vtxfs_find_all_leafs(struct vtxfs_root *root, u64 allocation_id,
			 struct list_head *out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path;
	struct vtx_key start = {
		.oid_and_type = cpu_to_le64(VTX_MAKE_OID_TYPE(1, VTX_TYPE_ANY)),
		.offset = 0,
	};
	int ret;

	VTXFS_TRACE_ENTER();
	if (!root || !root->fs || !allocation_id || !out ||
	    root->tree_type != VTX_TREE_FS)
		return -EINVAL;
	lockdep_assert_held(&root->fs->trans_lock);
	INIT_LIST_HEAD(out);
	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = vtxfs_search_slot_for_read(root, path, &start, 1);
	while (!ret) {
		struct vtxfs_extent_buffer *leaf = path->nodes[0];
		struct vtxfs_leaf_record *node;
		bool seen = false;
		int match;
		int slot = path->slots[0];

		if (!leaf || slot < 0 || slot >= eb_nritems(leaf)) {
			ret = -EUCLEAN;
			break;
		}
		match = vtxfs_check_extent_in_eb(leaf, slot, allocation_id);
		if (match < 0) {
			ret = match;
			break;
		}
		if (match > 0) {
			list_for_each_entry(node, out, list) {
				if (node->eb->vblk == leaf->vblk) {
					seen = true;
					break;
				}
			}
			if (!seen) {
				node = kzalloc(sizeof(*node), GFP_NOFS);
				if (!node) {
					ret = -ENOMEM;
					break;
				}
				vtxfs_eb_get(leaf);
				node->eb = leaf;
				node->owner_subvol = root->subvol_id;
				list_add_tail(&node->list, out);
			}
		}
		ret = vtxfs_next_key(root, path);
	}
	vtxfs_free_path(path);
	if (ret < 0)
		vtxfs_free_leaf_list(out);
	return ret == 1 ? 0 : ret;
}

/* Btrfs-specific tree parent and cache protocols cannot be implemented
 * against a REF key storing only allocation_id and aggregate refcount.
 * Reject before yielding false reachability/ownership information.
 */
int vtxfs_add_delayed_refs(struct vtxfs_backref_context *ctx,
			    struct vtxfs_trans *trans)
{
	VTXFS_TRACE_ENTER();
	/* VTXFS delayed refs have no owner inode/parent-tree identity. */
	return -EOPNOTSUPP;
}

int vtxfs_find_all_roots_safe(struct vtxfs_fs_info *fs,
			      u64 allocation_id, struct list_head *out)
{
	VTXFS_TRACE_ENTER();
	/* fs->roots caches loaded roots only; complete snapshot enumeration
	 * requires root-tree traversal and resolved OMAP views. */
	return -EOPNOTSUPP;
}

int vtxfs_find_one_extref(struct vtxfs_root *root, u64 ino)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_handle_direct_tree_backref(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_handle_indirect_tree_backref(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_backref_add_tree_node(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_backref_finish_upper_links(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

void vtxfs_backref_error_cleanup(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();
	vtxfs_prelim_release(ctx);
}

int vtxfs_backref_init_cache(struct vtxfs_backref_context *ctx)
{
	VTXFS_TRACE_ENTER();

	u64 allocation_id, xid;

	VTXFS_TRACE_ENTER();
	if (!ctx || !ctx->allocation_id)
		return -EINVAL;
	if (!RB_EMPTY_ROOT(&ctx->refs))
		return -EBUSY; /* release cached references before reinit */
	allocation_id = ctx->allocation_id;
	xid = ctx->view_xid;
	vtxfs_prelim_ref_init(ctx, allocation_id, xid);
	return 0;
}

/* The Btrfs graph-cache node lifecycle encodes a parent-bytenr graph that
 * cannot be synthesized from VTXFS REF (allocation_id, refcount) metadata.
 * No caller may mistake these failures for successful graph creation. */
int vtxfs_backref_alloc_node(struct vtxfs_backref_context *ctx, u64 node_bytenr)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_backref_free_node(struct vtxfs_backref_context *ctx, u64 node_bytenr)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_backref_drop_node(struct vtxfs_backref_context *ctx, u64 node_bytenr)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}

int vtxfs_backref_cleanup_node(struct vtxfs_backref_context *ctx,
			       u64 node_bytenr)
{
	VTXFS_TRACE_ENTER();
	return -EOPNOTSUPP;
}
