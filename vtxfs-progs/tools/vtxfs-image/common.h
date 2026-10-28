/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef VTXFS_IMAGE_COMMON_H
#define VTXFS_IMAGE_COMMON_H

#include <stdio.h>
#include "metadump.h"

struct md_ctx {
	struct vtx_bdev bdev;
	struct vtx_fs *fs;
	FILE *out;          /* create: ghi dump; restore: đọc dump */
	const char *path;
	int compress;
	int full;
	int sanitize;
	__u64 block_count;
};

int md_open_fs(struct md_ctx *c, const char *img, int rw);
void md_close_fs(struct md_ctx *c);

int image_create_main(int argc, char **argv);
int image_restore_main(int argc, char **argv);

/* sanitize.c */
int sanitize_node(struct md_ctx *c, void *node, __u32 node_size);

/* LZSS compression / decompression */
long vtxmd_lz_compress(const __u8 *in, size_t in_len, __u8 *out, size_t out_cap);
long vtxmd_lz_decompress(const __u8 *in, size_t in_len, __u8 *out, size_t out_cap);

#endif
