/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VTXFS v2.1 multi-level Copy-on-Write (CoW) B+Tree engine.
 *
 * Supports both PHYSICAL_VBLK trees (OMAP trees) and VIRTUAL_OID trees
 * (system and subvolume trees resolved via an authoritative OMAP view).
 * Preserves all invariants required by vtx_ro_lookup:
 *   - Strict key ordering and non-overlapping slotted values
 *   - Parent separator keys equal child minimum keys (entries[0].key)
 *   - Non-empty non-root nodes and automatic root split / contraction
 */
#ifndef _VTXFS_BTREE_RW_H
#define _VTXFS_BTREE_RW_H

#include "vtxfs_validate.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vtx_rw_ops {
    int (*read_node)(void *ctx, __u64 vblk, const void **data,
                     size_t *data_bytes, int *checksum_verified);
    void (*release_node)(void *ctx, const void *data);

    /* Return writable buffer for (oid, old_vblk) at tx_xid, CoW'ing if needed. */
    int (*cow_node)(void *ctx, __u8 tree_type, __u8 level, __u64 oid,
                    __u64 old_vblk, __u64 *new_vblk, void **writable_data);

    /* Allocate a brand-new node (new oid and new vblk) at tx_xid. */
    int (*alloc_node)(void *ctx, __u8 tree_type, __u8 level,
                      __u64 *new_oid, __u64 *new_vblk, void **writable_data);

    /* Retire a node whose contents were merged or whose root contracted. */
    int (*free_node)(void *ctx, __u8 tree_type, __u8 level,
                     __u64 oid, __u64 vblk);

    /* Required when is_physical == 0: authoritative view-scoped OMAP hooks. */
    int (*omap_resolve)(void *ctx, __u64 view_id, __u64 child_oid,
                        __u64 *child_vblk);
    int (*omap_set)(void *ctx, __u64 view_id, __u64 child_oid,
                    __u64 child_vblk);
    int (*omap_del)(void *ctx, __u64 view_id, __u64 child_oid);
};

struct vtx_rw_tree {
    const struct vtx_rw_ops *ops;
    void *ctx;
    void *scratch;         /* Caller scratch buffer >= 3 * node_bytes */
    size_t scratch_bytes;
    __u64 view_id;
    __u64 root_oid;
    __u64 root_vblk;
    __u64 tx_xid;
    __u64 owner_id;
    __u32 node_bytes;
    __u8 tree_type;
    __u8 root_level;
    __u8 is_physical;      /* 1 = PHYSICAL_VBLK, 0 = VIRTUAL_OID */
};

int vtx_rw_lookup(const struct vtx_rw_tree *tree,
                  const struct vtx_key *key,
                  void *value, size_t *value_bytes);

/* Ordered forward cursor search: smallest key >= start_key (if inclusive != 0)
 * or > start_key (if inclusive == 0). Returns 0 on match, -ENOENT at end.
 */
int vtx_rw_next(const struct vtx_rw_tree *tree,
                const struct vtx_key *start_key, int inclusive,
                struct vtx_key *found_key,
                void *value, size_t *value_bytes);

int vtx_rw_insert(struct vtx_rw_tree *tree,
                  const struct vtx_key *key,
                  const void *value, __u16 value_bytes);

int vtx_rw_replace(struct vtx_rw_tree *tree,
                   const struct vtx_key *key,
                   const void *value, __u16 value_bytes);

int vtx_rw_upsert(struct vtx_rw_tree *tree,
                  const struct vtx_key *key,
                  const void *value, __u16 value_bytes);

int vtx_rw_delete(struct vtx_rw_tree *tree,
                  const struct vtx_key *key);

#ifdef __cplusplus
}
#endif
#endif /* _VTXFS_BTREE_RW_H */
