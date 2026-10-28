/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef VTXFS_METADUMP_H
#define VTXFS_METADUMP_H

#include "vtxfs_engine.h"

#define VTXFS_MD_MAGIC   "VTXMD21"
#define VTXFS_MD_MAGIC_LEN 8
#define VTXFS_MD_VERSION 1U

#define VTXFS_MD_FLAG_SANITIZED  (1U << 0)
#define VTXFS_MD_FLAG_COMPRESSED (1U << 1)  /* mỗi record nén LZSS */
#define VTXFS_MD_FLAG_WITH_DATA  (1U << 2)  /* dump cả block dữ liệu */

/* Dump lưu mọi block siêu dữ liệu theo địa chỉ VẬT LÝ (pblk) để restore
 * không cần dịch lại VAG. Block dữ liệu file KHÔNG được dump trừ khi có --full. */
struct vtx_metadump_hdr {
	__u8 magic[VTXFS_MD_MAGIC_LEN];
	__le32 version;
	__le32 flags;
	__u8 fsid[VTXFS_UUID_SIZE];
	__le32 node_size;
	__le64 total_blocks;
	__le64 committed_xid;
	__le64 block_count;   /* số block siêu dữ liệu trong dump */
	__u8 reserved[64];
} __packed;

/* Một bản ghi dump: pblk vật lý + node_size byte dữ liệu (hoặc kích thước nén). */
struct vtx_md_rec {
	__le64 pblk;
	__le32 len;
	__le32 reserved;
} __packed;

#endif
