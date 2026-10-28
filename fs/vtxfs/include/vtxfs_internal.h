/* SPDX-License-Identifier: GPL-2.0-only */
/* In-memory INTERNAL slotted node operations, fixed 24-byte child pointers.
 * Caller owns private writable buffers, metadata reservations and CoW.
 * Parent separator propagation and checkpoint publish NOT implemented.
 */
#ifndef _VTXFS_INTERNAL_H
#define _VTXFS_INTERNAL_H

#include "vtxfs_validate.h"

#ifdef __cplusplus
extern "C" {
#endif
int vtx_internal_init(void *node, size_t bytes, __u64 oid, __u64 xid,
                      __u64 vblk, __u8 tree_type, __u16 flags, __u8 level);
int vtx_internal_find(const void *node, size_t bytes,
                      const struct vtx_key *key, __u32 *slot);
int vtx_internal_insert(void *node, size_t bytes, const struct vtx_key *key,
                        const struct vtx_key_ptr_val *child,
                        void *scratch, size_t scratch_bytes);
int vtx_internal_replace(void *node, size_t bytes, const struct vtx_key *key,
                         const struct vtx_key_ptr_val *child,
                         void *scratch, size_t scratch_bytes);
int vtx_internal_delete(void *node, size_t bytes, const struct vtx_key *key,
                        void *scratch, size_t scratch_bytes);
int vtx_internal_compact(void *node, size_t bytes,
                         void *scratch, size_t scratch_bytes);

/* Two-node internal operations preserve each node's OID, VBLK, owner, XID,
 * level and child-addressing flags. Roots cannot be balanced directly.
 * Scratch requires 2*node_size disjoint bytes; both inputs unchanged on error.
 */
int vtx_internal_split(void *left, void *right, size_t node_size,
                       void *scratch, size_t scratch_bytes);
int vtx_internal_split_insert(void *left, void *right, size_t node_size,
                              const struct vtx_key *key,
                              const struct vtx_key_ptr_val *child,
                              void *scratch, size_t scratch_bytes);
int vtx_internal_merge(void *left, void *right, size_t node_size,
                       void *scratch, size_t scratch_bytes);
int vtx_internal_rebalance(void *left, void *right, size_t node_size,
                           void *scratch, size_t scratch_bytes);
#ifdef __cplusplus
}
#endif
#endif

