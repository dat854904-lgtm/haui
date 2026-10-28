// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-quota.c — KHUNG. SIMPLE_QUOTA là DEFERRED theo UAPI feature gate
 * (VTXFS_FEAT_INCOMPAT_SIMPLE_QUOTA). Các trường quota_* trong
 * vtx_subvol_root_item đã có sẵn trên đĩa nhưng chưa có protocol cập nhật.
 */
#include <stdio.h>
#include "commands.h"
#include "common.h"

static int cmd_quota_enable(const struct cmd_struct *c, int argc, char **argv)
{
	(void)c;
	(void)argc;
	(void)argv;
	fprintf(stderr,
		"vtxfs quota enable: SIMPLE_QUOTA là DEFERRED — "
		"chưa có protocol cập nhật quota_referenced/exclusive. Từ chối.\n");
	return 1;
}

static int cmd_quota_disable(const struct cmd_struct *c, int argc, char **argv)
{
	(void)c;
	(void)argc;
	(void)argv;
	fprintf(stderr,
		"vtxfs quota disable: SIMPLE_QUOTA là DEFERRED. Từ chối.\n");
	return 1;
}

static int cmd_quota_status(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 subvol = 0;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs quota status <image> [--subvol=N]\n");
		return 1;
	}
	vtxfs_cli_parse_subvol(argc - 1, argv + 1, &subvol);
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	printf("Quota status: SIMPLE_QUOTA là DEFERRED.\n");
	printf("  (Các trường quota_* trong vtx_subvol_root_item tồn tại trên đĩa\n");
	printf("   nhưng không được cập nhật ở P0.)\n");
	vtxfs_cli_close(&h, 0);
	return 0;
}

static const struct cmd_struct quota_cmds[] = {
	{ "enable",  cmd_quota_enable,  NULL, "<image>" },
	{ "disable", cmd_quota_disable, NULL, "<image>" },
	{ "status",  cmd_quota_status,  NULL, "<image> [--subvol=N]" },
};

const struct cmd_group quota_cmd_group = {
	.name = "quota",
	.usage = "Quản lý quota (SIMPLE_QUOTA DEFERRED — chỉ status hoạt động)",
	.cmds = quota_cmds,
	.ncmds = sizeof(quota_cmds) / sizeof(quota_cmds[0]),
};
