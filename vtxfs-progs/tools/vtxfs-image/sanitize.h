/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef VTXFS_IMAGE_SANITIZE_H
#define VTXFS_IMAGE_SANITIZE_H

#include "common.h"

/* Làm mờ dữ liệu nhạy cảm trong một node TRƯỚC khi ghi dump:
 *  - VTX_TYPE_DIR_ITEM: thay tên bằng token cùng độ dài, cập nhật low32.
 *  - VTX_TYPE_EXTENT inline: zero payload.
 *  - VTX_TYPE_XATTR: zero value.
 * Sau đó tính lại BLAKE3-128 để node tự nhất quán (restore được).
 * Khoá cây (key/slot) giữ nguyên nên cấu trúc B+Tree không đổi. */
int sanitize_node(struct md_ctx *c, void *node, __u32 node_size);

#endif
