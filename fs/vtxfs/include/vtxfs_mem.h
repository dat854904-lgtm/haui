/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VTXFS v2.1 in-memory kernel structures.
 *
 * DESIGN SCAFFOLD, NOT A MOUNTABLE DRIVER.
 * All mutation, refcount, writeback and lock-order rules need executable
 * implementation and crash testing. These types are distinct from on-disk ABI.
 */
#ifndef _VTXFS_MEM_H
#define _VTXFS_MEM_H
#ifndef __KERNEL__
#error "vtxfs_mem.h is for kernel builds only"
#endif

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/rbtree.h>
#include <linux/xarray.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/refcount.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/byteorder/generic.h>
#include "vtxfs_validate.h"

#define VTXFS_MAX_LEVEL          8U
#define VTXFS_MAX_EXTENT_BLOCKS  256U
#define VTXFS_MAX_EXTENT_BYTES   ((u64)VTXFS_MAX_EXTENT_BLOCKS * VTXFS_BLOCK_SIZE)

/* Use these helpers instead of the truncating VTX_MAKE_OID_TYPE macro. */
static inline int vtxfs_mk_key(struct vtx_key *k, u64 oid, u8 type, u64 off)
{
    if (!k || !vtx_oid_valid(oid))
        return -ERANGE;
    k->oid_and_type = cpu_to_le64(VTX_MAKE_OID_TYPE(oid, type));
    k->offset = cpu_to_le64(off);
    return 0;
}
static inline u64 vtxfs_key_oid(const struct vtx_key *k)
{
    return VTX_KEY_GET_OID(le64_to_cpu(k->oid_and_type));
}
static inline u8 vtxfs_key_type(const struct vtx_key *k)
{
    return VTX_KEY_GET_TYPE(le64_to_cpu(k->oid_and_type));
}
static inline u64 vtxfs_key_off(const struct vtx_key *k)
{
    return le64_to_cpu(k->offset);
}

struct vtxfs_fs_info;
struct vtxfs_root;
struct vtxfs_inode;
struct vtxfs_trans;
struct vtxfs_vag_dir;
struct vtxfs_delayed_node;
struct vtxfs_lru_cache;
struct vtxfs_alloc_ops;

struct vtxfs_locking {
    struct rw_semaphore commit_sem;
    spinlock_t roots_lock;
};

/* Buffered node keyed by (checkpoint or view, VBLK, incarnation), not
 * bare VBLK across an allocation's lifetime. Reuse requires invalidation
 * and a proof that no RCU/pinned readers still hold stale cache records.
 */
enum vtxfs_eb_flags {
    VTXFS_EB_UPTODATE, VTXFS_EB_VERIFIED, VTXFS_EB_DIRTY,
    VTXFS_EB_WRITEBACK, VTXFS_EB_IOERR, VTXFS_EB_RETIRED,
    VTXFS_EB_PHYSICAL_CHILDREN
};
struct vtxfs_extent_buffer {
    struct vtxfs_fs_info *fs;
    u64 vblk, oid, allocation_id, birth_xid;
    u32 node_size;
    unsigned long flags;
    refcount_t refs;
    struct rw_semaphore lock;
    struct list_head lru, dirty_link;
    struct rcu_head rcu;
    struct vtx_node_hdr *hdr;
};

struct vtxfs_omap_view {
    struct vtxfs_fs_info *fs;
    struct vtxfs_extent_buffer *root;
    u64 root_vblk, committed_root_vblk, view_xid;
    struct xarray cache; /* entries owned by view; invalidate on mapping CoW */
    struct rw_semaphore lock;
    refcount_t refs;
};

enum vtxfs_root_flags {
    VTXFS_ROOT_DIRTY, VTXFS_ROOT_FORCE_COW, VTXFS_ROOT_RDONLY,
    VTXFS_ROOT_REAPING, VTXFS_ROOT_ORPHAN_CLEANUP
};
struct vtxfs_root {
    struct vtxfs_fs_info *fs;
    struct vtxfs_fs_info *fs_info;
    enum vtx_tree_type tree_type;
    u64 root_oid, subvol_id;
    struct vtxfs_extent_buffer __rcu *node;
    struct vtxfs_extent_buffer *committed_node;
    struct vtxfs_omap_view *view;
    struct vtxfs_root *resolve_root;  /* OMAP tree resolving VIRTUAL children of this root */
    struct vtx_subvol_root_item on_disk;
    struct vtx_subvol_root_item item;
    struct mutex op_mutex;
    unsigned long state;
    struct xarray inodes, delayed_nodes;
    atomic64_t next_ino;
    struct list_head dirty_link;
    u64 last_trans_xid;
    refcount_t refs;
    struct rcu_head rcu;
};

/* Mutating a reference item and adding a deferred-free record MUST be
 * atomically recoverable in the same CoW transaction.
 * A net ref delta is NOT an ownership proof for shared-root snapshots.
 */
struct vtxfs_delayed_ref {
    struct rb_node rb;
    u64 allocation_id, start_vblk;
    u32 blocks, flags;
    s64 delta;
};

enum vtxfs_trans_phase {
    VTXFS_TRANS_OPEN, VTXFS_TRANS_QUIESCING, VTXFS_TRANS_WRITING,
    VTXFS_TRANS_PUBLISHING, VTXFS_TRANS_COMMITTED, VTXFS_TRANS_ABORTED
};
struct vtxfs_trans {
    struct vtxfs_fs_info *fs;
    u64 xid;
    enum vtxfs_trans_phase phase;
    refcount_t joiners;
    struct list_head dirty_roots, dirty_ebs, retired_ebs;
    struct rb_root delayed_refs;
    spinlock_t delayed_ref_lock;
    atomic64_t bytes_reserved;
};

/* Commit serialization MUST quiesce joining writers, wait ordered I/O and
 * flush data+metadata dependencies before publishing a verified CPD.
 */
struct vtxfs_fs_info {
    struct super_block *sb;
    struct block_device *bdev; /* P0 single device only */
    struct vtx_super_header super;
    struct vtx_checkpoint_header cp;
    u64 current_xid, committed_xid;
    u32 node_size;
    u8 metadata_uuid[16];
    struct xarray eb_cache, roots;
    struct vtxfs_vag_dir *vag;
    struct vtxfs_root *omap_meta_root, *root_tree, *ref_root,
                      *sfq_root, *reaper_root, *uuid_root;
    struct mutex trans_lock;
    struct rw_semaphore commit_sem;
    struct vtxfs_trans *running;
    atomic64_t next_object_id;
    /* P0.6 / P1 kernel extensions */
    struct vtxfs_locking locking;
    struct xarray eb_index;
    struct vtxfs_lru_cache *eb_lru;
    struct mutex eb_mutex;
    spinlock_t eb_lock;
    struct list_head dirty_ebs;
    const struct vtxfs_alloc_ops *alloc_ops;
    void *alloc_priv;
    struct workqueue_struct *async_cow_wq;
    struct list_head ordered_extents;
    spinlock_t ordered_lock;
    atomic_t ordered_count;
    void *reaper_priv;
    void *sysfs_priv;
    void *scrub_priv;
    struct rb_root free_space_tree;
    spinlock_t free_space_lock;
    bool shadow_only; /* engine owns all writes to this mounted image */
    int (*read_block_fn)(struct vtxfs_fs_info *fs, u64 pblk, void *buf);
};

struct vtxfs_path {
    struct vtxfs_extent_buffer *nodes[VTXFS_MAX_LEVEL];
    int slots[VTXFS_MAX_LEVEL];
    u8 locks[VTXFS_MAX_LEVEL];
    u8 lowest_level;
    bool keep_locks, search_committed_root, for_cow;
};

/* All keys must be strictly sorted according to vtx_key_cmp. */
struct vtxfs_item_batch {
    const struct vtx_key *keys;
    const u32 *value_lengths;
    u32 total_padded_bytes;
    int count;
};

#define VTXFS_EXT_DIRTY      (1U << 0)
#define VTXFS_EXT_UPTODATE   (1U << 1)
#define VTXFS_EXT_LOCKED     (1U << 2)
#define VTXFS_EXT_DELALLOC   (1U << 3)
#define VTXFS_EXT_NEW        (1U << 4)
#define VTXFS_EXT_WRITEBACK  (1U << 5)
#define VTXFS_EXT_CSUM_OK    (1U << 6)
#define VTXFS_EXT_BOUNDARY   (1U << 7)

struct vtxfs_extent_state {
    struct rb_node rb;
    u64 start, end; /* inclusive range; reject end < start */
    u32 bits;
    refcount_t refs;
    wait_queue_head_t wq;
};
struct vtxfs_extent_io_tree {
    struct rb_root states;
    spinlock_t lock;
    struct vtxfs_inode *inode;
    u64 dirty_bytes;
    wait_queue_head_t lock_wq;
};
struct vtxfs_range { u64 start, len; };
struct vtxfs_extent_changeset {
    u64 bytes_changed;
    struct vtxfs_range *ranges;
    u32 count, capacity;
};

/* File offset -> virtual allocation mapping: a non-authoritative cache.
 * The tree extent item is the source of truth and includes checksums.
 * Pinned mappings cannot be replaced while their ordered I/O is active.
 */
#define VTXFS_EM_HOLE       (~0ULL)
#define VTXFS_EM_INLINE     (~0ULL - 1ULL)
#define VTXFS_EM_DELALLOC   (~0ULL - 2ULL)
#define VTXFS_EM_PINNED     (1U << 0)
#define VTXFS_EM_SHARED     (1U << 1)
struct vtxfs_extent_map {
    struct rb_node rb;
    u64 start, len, vblk, allocation_id, birth_xid;
    u32 slice_offset_blocks, flags, checksum_count;
    __le32 *checksums;
    refcount_t refs;
};
struct vtxfs_extent_map_tree {
    struct rb_root mappings;
    rwlock_t lock;
};

enum vtxfs_inode_flags {
    VTXFS_INODE_ORPHAN, VTXFS_INODE_NEEDS_SYNC,
    VTXFS_INODE_DIRSEQ_VALID
};
struct vtxfs_inode {
    struct vtxfs_root *root;
    u64 vtx_ino, ino, disk_i_size, generation, last_xid, sync_xid, next_dir_seq;
    struct vtx_inode_item disk;
    unsigned long runtime_flags;
    struct rw_semaphore mmap_sem;
    struct mutex log_mutex;
    spinlock_t lock;
    u64 outstanding_extents, reserved_bytes;
    struct vtxfs_extent_io_tree io_tree, file_extent_tree;
    struct vtxfs_extent_map_tree extent_map_tree;
    struct vtxfs_delayed_node *delayed_node;
    struct inode vfs_inode; /* containing_inode() via container_of */
};

enum vtxfs_delayed_type {
    VTXFS_DELAYED_INSERT = 1, VTXFS_DELAYED_DELETE = 2
};
struct vtxfs_delayed_item {
    struct rb_node rb;
    struct list_head link;
    struct vtx_key key;
    enum vtxfs_delayed_type type;
    u32 data_len, bytes_reserved;
    refcount_t refs;
    u8 data[];
};
struct vtxfs_delayed_node {
    struct vtxfs_root *root;
    u64 inode_id;
    refcount_t refs;
    struct mutex mutex;
    struct rb_root insertions, deletions;
    struct list_head pending_link;
    u32 insert_count, delete_count;
    u64 bytes_reserved;
    bool inode_dirty;
    struct vtx_inode_item inode_snapshot;
};

struct vtxfs_replace_extent {
    u64 file_offset, vblk, allocation_id;
    u32 slice_offset_blocks, blocks, logical_bytes;
    bool new_allocation;
    const __le32 *checksums;
};
struct vtxfs_drop_extents_args {
    struct vtxfs_path *path;
    u64 start, end; /* half-open [start,end), checked for overflow */
    u64 bytes_found;
    struct vtxfs_replace_extent *replacement;
    bool drop_cache, replacement_inserted;
    u32 refs_dropped, refs_added;
    struct vtx_key resume_cursor;
};
struct vtxfs_truncate_control {
    struct vtxfs_inode *inode;
    u64 new_size, last_size, released_bytes;
    u32 max_items_per_transaction, items_processed;
    struct vtx_key resume_cursor; /* must persist with Reaper job */
};
#endif /* _VTXFS_MEM_H */
