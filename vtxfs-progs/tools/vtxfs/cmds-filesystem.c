// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "commands.h"
#include "common.h"

static int cmd_fs_info(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct vtx_fs *fs;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs filesystem info <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	fs = h.fs;
	printf("VTXFS v%u.%u label='%s'\n",
	       vtx_cpu16(fs->super_blk.hdr.major),
	       vtx_cpu16(fs->super_blk.hdr.minor), fs->super_blk.hdr.label);
	printf("  fsid        : ");
	vtxfs_cli_print_uuid(fs->super_blk.hdr.fsid_uuid);
	printf("\n  total_blocks: %llu (%llu MiB)\n",
	       (unsigned long long)vtx_cpu64(fs->super_blk.hdr.total_blocks),
	       (unsigned long long)((vtx_cpu64(fs->super_blk.hdr.total_blocks)
				     * VTXFS_BLOCK_SIZE) >> 20));
	printf("  xid         : committed=%llu current=%llu\n",
	       (unsigned long long)fs->committed_xid,
	       (unsigned long long)fs->current_xid);
	printf("  free_blocks : %llu\n",
	       (unsigned long long)vtx_cpu64(fs->cp_blk.hdr.free_blocks));
	vtxfs_cli_close(&h, 0);
	return 0;
}

static int cmd_fs_sync(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs filesystem sync <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;
	rc = vtx_sync_checkpoint(h.fs);
	if (rc)
		printf("sync lỗi: %s\n", strerror(-rc));
	else
		printf("sync OK xid=%llu\n", (unsigned long long)h.fs->committed_xid);
	vtxfs_cli_close(&h, 1);
	return rc ? 1 : 0;
}

static int cmd_fs_usage(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u32 i;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs filesystem usage <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	printf("VAG usage (%u VAGs):\n", h.fs->vag.total_vags);
	for (i = 0; i < h.fs->vag.total_vags; i++)
		printf("  VAG[%u] free=%u/%u\n", i,
		       vtx_cpu32(h.fs->vag.descs[i].free_blocks),
		       VTXFS_VAG_BLOCKS);
	vtxfs_cli_close(&h, 0);
	return 0;
}

static const struct cmd_struct fs_cmds[] = {
	{ "info",       cmd_fs_info,    NULL, "<image>" },
	{ "sync",       cmd_fs_sync,    NULL, "<image>" },
	{ "usage",      cmd_fs_usage,   NULL, "<image>" },
	{ "defragment", cmd_defragment, NULL, "<image> [path]" },
};

const struct cmd_group filesystem_cmd_group = {
	.name = "filesystem",
	.usage = "Lệnh quản lý filesystem chung",
	.cmds = fs_cmds,
	.ncmds = sizeof(fs_cmds) / sizeof(fs_cmds[0]),
};
