// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-format.c — Unified CLI integration for vtxfs format / mkfs and vtxfs label
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "commands.h"
#include "common.h"
#include "vtxfs_engine.h"
#include "vtxfs_csum.h"

static void format_generate_uuid(__u8 uuid[16])
{
	FILE *urand = fopen("/dev/urandom", "rb");
	size_t i;

	if (urand) {
		size_t got = fread(uuid, 1, 16, urand);
		fclose(urand);
		if (got == 16) {
			uuid[6] = (__u8)((uuid[6] & 0x0FU) | 0x40U);
			uuid[8] = (__u8)((uuid[8] & 0x3FU) | 0x80U);
			return;
		}
	}
	srand((unsigned int)time(NULL));
	for (i = 0; i < 16; i++)
		uuid[i] = (__u8)(rand() & 0xFF);
	uuid[6] = (__u8)((uuid[6] & 0x0FU) | 0x40U);
	uuid[8] = (__u8)((uuid[8] & 0x3FU) | 0x80U);
}

int cmd_format(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	const char *label = "vtxfs";
	const char *path = NULL;
	__u64 blocks = 0;
	int user_specified_blocks = 0;
	__u8 uuid[16];
	struct vtx_bdev bdev;
	int i, rc;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
			label = argv[++i];
		} else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			char *end = NULL;
			unsigned long long val = strtoull(argv[++i], &end, 10);
			if (end && *end == '\0' && val > 0) {
				blocks = val;
				user_specified_blocks = 1;
			}
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			fprintf(stderr, "Cách dùng: vtxfs format [-L nhãn] [-b số_khối_4k] <thiết_bị_hoặc_image>\n"
					"Định dạng hệ thống tệp VTXFS v2.1\n");
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "vtxfs format: Tùy chọn không hợp lệ '%s'\n", argv[i]);
			return 1;
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		fprintf(stderr, "Cách dùng: vtxfs format [-L nhãn] [-b số_khối_4k] <thiết_bị_hoặc_image>\n");
		return 1;
	}

	if (!user_specified_blocks) {
		struct stat st;
		if (stat(path, &st) == 0 && S_ISBLK(st.st_mode)) {
			blocks = 0; /* Tự động truy vấn kích thước block device */
		} else if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
			blocks = (__u64)st.st_size / 4096ULL;
		} else {
			blocks = 65536ULL; /* Mặc định 256 MiB cho image mới */
		}
	}

	format_generate_uuid(uuid);

	rc = vtx_file_bdev_open(&bdev, path, 2, blocks);
	if (rc != 0) {
		fprintf(stderr, "vtxfs format: Không thể mở '%s': %s\n", path, strerror(-rc));
		return 1;
	}

	rc = vtx_mkfs(&bdev, label, uuid);
	if (rc != 0) {
		fprintf(stderr, "vtxfs format: Định dạng thất bại: %s\n", strerror(-rc));
		vtx_file_bdev_close(&bdev);
		return 1;
	}

	blocks = bdev.total_blocks;
	vtx_file_bdev_close(&bdev);

	printf("vtxfs format: Đã định dạng thành công '%s'\n", path);
	printf("  Nhãn phân vùng (Label)  : %s\n", label);
	printf("  Mã định danh FSID       : ");
	vtxfs_cli_print_uuid(uuid);
	printf("\n  Tổng số khối (Blocks)   : %llu (Kích thước: %llu MiB)\n",
	       (unsigned long long)blocks,
	       (unsigned long long)((blocks * VTXFS_BLOCK_SIZE) >> 20));
	printf("  Cấu hình VAG            : %llu VAGs x 32768 blocks (128 MiB/VAG)\n",
	       (unsigned long long)(blocks >= 32768 ? (blocks - 32768) / 32768 : 0));
	return 0;
}

int cmd_label(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs label <thiết_bị_hoặc_image> [nhãn_mới]\n"
				"Xem hoặc đổi nhãn phân vùng VTXFS\n");
		return 1;
	}
	const char *path = argv[0];

	if (argc == 1) {
		/* Chỉ xem nhãn */
		struct vtxfs_handle h;
		int rc = vtxfs_cli_open(&h, path, 0);
		if (rc != 0) {
			fprintf(stderr, "vtxfs label: Không thể mở '%s': %s\n", path, strerror(-rc));
			return 1;
		}
		printf("%s\n", h.fs->super_blk.hdr.label);
		vtxfs_cli_close(&h, 0);
		return 0;
	}

	/* Gán nhãn mới */
	const char *new_label = argv[1];
	struct vtxfs_handle h;
	int rc = vtxfs_cli_open(&h, path, 1);
	if (rc != 0) {
		fprintf(stderr, "vtxfs label: Không thể mở ghi '%s': %s\n", path, strerror(-rc));
		return 1;
	}

	size_t n = strlen(new_label);
	if (n >= VTXFS_LABEL_SIZE)
		n = VTXFS_LABEL_SIZE - 1U;

	memset(h.fs->super_blk.hdr.label, 0, VTXFS_LABEL_SIZE);
	memcpy(h.fs->super_blk.hdr.label, new_label, n);
	h.fs->super_blk.hdr.super_generation =
		vtx_le64(vtx_cpu64(h.fs->super_blk.hdr.super_generation) + 1);
	vtx_super_checksum_compute(&h.fs->super_blk);

	rc = h.bdev.ops->write_blocks(&h.bdev, VTXFS_SUPER_RING_PBLK, 1, &h.fs->super_blk);
	if (rc == 0) {
		(void)h.bdev.ops->write_blocks(&h.bdev,
			VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK,
			1, &h.fs->super_blk);
	}
	vtxfs_cli_close(&h, 1);

	if (rc == 0) {
		printf("vtxfs label: Đã đổi nhãn '%s' thành '%s' thành công.\n", path, new_label);
		return 0;
	} else {
		fprintf(stderr, "vtxfs label: Ghi nhãn thất bại: %s\n", strerror(-rc));
		return 1;
	}
}
