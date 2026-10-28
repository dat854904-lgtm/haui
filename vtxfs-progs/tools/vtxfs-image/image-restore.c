// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

int image_restore_main(int argc, char **argv)
{
	struct vtx_metadump_hdr hdr;
	struct vtx_bdev bdev;
	FILE *in = NULL;
	const char *dump = NULL, *target = NULL;
	__u8 *blk = NULL;
	__u8 *cbuf = NULL;
	__u64 written = 0;
	int i, rc = 0, compressed = 0;

	for (i = 0; i < argc; i++) {
		if (!dump)
			dump = argv[i];
		else if (!target)
			target = argv[i];
	}
	if (!dump || !target) {
		fprintf(stderr, "usage: vtxfs-image restore <dump> <target>\n");
		return 1;
	}
	in = fopen(dump, "rb");
	if (!in) {
		fprintf(stderr, "restore: không đọc được '%s'\n", dump);
		return 1;
	}
	if (fread(&hdr, sizeof(hdr), 1, in) != 1 ||
	    memcmp(hdr.magic, VTXFS_MD_MAGIC, VTXFS_MD_MAGIC_LEN) != 0) {
		fprintf(stderr, "restore: header dump không hợp lệ\n");
		fclose(in);
		return 1;
	}
	blk = malloc(VTXFS_BLOCK_SIZE);
	cbuf = malloc(VTXFS_BLOCK_SIZE + VTXFS_BLOCK_SIZE / 8U + 64U);
	if (!blk || !cbuf) {
		free(blk);
		free(cbuf);
		fclose(in);
		return 1;
	}
	/* Tạo ảnh đích với đúng total_blocks. */
	rc = vtx_file_bdev_open(&bdev, target, 2,
				vtx_cpu64(hdr.total_blocks));
	if (rc) {
		fprintf(stderr, "restore: tạo '%s' thất bại: %s\n",
			target, strerror(-rc));
		free(blk);
		free(cbuf);
		fclose(in);
		return 1;
	}
	compressed = (vtx_cpu32(hdr.flags) & VTXFS_MD_FLAG_COMPRESSED) != 0;
	for (;;) {
		struct vtx_md_rec rec;
		__u32 len;
		const __u8 *src;

		if (fread(&rec, sizeof(rec), 1, in) != 1)
			break;
		len = vtx_cpu32(rec.len);
		if (len > VTXFS_BLOCK_SIZE + VTXFS_BLOCK_SIZE / 8U + 64U) {
			rc = -EUCLEAN;
			break;
		}
		if (fread(cbuf, 1, len, in) != len) {
			rc = -EIO;
			break;
		}
		if (compressed) {
			long dsz = vtxmd_lz_decompress(cbuf, len, blk,
						       VTXFS_BLOCK_SIZE);
			if (dsz != VTXFS_BLOCK_SIZE) {
				rc = -EUCLEAN;
				break;
			}
			src = blk;
		} else {
			if (len != VTXFS_BLOCK_SIZE) {
				rc = -EUCLEAN;
				break;
			}
			memcpy(blk, cbuf, len);
			src = blk;
		}
		rc = bdev.ops->write_blocks(&bdev, vtx_cpu64(rec.pblk), 1, src);
		if (rc)
			break;
		written++;
	}
	bdev.ops->flush(&bdev);
	vtx_file_bdev_close(&bdev);
	free(blk);
	free(cbuf);
	fclose(in);
	printf("vtxfs-image: restore %s, ghi %llu block%s%s\n",
	       rc ? "THẤT BẠI" : "OK", (unsigned long long)written,
	       (hdr.flags & vtx_le32(VTXFS_MD_FLAG_SANITIZED)) ?
	       " (dump đã sanitize)" : "",
	       compressed ? " (đã giải nén LZSS)" : "");
	if (!rc && !(vtx_cpu32(hdr.flags) & VTXFS_MD_FLAG_WITH_DATA))
		printf("  Lưu ý: block dữ liệu file KHÔNG được khôi phục "
		       "(fsck data CRC có thể báo lỗi — bình thường với metadump).\n");
	return rc ? 1 : 0;
}
