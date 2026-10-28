/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VTXFS v2.1 portable checksum and hash primitives:
 *   - CRC32C (Castagnoli) for data extents, VAG leaves and VAG bitmaps
 *   - BLAKE3-256 for superblocks, checkpoint descriptors and device tables
 *   - Domain-separated BLAKE3-128(metadata_uuid || payload) for B+Tree nodes
 *     and VAG directory roots
 *   - SipHash-2-4 for keyed directory and xattr name hashing
 */
#ifndef _VTXFS_CSUM_H
#define _VTXFS_CSUM_H

#include "vtxfs_validate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CRC32C (Castagnoli, reflected polynomial 0x82F63B78). */
__u32 vtx_crc32c_update(__u32 crc, const void *data, size_t len);
__u32 vtx_crc32c(const void *data, size_t len);

/* Official BLAKE3-256 and domain-separated BLAKE3-128. */
void vtx_blake3_256(const void *data, size_t len, __u8 out[32]);
void vtx_blake3_128_domain(const __u8 domain_uuid[VTXFS_UUID_SIZE],
                           const void *data, size_t len, __u8 out[16]);

/* SipHash-2-4 with 128-bit little-endian key seed[16]. */
__u64 vtx_siphash24(const __u8 seed[16], const void *data, size_t len);
void vtx_dir_name_hash(const __u8 seed[16], const void *name, size_t name_len,
                       __u64 *hash48_out, __u32 *low32_out);

/* On-disk structure checksum helpers (compute & verify). */
void vtx_super_checksum_compute(struct vtx_super_block *sb);
int vtx_super_checksum_verify(const struct vtx_super_block *sb);

void vtx_cpd_checksum_compute(struct vtx_checkpoint_desc *cpd);
int vtx_cpd_checksum_verify(const struct vtx_checkpoint_desc *cpd);

void vtx_dev_table_checksum_compute(struct vtx_dev_table *dt);
int vtx_dev_table_checksum_verify(const struct vtx_dev_table *dt);

void vtx_vag_root_checksum_compute(struct vtx_vag_dir_root *root,
                                   const __u8 metadata_uuid[VTXFS_UUID_SIZE]);
int vtx_vag_root_checksum_verify(const struct vtx_vag_dir_root *root,
                                 const __u8 metadata_uuid[VTXFS_UUID_SIZE]);

int vtx_node_checksum_compute(void *node, size_t node_bytes,
                              const __u8 metadata_uuid[VTXFS_UUID_SIZE]);
int vtx_node_checksum_verify(const void *node, size_t node_bytes,
                             const __u8 metadata_uuid[VTXFS_UUID_SIZE]);

#ifdef __cplusplus
}
#endif
#endif /* _VTXFS_CSUM_H */
