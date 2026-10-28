/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VTXFS allocation backreference querying and aggregation.
 *
 * VTXFS v2.1 is NOT Btrfs on-disk format. In VTXFS, EXTENT belongs to
 * (subvolume,inode,offset); REF is authoritative by allocation_id.
 * INLINE and keyed Btrfs backrefs do not exist on disk; unsupported
 * queries must return -EOPNOTSUPP instead of manufacturing references.
 *
 * Caller holds fs->trans_lock for the duration of any FS/REF-tree walk.
 * Backref results are transient diagnostic objects: NEVER use an in-memory
 * list to decide whether blocks may be reused (REF+SFQ rules are authoritative).
 */
#ifndef VTXFS_BACKREF_H
#define VTXFS_BACKREF_H

#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/types.h>

struct vtxfs_fs_info;
struct vtxfs_extent_buffer;
struct vtxfs_root;
struct vtxfs_trans;
struct vtx_extent_item;

struct vtxfs_backref;
struct vtxfs_prelim_ref {
	struct rb_node rb;
	struct list_head list;
	u64 allocation_id;
	u64 owner_subvol;
	u64 owner_ino;
	u64 file_offset;
	s64 count;
	bool indirect;
};

struct vtxfs_backref_context {
	struct rb_root refs;
	struct list_head pending;
	u64 allocation_id;
	u64 view_xid;
	u64 observed_references;
};

struct vtxfs_backref_iter {
	struct vtxfs_fs_info *fs;
	struct list_head references;
	struct list_head *cursor;
	u64 allocation_id;
	bool started;
};

struct vtxfs_inode_elem {
	struct list_head list;
	u64 subvol_id;
	u64 ino;
	u64 offset;
};

struct vtxfs_path_record {
	struct list_head list;
	char *path;
	u64 subvol_id;
	u64 ino;
};

struct vtxfs_inode_path {
	struct list_head records;
	u32 count;
};

typedef int (*vtxfs_backref_visit_t)(const struct vtxfs_backref *ref,
				     void *private);
typedef int (*vtxfs_extent_visit_t)(u64 subvol, u64 ino, u64 offset,
				    const struct vtx_extent_item *extent,
				    void *private);

int vtxfs_check_extent_in_eb(const struct vtxfs_extent_buffer *eb,
			     int slot, u64 allocation_id);
int vtxfs_find_extent_in_eb(const struct vtxfs_extent_buffer *eb,
			    u64 allocation_id, int *slot_out);
int vtxfs_extent_is_shared(struct vtxfs_fs_info *fs, u64 allocation_id,
			   bool *shared_out);
int vtxfs_is_data_extent_shared(struct vtxfs_fs_info *fs,
			       u64 allocation_id, bool *shared_out);

void vtxfs_prelim_ref_init(struct vtxfs_backref_context *ctx, u64 allocation_id,
			   u64 view_xid);
int vtxfs_prelim_ref_compare(const struct vtxfs_prelim_ref *a,
			    const struct vtxfs_prelim_ref *b);
int vtxfs_prelim_ref_rb_add_cmp(const struct vtxfs_prelim_ref *a,
			       const struct vtxfs_prelim_ref *b);
int vtxfs_update_share_count(struct vtxfs_backref_context *ctx,
			     const struct vtxfs_prelim_ref *ref,
			     s64 delta);
int vtxfs_prelim_ref_insert(struct vtxfs_backref_context *ctx,
			    const struct vtxfs_prelim_ref *ref);
int vtxfs_add_prelim_ref(struct vtxfs_backref_context *ctx, u64 subvol,
			 u64 ino, u64 file_offset, s64 count, bool indirect);
int vtxfs_add_direct_ref(struct vtxfs_backref_context *ctx, u64 subvol,
			 u64 ino, u64 file_offset, s64 count);
int vtxfs_add_indirect_ref(struct vtxfs_backref_context *ctx, u64 subvol,
			   u64 ino, u64 file_offset, s64 count);
int vtxfs_is_shared_data_backref(struct vtxfs_backref_context *ctx);
int vtxfs_lookup_backref_shared_cache(const struct vtxfs_backref_context *ctx,
				      u64 subvol, u64 ino, u64 offset);
int vtxfs_store_backref_shared_cache(struct vtxfs_backref_context *ctx,
				     u64 subvol, u64 ino, u64 offset);
void vtxfs_prelim_release(struct vtxfs_backref_context *ctx);

int vtxfs_extent_from_logical(struct vtxfs_root *root, u64 ino,
                              u64 file_offset, struct vtx_extent_item *extent_out,
                              u64 *extent_start);
int vtxfs_iterate_leaf_refs(struct vtxfs_root *root, vtxfs_extent_visit_t cb,
			    void *private);
int vtxfs_iterate_extent_inodes(struct vtxfs_fs_info *fs, u64 allocation_id,
				vtxfs_backref_visit_t cb, void *private);
int vtxfs_iterate_inodes_from_logical(struct vtxfs_fs_info *fs,
				     u64 allocation_id,
				     vtxfs_backref_visit_t cb, void *private);
int vtxfs_build_ino_list(struct vtxfs_fs_info *fs, u64 allocation_id,
			 struct list_head *out);
void vtxfs_free_inode_elem_list(struct list_head *list);

int vtxfs_backref_iter_init(struct vtxfs_backref_iter *it,
			    struct vtxfs_fs_info *fs, u64 allocation_id);
int vtxfs_backref_iter_start(struct vtxfs_backref_iter *it);
const struct vtxfs_backref *vtxfs_backref_iter_next(struct vtxfs_backref_iter *it);
bool vtxfs_backref_iter_is_inline_ref(const struct vtxfs_backref_iter *it);
void vtxfs_backref_iter_release(struct vtxfs_backref_iter *it);

int vtxfs_inode_to_path(struct vtxfs_root *root, u64 ino,
			char **path_out);
int vtxfs_paths_from_inode(struct vtxfs_root *root, u64 ino,
			   struct vtxfs_inode_path *result);
void vtxfs_free_leaf_list(struct list_head *list);
int vtxfs_iterate_inode_refs(struct vtxfs_root *root, u64 ino,
			    vtxfs_backref_visit_t cb, void *private);
int vtxfs_iterate_inode_extrefs(struct vtxfs_root *root, u64 ino,
			       vtxfs_backref_visit_t cb, void *private);

int vtxfs_get_extent_inline_ref(void);
int vtxfs_add_inline_refs(void);
int vtxfs_add_keyed_refs(void);
int vtxfs_resolve_indirect_ref(void);
int vtxfs_resolve_indirect_refs(void);
int vtxfs_add_missing_keys(void);
int vtxfs_find_parent_nodes(void);


/* Diagnostic enumeration result: a held extent buffer per distinct leaf. */
struct vtxfs_leaf_record {
	struct list_head list;
	struct vtxfs_extent_buffer *eb;
	u64 owner_subvol;
};

struct vtxfs_data_container {
	struct list_head inode_elems;
	u64 allocation_id;
};

int vtxfs_init_data_container(struct vtxfs_data_container *container,
			      u64 allocation_id);
int vtxfs_init_ipath(struct vtxfs_inode_path *ipath);
int vtxfs_find_all_leafs(struct vtxfs_root *root, u64 allocation_id,
			 struct list_head *out);
void vtxfs_free_path_records(struct list_head *list);
int vtxfs_free_pref(struct vtxfs_backref_context *ctx,
		    const struct vtxfs_prelim_ref *key);
int vtxfs_unode_aux_to_inode_list(struct vtxfs_backref_context *ctx,
				  struct list_head *out);
int vtxfs_add_all_parents(struct vtxfs_root *root, u64 ino,
			  vtxfs_backref_visit_t cb, void *private);
int vtxfs_ref_to_path(struct vtxfs_root *root, u64 ino, char **path_out);
int vtxfs_tree_backref_for_extent(struct vtxfs_fs_info *fs,
				  u64 allocation_id, struct vtx_ref_item *out);

/* The following Btrfs constructs do not have VTXFS v2.1 disk semantics:
 * signatures are intentionally explicit and return -EOPNOTSUPP.
 * No synthetic ownership chains may be used for freeing blocks. */
int vtxfs_add_delayed_refs(struct vtxfs_backref_context *ctx,
			    struct vtxfs_trans *trans);
int vtxfs_find_all_roots_safe(struct vtxfs_fs_info *fs,
			      u64 allocation_id, struct list_head *out);
int vtxfs_find_one_extref(struct vtxfs_root *root, u64 ino);
int vtxfs_handle_direct_tree_backref(struct vtxfs_backref_context *ctx);
int vtxfs_handle_indirect_tree_backref(struct vtxfs_backref_context *ctx);
int vtxfs_backref_add_tree_node(struct vtxfs_backref_context *ctx);
int vtxfs_backref_finish_upper_links(struct vtxfs_backref_context *ctx);
void vtxfs_backref_error_cleanup(struct vtxfs_backref_context *ctx);
int vtxfs_backref_init_cache(struct vtxfs_backref_context *ctx);
int vtxfs_backref_alloc_node(struct vtxfs_backref_context *ctx,
                             u64 node_bytenr);
int vtxfs_backref_free_node(struct vtxfs_backref_context *ctx,
                            u64 node_bytenr);
int vtxfs_backref_drop_node(struct vtxfs_backref_context *ctx,
                            u64 node_bytenr);
int vtxfs_backref_cleanup_node(struct vtxfs_backref_context *ctx,
                               u64 node_bytenr);

#endif /* VTXFS_BACKREF_H */
