/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VTXFS v2.1 P0 Complete Storage, Allocator, CoW Transaction, Recovery,
 * Ownership/SFQ/Reaper, POSIX/Subvolume/Snapshot/Reflink, and Fsck Engine.
 *
 * Dual-mode portable C (compiles in both Linux kernel kbuild and userspace).
 */
#ifndef _VTXFS_ENGINE_H
#define _VTXFS_ENGINE_H

#include "vtxfs_csum.h"
#include "vtxfs_leaf.h"
#include "vtxfs_internal.h"
#include "vtxfs_btree_ro.h"
#include "vtxfs_btree_rw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * 1. Block Device Abstraction & Fault Injection
 * ------------------------------------------------------------------------- */

enum vtx_io_fault_stage {
    VTX_FAULT_NONE          = 0,
    VTX_FAULT_DATA_WRITE    = 1,
    VTX_FAULT_DATA_FLUSH    = 2,
    VTX_FAULT_META_WRITE    = 3,
    VTX_FAULT_VAG_WRITE     = 4,
    VTX_FAULT_META_FLUSH    = 5,
    VTX_FAULT_CPD_PRIMARY   = 6,
    VTX_FAULT_CPD_MIRROR    = 7
};

struct vtx_bdev;

struct vtx_bdev_ops {
    int (*read_blocks)(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                       void *buf);
    int (*write_blocks)(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                        const void *buf);
    int (*write_fua)(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                     const void *buf);
    int (*flush)(struct vtx_bdev *bdev);
};

struct vtx_bdev {
    const struct vtx_bdev_ops *ops;
    void *priv;
    __u64 total_blocks;
    __u32 fault_stage;      /* enum vtx_io_fault_stage to fail with -EIO */
    __u32 fault_triggered;
    __u64 reads_issued;
    __u64 writes_issued;
    __u64 flushes_issued;
    __u64 fuas_issued;
};

/* Sparse in-memory block device (works in both kernel and userspace). */
int vtx_ram_bdev_init(struct vtx_bdev *bdev, __u64 total_blocks);
void vtx_ram_bdev_destroy(struct vtx_bdev *bdev);

#ifndef __KERNEL__
/* File-backed block device for mkfs.vtxfs, fsck.vtxfs and vtxfs-inspect. */
int vtx_file_bdev_open(struct vtx_bdev *bdev, const char *path,
                       int create_or_rw, __u64 create_blocks);
void vtx_file_bdev_close(struct vtx_bdev *bdev);
#endif

/* Portable wall-clock source: ns since Unix epoch. May return 0 if the
 * host clock is unavailable; callers treat 0 as "unknown time". */
__u64 vtx_time_now_ns(void);

/* -------------------------------------------------------------------------
 * 2. VAG Two-Level Directory and Physical Block Allocator
 * ------------------------------------------------------------------------- */

#define VTX_MAX_SUPPORTED_VAGS 256U
#define VTX_IP_SLOT_STRIDE     512ULL

struct vtx_vag_state {
    struct vtx_vag_dir_root root;
    struct vtx_vag_desc descs[VTXFS_VAG_LEAF_ENTRIES];
    __u8 *bitmaps[VTX_MAX_SUPPORTED_VAGS]; /* 4096 bytes each */
    __u32 total_vags;
    __u32 alloc_cursor[VTX_MAX_SUPPORTED_VAGS];
    /*
     * Free-run summary index (RAM only, NEVER persisted: the on-disk bitmap
     * stays authoritative). max_free_run[v] is an UPPER BOUND of the longest
     * run of free blocks in VAG v: it is exact after a rebuild, only grows on
     * frees (merged-run length), and is tightened again when a search fails.
     * It must never under-estimate, otherwise a VAG would be skipped wrongly.
     * Any code writing bitmaps directly MUST call vtx_vag_summary_invalidate().
     */
    __u32 max_free_run[VTX_MAX_SUPPORTED_VAGS];
    __u32 summary_ready;
};

int vtx_vag_init_empty(struct vtx_vag_state *vag, __u32 total_vags,
                       __u64 xid, const __u8 metadata_uuid[VTXFS_UUID_SIZE]);
void vtx_vag_destroy(struct vtx_vag_state *vag);

int vtx_vag_vblk_to_pblk(const struct vtx_vag_state *vag, __u64 vblk,
                         __u64 *pblk_out);
int vtx_vag_test_bit(const struct vtx_vag_state *vag, __u64 vblk);
int vtx_vag_mark_range(struct vtx_vag_state *vag, __u64 start_vblk,
                       __u32 count, int allocated);
int vtx_vag_alloc_range(struct vtx_vag_state *vag, __u32 count,
                        __u64 *start_vblk_out);

/* Free-run summary index API. */
void vtx_vag_summary_rebuild(struct vtx_vag_state *vag);
static inline void vtx_vag_summary_invalidate(struct vtx_vag_state *vag)
{
    vag->summary_ready = 0;
}
/* Exact statistics of one VAG: number of free runs and the longest one. */
int vtx_vag_free_stats(const struct vtx_vag_state *vag, __u32 vag_id,
                       __u32 *free_runs, __u32 *longest_run);

/* Persist/load VAG root, 64K leaf and bitmaps for CPD slot (xid % 8). */
int vtx_vag_write_slot(struct vtx_bdev *bdev, struct vtx_vag_state *vag,
                       __u64 xid, const __u8 metadata_uuid[VTXFS_UUID_SIZE],
                       struct vtx_phys_ref *ref_out);
int vtx_vag_load_from_ref(struct vtx_bdev *bdev, struct vtx_vag_state *vag,
                          const struct vtx_phys_ref *ref,
                          __u64 total_blocks,
                          const __u8 metadata_uuid[VTXFS_UUID_SIZE]);

/* -------------------------------------------------------------------------
 * 3. Core Filesystem Context & Transaction / Checkpoint Engine
 * ------------------------------------------------------------------------- */

#define VTX_MAX_CACHED_NODES   4096U
#define VTX_MAX_DELAYED_REFS   4096U
#define VTX_MAX_PENDING_DATA   4096U

#define VTX_NODE_HASH_BUCKETS    4096U
#define VTX_CACHE_INITIAL_NODES  4096U
#define VTX_CACHE_MAX_NODES      65536U

struct vtx_cached_node {
    __u64 vblk;
    __u64 oid;
    struct vtx_cached_node *hash_next;
    struct vtx_cached_node *lru_prev;
    struct vtx_cached_node *lru_next;
    __u8 tree_type;
    __u8 level;
    __u8 dirty;
    __u8 in_use;
    __u8 pinned; /* temporary reference: never evict this cache buffer */
    __u8 reserved[3];
    __u8 data[VTXFS_BLOCK_SIZE];
};

struct vtx_delayed_ref_entry {
    __u64 allocation_id;
    __u64 start_vblk;
    __u32 block_count;
    __u32 flags;
    __u32 owner_hint;
    __s32 delta; /* +1 or -1 */
};

struct vtx_pending_data_write {
    __u64 vblk;
    __u32 block_count;
    __u8 *data;
};

struct vtx_fs {
    struct vtx_bdev *bdev;
    struct vtx_super_block super_blk;
    struct vtx_dev_table dev_tbl;
    struct vtx_checkpoint_desc cp_blk;
    struct vtx_vag_state vag;

    int mounted;
    int read_only;
    int defer_unlink; /* mounted VFS: maintain orphan until evict_inode */
    int view_only;   /* đặt bởi vtx_subvol_switch_view(); chặn mọi đột biến */
    __u64 committed_xid;
    __u64 current_xid;
    __u64 next_oid;

    /* System tree mutable handles for current transaction. */
    struct vtx_rw_tree omap_meta_tree;
    struct vtx_rw_tree root_tree;
    struct vtx_rw_tree ref_tree;
    struct vtx_rw_tree reaper_tree;
    struct vtx_rw_tree sfq_tree;
    struct vtx_rw_tree uuid_tree;

    /* Active subvolume cache during operations. */
    __u64 active_subvol_id;
    struct vtx_subvol_root_item active_subvol;
    struct vtx_rw_tree active_omap_view;
    struct vtx_rw_tree active_fs_tree;

    __u8 scratch_sys[VTXFS_BLOCK_SIZE * 3U];
    __u8 scratch_omap[VTXFS_BLOCK_SIZE * 3U];
    __u8 scratch_view[VTXFS_BLOCK_SIZE * 3U];
    __u8 scratch_fs[VTXFS_BLOCK_SIZE * 3U];

    int draining_refs;
    __u32 num_delayed_refs;
    struct vtx_delayed_ref_entry delayed_refs[VTX_MAX_DELAYED_REFS];

    __u32 num_pending_data;
    struct vtx_pending_data_write pending_data[VTX_MAX_PENDING_DATA];

    /* Dynamic node cache */
    struct vtx_cached_node *node_hash[VTX_NODE_HASH_BUCKETS];
    struct vtx_cached_node *lru_head;
    struct vtx_cached_node *lru_tail;
    __u32 cache_count;
    __u32 cache_capacity;
    __u32 cache_max_capacity;
};

struct vtx_fs *vtx_fs_alloc(void);
void vtx_fs_free(struct vtx_fs *fs);
struct vtx_cached_node *vtx_find_cached_node(struct vtx_fs *fs, __u64 vblk);

int vtx_mkfs(struct vtx_bdev *bdev, const char *label,
             const __u8 fsid_uuid[VTXFS_UUID_SIZE]);
int vtx_mount(struct vtx_fs *fs, struct vtx_bdev *bdev, int force_ro);
int vtx_unmount(struct vtx_fs *fs);
int vtx_sync_checkpoint(struct vtx_fs *fs);

int vtx_migrate_enable_compression(struct vtx_fs *fs, __u8 algo);
int vtx_migrate_disable_compression(struct vtx_fs *fs);

int vtx_enqueue_delayed_ref(struct vtx_fs *fs, __u64 allocation_id,
                            __u64 start_vblk, __u32 block_count,
                            __u32 flags, __u32 owner_hint, __s32 delta);
int vtx_queue_data_write(struct vtx_fs *fs, __u64 vblk, __u32 blocks,
                         const void *data);

/* -------------------------------------------------------------------------
 * 4. Ownership, Safe-Free Queue (SFQ), Reaper & Bitmap Cache Rebuild
 * ------------------------------------------------------------------------- */

/* Promote SFQ entries with retired_xid <= safe_xid into reusable free blocks. */
int vtx_sfq_promote(struct vtx_fs *fs, __u64 safe_xid, __u32 *promoted_out);

/* Rebuild VAG bitmaps and free_blocks purely from authoritative REF + SFQ. */
int vtx_vag_rebuild_bitmaps(struct vtx_fs *fs);

/* Execute up to max_steps of pending Reaper jobs in a recoverable transaction. */
int vtx_reaper_run_steps(struct vtx_fs *fs, __u32 max_steps,
                         __u32 *steps_done_out, int *all_done_out);
int vtx_reaper_run_all(struct vtx_fs *fs);

/* -------------------------------------------------------------------------
 * 5. Subvolumes, Snapshots, POSIX File/Dir Operations, Xattr & Reflink
 * ------------------------------------------------------------------------- */

int vtx_subvol_switch(struct vtx_fs *fs, __u64 subvol_id);
int vtx_subvol_switch_view(struct vtx_fs *fs, __u64 subvol_id);
int vtx_subvol_create(struct vtx_fs *fs, __u64 parent_subvol_id,
                      const char *name, __u64 *new_subvol_id_out);
int vtx_snapshot_create(struct vtx_fs *fs, __u64 src_subvol_id,
                        const char *name, int read_only,
                        __u64 *snap_subvol_id_out);
int vtx_subvol_destroy(struct vtx_fs *fs, __u64 subvol_id);
int vtx_subvol_get_name(struct vtx_fs *fs, __u64 subvol_id,
			char *name_out, size_t name_buf_size);

int vtx_mkdir(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
              const char *name, __u32 mode, __u64 *ino_out);
int vtx_create(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
               const char *name, __u32 mode, __u64 *ino_out);
int vtx_symlink(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
                const char *name, const char *target, __u64 *ino_out);
int vtx_readlink(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 char *buf, size_t buf_size, size_t *len_out);

int vtx_lookup(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
               const char *name, __u64 *ino_out, __u8 *ftype_out);
int vtx_lookup_path(struct vtx_fs *fs, __u64 subvol_id, const char *path,
                    __u64 *ino_out, __u8 *ftype_out);
int vtx_stat(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
             struct vtx_inode_item *inode_out);
int vtx_chmod(struct vtx_fs *fs, __u64 subvol_id, __u64 ino, __u32 mode);
int vtx_chown(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
              __u32 uid, __u32 gid);
int vtx_utimes(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
               __u64 atime_ns, __u64 mtime_ns);

int vtx_write(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
              __u64 file_offset, const void *data, size_t len);
int vtx_read(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
             __u64 file_offset, void *buf, size_t len, size_t *bytes_read_out);
/* Eager zero-backed block allocation for mode-0 fallocate.
 * Align 4 KiB, at most 1 MiB/call; KEEP_SIZE/PUNCH_HOLE not supported.
 */
int vtx_fallocate_eager(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                        __u64 offset, __u64 len);
int vtx_truncate(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 __u64 new_size);

/* Delete deferred POSIX orphan after last VFS inode reference disappeared.
 * Safe to retry after errors; recovery scans these markers on next mount. */
int vtx_orphan_purge(struct vtx_fs *fs, __u64 subvol_id, __u64 ino);
int vtx_orphan_recover(struct vtx_fs *fs, __u64 subvol_id);
int vtx_unlink(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
               const char *name);
/* Actual shared inode: multiple hard-link names, single nlink and extents. */
int vtx_link(struct vtx_fs *fs, __u64 subvol_id, __u64 inode_ino,
             __u64 new_parent_ino, const char *new_name);
int vtx_rmdir(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
              const char *name);
int vtx_rename(struct vtx_fs *fs, __u64 subvol_id,
               __u64 old_parent_ino, const char *old_name,
               __u64 new_parent_ino, const char *new_name);

struct vtx_dirent {
    __u64 ino;
    __u64 dir_seq;
    __u8 file_type;
    char name[VTXFS_NAME_MAX + 1];
};

int vtx_readdir(struct vtx_fs *fs, __u64 subvol_id, __u64 dir_ino,
                __u64 *cookie_inout, struct vtx_dirent *entries,
                size_t max_entries, size_t *count_out);

int vtx_setxattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 const char *name, const void *value, size_t value_len);
int vtx_getxattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 const char *name, void *value, size_t buf_size,
                 size_t *value_len_out);
int vtx_removexattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                    const char *name);
int vtx_listxattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                  char *buf, size_t buf_size, size_t *len_out);
int vtx_mknod(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
              const char *name, __u32 mode, __u64 rdev, __u64 *ino_out);

/* Reflink (FICLONERANGE semantics): share block-aligned extents CoW. */
int vtx_reflink(struct vtx_fs *fs, __u64 subvol_id,
                __u64 src_ino, __u64 src_offset,
                __u64 dst_ino, __u64 dst_offset, __u64 byte_len);

/* Exchange two existing directory entries without replacing either inode. */
int vtx_rename_exchange(struct vtx_fs *fs, __u64 subvol_id,
                        __u64 old_parent_ino, const char *old_name,
                        __u64 new_parent_ino, const char *new_name);

/*
 * Standalone P2 data transforms.  These helpers intentionally do not enable
 * the corresponding on-disk feature bits: extent integration and recovery
 * policy remain separate protocols.  Buffers returned through output_out
 * must be released with vtx_buffer_free().
 */
int vtx_compress_extent(const void *input, __u32 input_len,
                        __u8 algorithm, __u8 level,
                        __u8 **output_out, __u32 *output_len_out);
int vtx_decompress_extent(const void *input, __u32 input_len,
                          __u8 **output_out, __u32 *output_len_out);
void vtx_buffer_free(void *buffer);

int vtx_encrypt_extent(__u8 enc_type, const __u8 key[32],
                       __u64 allocation_id, __u64 vblk, __u64 birth_xid,
                       const __u8 *plaintext, __u32 len,
                       __u8 *ciphertext, __u8 tag[16]);
int vtx_decrypt_extent(__u8 enc_type, const __u8 key[32],
                       __u64 allocation_id, __u64 vblk, __u64 birth_xid,
                       const __u8 *ciphertext, __u32 len,
                       __u8 *plaintext, const __u8 tag[16]);
int vtx_keybag_wrap_key(const __u8 master_key[32], const __u8 data_key[32],
                        const __u8 salt[16], __u8 wrapped_key[48],
                        __u32 *wrapped_len);
int vtx_keybag_unwrap_key(const __u8 master_key[32],
                          const __u8 wrapped_key[48], __u32 wrapped_len,
                          const __u8 salt[16], __u8 data_key[32]);

/* UAPI ioctl query and diagnostic handlers. */
int vtx_ioctl_get_abi(const struct vtx_fs *fs, struct vtx_ioctl_abi *arg);
int vtx_ioctl_get_capabilities(const struct vtx_fs *fs,
                               struct vtx_ioc_capabilities *arg);
int vtx_ioctl_omap_lookup(struct vtx_fs *fs, struct vtx_ioc_omap_lookup *arg);
int vtx_ioctl_scrub(struct vtx_fs *fs, struct vtx_ioc_scrub_args *arg);

/* -------------------------------------------------------------------------
 * 6. Independent Safe Parser / Fsck & Inspector
 * ------------------------------------------------------------------------- */

struct vtx_fsck_report {
    __u64 committed_xid;
    __u64 super_generation;
    __u32 total_vags;
    __u32 subvolumes_checked;
    __u64 btree_nodes_verified;
    __u64 data_extents_verified;
    __u64 ref_items_verified;
    __u64 sfq_entries_verified;
    __u64 reaper_jobs_checked;
    __u64 allocated_blocks_counted;
    __u32 errors_found;
    __u32 repairs_planned;
    __u32 repairs_performed;
};

int vtx_fsck(struct vtx_bdev *bdev, struct vtx_fsck_report *report_out);
int vtx_fsck_repair(struct vtx_bdev *bdev, int dry_run, struct vtx_fsck_report *report_out);

#ifdef __cplusplus
}
#endif
#endif /* _VTXFS_ENGINE_H */
