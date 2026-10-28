// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

int md_open_fs(struct md_ctx *c, const char *img, int rw)
{
	int rc;

	memset(&c->bdev, 0, sizeof(c->bdev));
	c->fs = NULL;
	c->out = NULL;
	c->path = img;
	c->block_count = 0;
	rc = vtx_file_bdev_open(&c->bdev, img, rw ? 1 : 0, 0);
	if (rc)
		return rc;
	c->fs = vtx_fs_alloc();
	if (!c->fs) {
		vtx_file_bdev_close(&c->bdev);
		return -ENOMEM;
	}
	rc = vtx_mount(c->fs, &c->bdev, 1);
	if (rc) {
		vtx_fs_free(c->fs);
		vtx_file_bdev_close(&c->bdev);
		c->fs = NULL;
		return rc;
	}
	return 0;
}

void md_close_fs(struct md_ctx *c)
{
	if (c->fs) {
		vtx_unmount(c->fs);
		vtx_fs_free(c->fs);
		c->fs = NULL;
	}
	vtx_file_bdev_close(&c->bdev);
	if (c->out) {
		fclose(c->out);
		c->out = NULL;
	}
}

/* LZSS sliding-window, block 4 KiB */
#define LZ_N 4096
#define LZ_F 18
#define LZ_T 3

long vtxmd_lz_compress(const __u8 *in, size_t in_len, __u8 *out, size_t out_cap)
{
	size_t i = 0, o = 0;

	while (i < in_len) {
		size_t flag_pos = o;
		__u8 flags = 0;
		int b;

		if (o >= out_cap)
			return -1;
		out[o++] = 0;
		for (b = 0; b < 8 && i < in_len; b++) {
			size_t best_len = 0, best_off = 0;
			size_t j = i;
			int cands = 0;

			while (j > 0 && (i - j) <= LZ_N && cands < 256) {
				size_t l = 0;

				j--;
				cands++;
				while (l < LZ_F && i + l < in_len &&
				       in[j + l] == in[i + l])
					l++;
				if (l > best_len) {
					best_len = l;
					best_off = i - j;
					if (best_len >= LZ_F)
						break;
				}
			}
			if (best_len >= LZ_T) {
				__u16 off = (__u16)(best_off - 1U);
				__u8 len = (__u8)(best_len - LZ_T);

				flags |= (__u8)(1 << b);
				if (o + 2 > out_cap)
					return -1;
				out[o++] = (__u8)(off & 0xFFU);
				out[o++] = (__u8)((off >> 8) | (len << 4));
				i += best_len;
			} else {
				if (o >= out_cap)
					return -1;
				out[o++] = in[i++];
			}
		}
		out[flag_pos] = flags;
	}
	return (long)o;
}

long vtxmd_lz_decompress(const __u8 *in, size_t in_len, __u8 *out, size_t out_cap)
{
	size_t i = 0, o = 0;

	while (i < in_len) {
		__u8 flags = in[i++];
		int b;

		for (b = 0; b < 8 && i < in_len; b++) {
			if (flags & (1 << b)) {
				__u16 off;
				size_t len, k;

				if (i + 2 > in_len)
					return -1;
				off = (__u16)(in[i] | ((in[i + 1] & 0x0FU) << 8));
				len = (size_t)(in[i + 1] >> 4) + LZ_T;
				i += 2;
				off = (__u16)(off + 1U);
				if (o < off || o + len > out_cap)
					return -1;
				for (k = 0; k < len; k++)
					out[o + k] = out[o - off + k];
				o += len;
			} else {
				if (o >= out_cap)
					return -1;
				out[o++] = in[i++];
			}
		}
	}
	return (long)o;
}
