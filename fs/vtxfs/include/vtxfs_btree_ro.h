/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * P0 READ-ONLY B+Tree walker over already-verified immutable nodes.
 * The callbacks must verify BLAKE3, VAG bounds and allocation identity.
 * This module does not implement the OMAP tree itself, checkpoint or I/O.
 */
#ifndef _VTXFS_BTREE_RO_H
#define _VTXFS_BTREE_RO_H
#include "vtxfs_validate.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vtx_ro_ops {
    /* Returned pointer stays pinned until release_node; cannot be reused. */
    int (*read_node)(void *ctx, __u64 vblk, const void **data,
                     size_t *data_bytes, int *checksum_verified);
    void (*release_node)(void *ctx, const void *data);
    /* Mandatory for virtual child pointers, view-scoped and authoritative. */
    int (*omap_resolve)(void *ctx, __u64 view_id, __u64 child_oid,
                        __u64 *child_vblk);
};

struct vtx_ro_tree {
    const struct vtx_ro_ops *ops;
    void *ctx;
    __u64 view_id;
    __u64 root_oid, root_vblk, max_xid, owner_id;
    __u32 node_bytes;
    __u8 tree_type, root_level;
};

/* Returns 0 if key exists; -ENOENT if not; -ENOBUFS with *value_bytes set
 * to required buffer length; -EUCLEAN on malformed nodes/identities.
 * Never accepts hinted physical block location for virtual child pointers.
 * For zero-length values, NULL buffer is permitted.
 */
int vtx_ro_lookup(const struct vtx_ro_tree *tree,
                  const struct vtx_key *key,
                  void *value, size_t *value_bytes);
#ifdef __cplusplus
}
#endif
#endif
