// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "commands.h"
#include "common.h"

extern int cmd_rescue_fix_data_checksum(const struct cmd_struct *cmd,
					int argc, char **argv);

static int cmd_rescue_super(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtx_bdev bdev;
	struct vtx_super_block sb, sb_mirr;
	int rc, bad_prim, bad_mirr;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs rescue super-recover <image>\n");
		return 1;
	}
	rc = vtx_file_bdev_open(&bdev, argv[0], 1, 0);
	if (rc)
		return 1;
	rc = bdev.ops->read_blocks(&bdev, VTXFS_SUPER_RING_PBLK, 1, &sb);
	bad_prim = rc || !vtx_super_checksum_verify(&sb);
	rc = bdev.ops->read_blocks(&bdev,
			VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK,
			1, &sb_mirr);
	bad_mirr = rc || !vtx_super_checksum_verify(&sb_mirr);
	if (!bad_prim) {
		printf("Superblock chính hợp lệ, không cần khôi phục.\n");
	} else if (!bad_mirr) {
		rc = bdev.ops->write_blocks(&bdev, VTXFS_SUPER_RING_PBLK, 1,
					    &sb_mirr);
		if (rc)
			printf("Ghi khôi phục thất bại: %s\n", strerror(-rc));
		else
			printf("Đã khôi phục superblock chính từ mirror 64MiB.\n");
	} else {
		printf("Cả superblock chính lẫn mirror đều hỏng.\n");
		rc = -EUCLEAN;
	}
	vtx_file_bdev_close(&bdev);
	return rc ? 1 : 0;
}

static int cmd_rescue_vag(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs rescue vag-recover <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;
	rc = vtx_vag_rebuild_bitmaps(h.fs);
	if (!rc)
		rc = vtx_sync_checkpoint(h.fs);
	if (rc)
		printf("vag-recover lỗi: %s\n", strerror(-rc));
	else
		printf("Đã tái dựng VAG bitmap từ REF+SFQ.\n");
	vtxfs_cli_close(&h, 1);
	return rc ? 1 : 0;
}

static const struct cmd_struct rescue_cmds[] = {
	{ "super-recover",     cmd_rescue_super,             NULL, "<image>" },
	{ "vag-recover",       cmd_rescue_vag,               NULL, "<image>" },
	{ "fix-data-checksum", cmd_rescue_fix_data_checksum, NULL, "<image> [--subvol=N]" },
};

const struct cmd_group rescue_cmd_group = {
	.name = "rescue",
	.usage = "Các công cụ cứu hộ superblock, VAG và data CRC",
	.cmds = rescue_cmds,
	.ncmds = sizeof(rescue_cmds) / sizeof(rescue_cmds[0]),
};
