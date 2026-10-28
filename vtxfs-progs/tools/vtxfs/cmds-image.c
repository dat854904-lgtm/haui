// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-image.c — Unified CLI integration for vtxfs image (metadump create/restore)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include "commands.h"

static int run_image_subcmd(const char *subcmd, int argc, char **argv)
{
	char **new_argv = malloc((argc + 3) * sizeof(char *));
	if (!new_argv)
		return 1;

	new_argv[0] = "vtxfs-image";
	new_argv[1] = (char *)subcmd;
	for (int i = 0; i < argc; i++)
		new_argv[i + 2] = argv[i];
	new_argv[argc + 2] = NULL;

	pid_t pid = fork();
	if (pid == 0) {
		execvp("vtxfs-image", new_argv);
		execv("/usr/local/bin/vtxfs-image", new_argv);
		fprintf(stderr, "vtxfs: Không tìm thấy binary 'vtxfs-image' trong PATH hoặc /usr/local/bin\n");
		_exit(127);
	}
	if (pid < 0) {
		free(new_argv);
		return 1;
	}

	int status = 0;
	waitpid(pid, &status, 0);
	free(new_argv);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int cmd_image_create(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs image create <image|device> -o <dump> [--sanitize] [--compress] [--full]\n");
		return 1;
	}
	return run_image_subcmd("create", argc, argv);
}

static int cmd_image_restore(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 2) {
		fprintf(stderr, "Cách dùng: vtxfs image restore <dump> <target>\n");
		return 1;
	}
	return run_image_subcmd("restore", argc, argv);
}

static const struct cmd_struct image_cmds[] = {
	{ "create",  cmd_image_create,  NULL, "<image> -o <dump> [--sanitize] [--compress] [--full]" },
	{ "restore", cmd_image_restore, NULL, "<dump> <target>" },
};

const struct cmd_group image_cmd_group = {
	.name = "image",
	.usage = "Dump và khôi phục siêu dữ liệu (metadump create/restore)",
	.cmds = image_cmds,
	.ncmds = sizeof(image_cmds) / sizeof(image_cmds[0]),
};
