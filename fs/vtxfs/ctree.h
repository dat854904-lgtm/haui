/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VTXFS_CTREE_H
#define _VTXFS_CTREE_H

#include <linux/bio.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

/* ---- Hook cấp phát VBLK (được nối với VAG allocator khi tích hợp) ---- */
struct vtxfs_alloc_ops {
	int (*alloc_blocks)(struct vtxfs_fs_info *fs, u32 blocks,
			    u64 *start_vblk_out);
	int (*free_blocks)(struct vtxfs_fs_info *fs, u64 start_vblk,
			   u32 blocks);
};

/* ---- extent_io.c: extent buffer ---- */
struct vtxfs_extent_buffer *vtxfs_eb_alloc(struct vtxfs_fs_info *fs,
					   u64 vblk, u32 node_size);
struct vtxfs_extent_buffer *vtxfs_find_eb(struct vtxfs_fs_info *fs, u64 vblk);
void vtxfs_eb_get(struct vtxfs_extent_buffer *eb);
void vtxfs_eb_put(struct vtxfs_extent_buffer *eb);
int vtxfs_read_tree_block(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
			  u64 oid, u64 vblk, u8 level,
			  struct vtxfs_extent_buffer **eb_out);
void vtxfs_mark_eb_dirty(struct vtxfs_extent_buffer *eb);
static inline int eb_nritems(const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	return vtx_cpu16(eb->hdr->nitems);
}
static inline int eb_level(const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	return eb->hdr->level;
}
static inline void eb_key(const struct vtxfs_extent_buffer *eb, int slot,
			  struct vtx_key *k)
{
	VTXFS_TRACE_ENTER();

	*k = eb->hdr->entries[slot].key;
}
static inline void *eb_val(const struct vtxfs_extent_buffer *eb, int slot)
{
	VTXFS_TRACE_ENTER();

	return (u8 *)eb->hdr +
		vtx_cpu16(eb->hdr->entries[slot].slot.val_off8) * 8U;
}
static inline u16 eb_val_len(const struct vtxfs_extent_buffer *eb, int slot)
{
	VTXFS_TRACE_ENTER();

	return vtx_cpu16(eb->hdr->entries[slot].slot.val_len);
}

#define VTXFS_LOCK_NONE  0
#define VTXFS_LOCK_READ  1
#define VTXFS_LOCK_WRITE 2

/* Public accessors for ctree_balance.c / ctree_locking.c / cpd.c / selftest */
int eb_nritems_pub(const struct vtxfs_extent_buffer *eb);
void eb_key_pub(const struct vtxfs_extent_buffer *eb, int slot,
		struct vtx_key *k);
void *eb_val_pub(const struct vtxfs_extent_buffer *eb, int slot);
u16 eb_val_len_pub(const struct vtxfs_extent_buffer *eb, int slot);
int eb_level_pub(const struct vtxfs_extent_buffer *eb);
int root_level_pub(struct vtxfs_root *root);
struct vtxfs_extent_buffer *root_node_get_pub(struct vtxfs_root *root);
int root_is_physical_pub(const struct vtxfs_root *root);
int bin_search_pub(const struct vtxfs_extent_buffer *eb,
		   const struct vtx_key *key, int *slot);
void fix_sep_pub(struct vtxfs_extent_buffer *parent, int slot,
		 struct vtxfs_extent_buffer *child);
int read_child_block_pub(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
			 struct vtxfs_extent_buffer *parent, int slot,
			 struct vtxfs_extent_buffer **out);
int alloc_tree_block_pub(struct vtxfs_root *root, u8 level, u64 oid, int is_root,
			 struct vtxfs_extent_buffer **out);

int vtxfs_write_dirty_ebs(struct vtxfs_fs_info *fs);
int vtxfs_write_eb(struct vtxfs_fs_info *fs, struct vtxfs_extent_buffer *eb);

/* extent_state io_tree (delalloc/lock) */
int vtxfs_io_tree_init(struct vtxfs_extent_io_tree *tree,
		       struct vtxfs_inode *inode);
void vtxfs_io_tree_destroy(struct vtxfs_extent_io_tree *tree);
int vtxfs_set_extent_bit(struct vtxfs_extent_io_tree *tree,
			 u64 start, u64 end, u32 bits);
int vtxfs_clear_extent_bit(struct vtxfs_extent_io_tree *tree,
			   u64 start, u64 end, u32 bits);
int vtxfs_lock_extent(struct vtxfs_extent_io_tree *tree, u64 start, u64 end);
void vtxfs_unlock_extent(struct vtxfs_extent_io_tree *tree,
			 u64 start, u64 end);

/* extent_map */
int vtxfs_add_extent_mapping(struct vtxfs_extent_map_tree *tree,
			     struct vtxfs_extent_map *em);
struct vtxfs_extent_map *vtxfs_lookup_extent_mapping(
		struct vtxfs_extent_map_tree *tree, u64 start, u64 len);
void vtxfs_free_extent_map(struct vtxfs_extent_map *em);
void vtxfs_extent_map_tree_destroy(struct vtxfs_extent_map_tree *tree);

/* ---- extent-tree.c ---- */
int vtxfs_alloc_tree_block(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
			   u8 level, u64 owner_oid,
			   struct vtxfs_extent_buffer **eb_out);
int vtxfs_alloc_extent(struct vtxfs_fs_info *fs, u32 blocks,
		       u64 *start_vblk, u64 *allocation_id);
int vtxfs_add_extent_ref(struct vtxfs_fs_info *fs, u64 allocation_id,
			 u64 start_vblk, u32 blocks, u32 flags, u32 owner);
int vtxfs_drop_extent_ref(struct vtxfs_fs_info *fs, u64 allocation_id,
			  u64 start_vblk, u32 blocks);
int vtxfs_lookup_extent_ref(struct vtxfs_fs_info *fs, u64 start_vblk,
			    struct vtx_ref_item *out);
int vtxfs_pin_extent(struct vtxfs_fs_info *fs, u64 start_vblk, u32 blocks);
int vtxfs_sfq_promote(struct vtxfs_fs_info *fs, u64 safe_xid, u32 *promoted_out);

/* ---- ctree.c: path ---- */
struct vtxfs_path *vtxfs_alloc_path(void);
void vtxfs_free_path(struct vtxfs_path *path);
void vtxfs_release_path(struct vtxfs_path *path);

/* ---- ctree.c: search ---- */
int vtxfs_search_slot(struct vtxfs_root *root, struct vtxfs_path *path,
		      const struct vtx_key *key, int ins_len, int cow);
int vtxfs_search_old_slot(struct vtxfs_root *root, struct vtxfs_path *path,
			  const struct vtx_key *key, int ins_len);
int vtxfs_search_slot_for_read(struct vtxfs_root *root,
			       struct vtxfs_path *path,
			       const struct vtx_key *find_higher,
			       int return_any);
int vtxfs_search_forward(struct vtxfs_root *root, struct vtxfs_path *path,
			 u64 *min_oid, u8 *min_type, u64 *min_offset);
int vtxfs_find_item(struct vtxfs_root *root, const struct vtx_key *key,
		    struct vtxfs_path **path_out);
int vtxfs_next_key(struct vtxfs_root *root, struct vtxfs_path *path);
int vtxfs_previous_item(struct vtxfs_root *root, struct vtxfs_path *path,
			u64 min_oid, int type_filter);
int vtxfs_previous_extent_item(struct vtxfs_root *root,
			       struct vtxfs_path *path, u64 min_objectid);

/* ---- ctree.c: item / pointer mutation ---- */
int vtxfs_insert_item(struct vtxfs_root *root, struct vtxfs_path *path,
		      const struct vtx_key *key, u32 data_size,
		      void **write_ptr_out);
int vtxfs_split_item(struct vtxfs_root *root, struct vtxfs_path *path,
		     const struct vtx_key *new_key, u32 split_offset);
int vtxfs_truncate_item(struct vtxfs_path *path, u32 new_size, int from_end);
int vtxfs_extend_item(struct vtxfs_root *root, struct vtxfs_path *path,
		      u32 new_size);
int vtxfs_del_item(struct vtxfs_root *root, struct vtxfs_path *path);
int vtxfs_del_ptr(struct vtxfs_root *root, struct vtxfs_path *path,
		  int level, int slot);
int vtxfs_set_node_ptr(struct vtxfs_extent_buffer *parent, int slot,
		       const struct vtx_key *sep_key,
		       const struct vtx_key_ptr_val *ptr);

/* ---- ctree.c: CoW / root ---- */
bool vtxfs_block_can_be_shared(struct vtxfs_root *root,
			       struct vtxfs_extent_buffer *eb);
int vtxfs_cow_block(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
		    struct vtxfs_extent_buffer *eb,
		    struct vtxfs_extent_buffer *parent, int parent_slot,
		    struct vtxfs_extent_buffer **cow_ret);
int vtxfs_copy_root(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
		    struct vtxfs_extent_buffer **new_root_out);
int alloc_tree_block_for_test(struct vtxfs_root *root,
			      struct vtxfs_extent_buffer **out);


/* ---- inode-ext.c ---- */
int vtxfs_lookup_inode(struct vtxfs_root *root, u64 ino,
		       struct vtxfs_path *path);
int vtxfs_insert_inode(struct vtxfs_root *root, u64 ino,
		       const struct vtx_inode_item *item);
int vtxfs_insert_inode_ref(struct vtxfs_root *root, u64 ino, u64 dir_ino,
			   const char *name, u16 namelen);
int vtxfs_lookup_inode_ref(struct vtxfs_root *root, u64 ino, u64 dir_ino,
			   struct vtxfs_path *path);
bool can_cow_file_range_inline(struct inode *inode, u64 size);
int insert_inline_extent(struct inode *inode, struct vtxfs_path *path,
			u64 start, const void *data, size_t size);
bool inode_need_compress(struct inode *inode, u64 start, u64 end);

struct vtxfs_async_extent {
	struct list_head list;
	u64 start, end;
	struct inode *inode;
};
struct vtxfs_async_cow {
	struct list_head extents;
	struct work_struct work;
	struct vtxfs_root *root;
};
void add_async_extent(struct vtxfs_async_cow *async_cow,
		      u64 start, u64 end, struct inode *inode);
int vtxfs_run_delalloc_range(struct inode *inode, u64 start, u64 end);
int cow_file_range(struct inode *inode, u64 start, u64 end,
		   const u8 *data);

/* ---- ctree_balance.c ---- */
int vtxfs_balance_after_delete(struct vtxfs_root *root,
			       struct vtxfs_path *path, int level);
int vtxfs_contract_root(struct vtxfs_root *root, struct vtxfs_path *path);

/* ---- ctree_compat.c ---- */
void vtxfs_ctree_set_root_flag(struct vtxfs_extent_buffer *eb, int on);
int vtxfs_ctree_delayed_ref(struct vtxfs_fs_info *fs, u64 alloc_id,
			    u64 vblk, u32 blocks, u32 flags, s64 delta);

/* ---- omap.c ---- */
int vtxfs_omap_publish(struct vtxfs_root *omap_root, u64 child_oid, u64 vblk);
int vtxfs_omap_resolve_vblk(struct vtxfs_root *omap_root, u64 child_oid,
			    u64 *child_vblk);

/* ---- ramdisk.c ---- */
struct gendisk *vtxfs_ramdisk_create(u64 nr_fs_blocks);
void vtxfs_ramdisk_destroy(struct gendisk *disk);

/* ---- delalloc.c ---- */
int vtxfs_delalloc_reserve(struct inode *inode, u64 start, u64 end);

/* ---- ctree_locking.c ---- */
int vtxfs_search_slot_locked(struct vtxfs_root *root,
			     struct vtxfs_path *path,
			     const struct vtx_key *key, int ins_len, int cow);
void vtxfs_release_path_locked(struct vtxfs_path *path);

/* ---- ctree_balance.c insert balance ---- */
int vtxfs_balance_after_insert(struct vtxfs_root *root,
			       struct vtxfs_path *path);

/* ---- extent-item.c ---- */
int vtxfs_build_extent_item(struct vtxfs_root *root, u64 ino, u64 file_off,
			    u64 vblk, u64 alloc_id, u32 nblocks,
			    const __le32 *csums);
int insert_extent_item_crc(struct vtxfs_root *root, u64 ino, u64 file_off,
			   u64 vblk, u64 alloc_id, u32 nblocks,
			   const __le32 *csums);

#endif /* _VTXFS_CTREE_H */
