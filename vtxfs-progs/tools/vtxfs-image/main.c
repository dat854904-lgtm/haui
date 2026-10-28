// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <stdio.h>
#include <string.h>
#include "common.h"

static void usage(void)
{
	fprintf(stderr,
		"vtxfs-image — dump/khôi phục siêu dữ liệu VTXFS\n"
		"  vtxfs-image create  <image> -o <dump> [--sanitize] [--compress] [--full]\n"
		"  vtxfs-image restore <dump> <target>\n");
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : NULL;

	if (!cmd || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		usage();
		return cmd ? 0 : 1;
	}
	argc -= 2;
	argv += 2;
	if (strcmp(cmd, "create") == 0)
		return image_create_main(argc, argv);
	if (strcmp(cmd, "restore") == 0)
		return image_restore_main(argc, argv);
	usage();
	return 1;
}
