/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VTXFS_SUBSYS_H
#define _VTXFS_SUBSYS_H

#include "ctree.h"

/* ---- ordered-extent.c ---- */
enum { OE_IO_DONE = 0, OE_FINISHED = 1 };

struct vtxfs_ordered_extent {
	struct list_head list;
	refcount_t refs;
	struct inode *inode;
	u64 file_offset, len;
	u64 vblk, allocation_id;
	u32 blocks;
	unsigned long flags;	/* OE_IO_DONE, OE_FINISHED */
	struct completion done;
};
struct vtxfs_ordered_extent *vtxfs_alloc_ordered(struct inode *inode,
		u64 file_offset, u64 len, u64 vblk, u64 alloc_id, u32 blocks);
void vtxfs_put_ordered(struct vtxfs_ordered_extent *oe);
void vtxfs_ordered_set_io_done(struct vtxfs_ordered_extent *oe);
void vtxfs_ordered_finish(struct vtxfs_ordered_extent *oe);
void vtxfs_ordered_complete(struct vtxfs_ordered_extent *oe);
int vtxfs_wait_ordered_all(struct vtxfs_fs_info *fs);
int vtxfs_submit_ordered_data(struct vtxfs_fs_info *fs, u64 pblk,
			      struct page **pages, unsigned int npages,
			      struct vtxfs_ordered_extent *oe);
int vtxfs_flush_all_delalloc(struct vtxfs_fs_info *fs);

/* ---- mount-native.c ---- */
int vtxfs_mount_native(struct super_block *sb);
void vtxfs_mount_native_teardown(struct super_block *sb);

/* ---- trans.c ---- */
struct vtxfs_trans *vtxfs_start_trans(struct vtxfs_fs_info *fs);
int vtxfs_trans_join(struct vtxfs_trans *trans);
void vtxfs_add_dirty_root(struct vtxfs_trans *trans, struct vtxfs_root *root);
int vtxfs_run_delayed_refs(struct vtxfs_trans *trans);
int vtxfs_commit_transaction(struct vtxfs_fs_info *fs,
			     struct vtxfs_trans *trans);
int vtxfs_end_trans(struct vtxfs_fs_info *fs, struct vtxfs_trans *trans);
int vtxfs_publish_cpd(struct vtxfs_fs_info *fs, u64 xid);
int vtxfs_sfq_promote(struct vtxfs_fs_info *fs, u64 safe_xid, u32 *promoted_out);


/* ---- delayed-inode.c ---- */
struct vtxfs_delayed_node *vtxfs_delayed_node_get(struct vtxfs_root *root,
						  u64 ino);
void vtxfs_delayed_node_put(struct vtxfs_delayed_node *node);
int vtxfs_delayed_insert_dir_item(struct vtxfs_delayed_node *node,
				  const struct vtx_key *key,
				  const void *data, u32 len);
int vtxfs_delayed_delete_dir_item(struct vtxfs_delayed_node *node,
				  const struct vtx_key *key);
int vtxfs_delayed_flush_node(struct vtxfs_delayed_node *node);
int vtxfs_delayed_flush_all(struct vtxfs_root *root);

/* ---- async-thread.c ---- */
int vtxfs_async_init(struct vtxfs_fs_info *fs);
void vtxfs_async_stop(struct vtxfs_fs_info *fs);
int vtxfs_submit_async_cow(struct vtxfs_fs_info *fs, struct inode *inode,
			   u64 start, u64 end);

/* ---- compression.c ---- */
enum vtx_compr_type vtxfs_select_compression(struct inode *inode);
bool vtxfs_compress_extent(struct inode *inode, u64 start, u64 len);

/* ---- uuid-tree.c ---- */
int vtxfs_uuid_insert(struct vtxfs_fs_info *fs, const u8 uuid[16],
		      u64 subvol_id, u32 kind);
int vtxfs_uuid_lookup(struct vtxfs_fs_info *fs, const u8 uuid[16],
		      u64 *subvol_id);
int vtxfs_uuid_delete(struct vtxfs_fs_info *fs, const u8 uuid[16],
		      u64 subvol_id);

/* ---- orphan.c ---- */
int vtxfs_orphan_add(struct vtxfs_root *root, u64 ino);
int vtxfs_orphan_del(struct vtxfs_root *root, u64 ino);
int vtxfs_orphan_cleanup(struct vtxfs_root *root);

/* ---- backref.c ---- */
struct vtxfs_backref {
	struct list_head list;
	u64 owner_subvol, owner_ino, file_offset, allocation_id;
	u32 type;
};
int vtxfs_find_extent_backrefs(struct vtxfs_fs_info *fs, u64 allocation_id,
			       struct list_head *out);
void vtxfs_free_backrefs(struct list_head *list);
#include "vtxfs_backref.h"

/* ---- print-tree.c ---- */
void vtxfs_print_leaf(struct vtxfs_extent_buffer *eb);
void vtxfs_print_node(struct vtxfs_extent_buffer *eb);
void vtxfs_print_root(struct vtxfs_root *root);

/* ---- reaper.c (kernel worker) ---- */
int vtxfs_reaper_start(struct vtxfs_fs_info *fs);
void vtxfs_reaper_stop(struct vtxfs_fs_info *fs);

/* ---- sysfs.c ---- */
int vtxfs_sysfs_init(struct vtxfs_fs_info *fs);
void vtxfs_sysfs_exit(struct vtxfs_fs_info *fs);

/* ---- free-space-cache.c ---- */
int vtxfs_free_space_load(struct vtxfs_fs_info *fs);
void vtxfs_free_space_invalidate(struct vtxfs_fs_info *fs);
int vtxfs_free_space_add(struct vtxfs_fs_info *fs, u64 start_vblk,
			 u64 blocks);
int vtxfs_free_space_remove(struct vtxfs_fs_info *fs, u64 start_vblk,
			    u64 blocks);
int vtxfs_free_space_find(struct vtxfs_fs_info *fs, u64 blocks,
			  u64 *start_vblk);

/* ---- omap.c ---- */
int vtxfs_omap_view_resolve(struct vtxfs_omap_view *view, u64 child_oid,
			    u64 *child_vblk);
int vtxfs_omap_view_set(struct vtxfs_omap_view *view, u64 child_oid,
			u64 child_vblk);
int vtxfs_omap_view_invalidate(struct vtxfs_omap_view *view, u64 child_oid);
struct vtxfs_omap_view *vtxfs_omap_view_alloc(struct vtxfs_fs_info *fs,
					      struct vtxfs_extent_buffer *root,
					      u64 view_xid);
void vtxfs_omap_view_put(struct vtxfs_omap_view *view);

/* ---- mount-native.c ---- */
int vtxfs_mount_native(struct super_block *sb);

/* ---- truncate.c ---- */
int vtxfs_drop_extents(struct vtxfs_root *root,
		       struct vtxfs_drop_extents_args *args);
int vtxfs_truncate_inode(struct vtxfs_root *root, struct vtxfs_inode *vinode,
			 u64 new_size);

/* ---- scrub.c ---- */
struct vtxfs_scrub_ctx {
	struct vtxfs_fs_info *fs;
	struct task_struct *task;
	struct vtx_scrub_state state;
};

int scrub_verify_extent(struct vtxfs_scrub_ctx *sc,
			const struct vtx_extent_item *ext);
int vtxfs_scrub_verify_extent(struct vtxfs_scrub_ctx *sc,
			      const struct vtx_extent_item *ext);
int vtxfs_scrub_start(struct vtxfs_fs_info *fs);
void vtxfs_scrub_stop(struct vtxfs_fs_info *fs);
int vtxfs_scrub_status(struct vtxfs_fs_info *fs, struct vtx_scrub_state *out);

/* ---- selftest.c ---- */
int vtxfs_run_selftest(void);

#endif /* _VTXFS_SUBSYS_H */



