// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs balance — SCAFFOLD.
 *   status: đọc VAG descriptors + CPD free/used (số liệu thật).
 *   start : kiểm tra feature gate; P0 single-device -> không có việc gì làm.
 *           MULTIDEV/RELOC/USAGE-balance là DEFERRED -> -EOPNOTSUPP.
 *   cancel: không có job nào được tạo ở P0 -> -ENOENT.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "common.h"

static int cmd_balance_status(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 total_free = 0, total_used = 0;
	__u32 i, active = 0;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs balance status <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;

	printf("Balance status '%s':\n", argv[0]);
	printf("  committed_xid : %llu\n",
	       (unsigned long long)h.fs->committed_xid);
	printf("  total_blocks  : %llu\n",
	       (unsigned long long)vtx_cpu64(h.fs->super_blk.hdr.total_blocks));
	printf("  cp free/used  : %llu / %llu\n",
	       (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.free_blocks),
	       (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.used_blocks));
	printf("  VAG count     : %u\n", h.fs->vag.total_vags);
	for (i = 0; i < h.fs->vag.total_vags; i++) {
		__u32 free_b = vtx_cpu32(h.fs->vag.descs[i].free_blocks);
		__u32 used_b = VTXFS_VAG_BLOCKS - free_b;

		total_free += free_b;
		total_used += used_b;
		if (used_b)
			active++;
		if (vtxfs_cli_opt(argc - 1, argv + 1, "verbose"))
			printf("    VAG[%u] free=%u used=%u\n", i, free_b, used_b);
	}
	printf("  VAG sum       : free=%llu used=%llu active=%u/%u\n",
	       (unsigned long long)total_free,
	       (unsigned long long)total_used,
	       active, h.fs->vag.total_vags);
	printf("  reloc job     : NONE (P0 single-device)\n");
	vtxfs_cli_close(&h, 0);
	return 0;
}

static int cmd_balance_start(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u32 devices;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr,
			"usage: vtxfs balance start <image> [--usage=N|-d|-m]\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;

	devices = vtx_cpu32(h.fs->super_blk.hdr.device_count);
	if (devices <= 1U) {
		printf("balance: thiết bị đơn — không có gì để cân bằng.\n");
		printf("         (Dùng 'balance status' để xem phân bố VAG.)\n");
		vtxfs_cli_close(&h, 0);
		return 0;
	}
	fprintf(stderr,
		"balance: MULTIDEV/TIERING là DEFERRED — từ chối tạo job.\n");
	vtxfs_cli_close(&h, 0);
	return 1;	/* -EOPNOTSUPP ở tầng feature gate */
}

static int cmd_balance_cancel(const struct cmd_struct *c, int argc, char **argv)
{
	(void)c;
	(void)argv;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs balance cancel <image>\n");
		return 1;
	}
	fprintf(stderr,
		"balance: không có job đang chạy ở P0 (không có gì để huỷ).\n");
	return 1;
}

static const struct cmd_struct balance_cmds[] = {
	{ "status", cmd_balance_status, NULL, "<image> [--verbose]" },
	{ "start",  cmd_balance_start,  NULL, "<image> [--usage=N|-d|-m]" },
	{ "cancel", cmd_balance_cancel, NULL, "<image>" },
};

const struct cmd_group balance_cmd_group = {
	.name = "balance",
	.usage = "Scaffold: status thật; start/cancel cần MULTIDEV (DEFERRED)",
	.cmds = balance_cmds,
	.ncmds = sizeof(balance_cmds) / sizeof(balance_cmds[0]),
};
