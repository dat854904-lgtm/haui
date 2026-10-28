// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <stdio.h>
#include <string.h>
#include "commands.h"
#include "common.h"

static int cmd_reaper_run(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs reaper run <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;
	rc = vtx_reaper_run_all(h.fs);
	if (!rc)
		rc = vtx_sync_checkpoint(h.fs);
	printf(rc ? "reaper lỗi: %s\n" : "reaper: hoàn tất mọi job\n",
	       rc ? strerror(-rc) : "");
	vtxfs_cli_close(&h, 1);
	return rc ? 1 : 0;
}

static const struct cmd_struct reaper_cmds[] = {
	{ "run", cmd_reaper_run, NULL, "<image>" },
};

const struct cmd_group reaper_cmd_group = {
	.name = "reaper",
	.usage = "Chạy trình thu hồi không gian nền của các subvolume đã xoá",
	.cmds = reaper_cmds,
	.ncmds = sizeof(reaper_cmds) / sizeof(reaper_cmds[0]),
};
