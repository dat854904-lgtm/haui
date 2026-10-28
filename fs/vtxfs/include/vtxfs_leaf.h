/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Portable, in-memory VTXFS v2.1 slotted B+Tree LEAF operations.
 *
 * NOT an on-disk transaction manager. Callers must supply PRIVATE writable
 * buffers, reserve allocation/metadata, and perform CoW + atomic checkpoint.
 *
 * All functions reject corrupt node geometry/slots before mutation. The
 * scratch buffer must be separate, non-overlapping and at least node_size.
 * Writes are all-or-nothing with respect to the in-memory node. Metadata
 * checksum is cleared on change and must be regenerated before disk I/O.
 */
#ifndef _VTXFS_LEAF_H
#define _VTXFS_LEAF_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif
#include "vtxfs_validate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Key fields in arguments must already be little-endian encoded. */
int vtx_leaf_init(void *node, size_t node_bytes, __u64 oid, __u64 xid,
                  __u64 vblk, __u8 tree_type, __u16 flags);

/* Return 0 if key exists, 1 if absent, negative errno on corrupt input. */
int vtx_leaf_find(const void *node, size_t node_bytes,
                  const struct vtx_key *key, __u32 *slot);

/* Insert/replace/delete and compact a verified, private leaf.
 * - insert: -EEXIST for duplicate, -ENOSPC when full
 * - replace: -ENOENT for missing key
 * - delete: -ENOENT for missing key
 * - compact: remove holes/dead byte accounting without logical changes
 * Zero-length values are valid; their offsets have no allocated range.
 */
int vtx_leaf_insert(void *node, size_t node_bytes, const struct vtx_key *key,
                    const void *value, __u16 value_bytes,
                    void *scratch, size_t scratch_bytes);
int vtx_leaf_replace(void *node, size_t node_bytes, const struct vtx_key *key,
                     const void *value, __u16 value_bytes,
                     void *scratch, size_t scratch_bytes);
int vtx_leaf_delete(void *node, size_t node_bytes, const struct vtx_key *key,
                    void *scratch, size_t scratch_bytes);
int vtx_leaf_compact(void *node, size_t node_bytes,
                     void *scratch, size_t scratch_bytes);

/* Two-node operations preserve each destination node's own OID, VBLK,
 * owner, XID, level and addressing flags. Caller holds exclusive ownership
 * and updates parent/OMAP/ref accounting in the same CoW transaction.
 * Roots cannot be balanced here. Both inputs remain unchanged on error.
 * Scratch requires 2*node_size disjoint bytes, no heap allocation.
 */
int vtx_leaf_split(void *left, void *right, size_t node_size,
                   void *scratch, size_t scratch_bytes);
int vtx_leaf_split_insert(void *left, void *right, size_t node_size,
                          const struct vtx_key *key, const void *value,
                          __u16 value_bytes, void *scratch, size_t scratch_bytes);
int vtx_leaf_merge(void *left, void *right, size_t node_size,
                   void *scratch, size_t scratch_bytes);
int vtx_leaf_rebalance(void *left, void *right, size_t node_size,
                       void *scratch, size_t scratch_bytes);

#ifdef __cplusplus
}
#endif
#endif /* _VTXFS_LEAF_H */
