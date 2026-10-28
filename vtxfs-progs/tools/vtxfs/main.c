// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "commands.h"

void vtxfs_print_group_usage(const struct cmd_group *grp)
{
	size_t i;

	fprintf(stderr, "Usage: vtxfs %s%s<command> [args]\n",
		grp->name[0] ? grp->name : "",
		grp->name[0] ? " " : "");
	if (grp->usage)
		fprintf(stderr, "  %s\n", grp->usage);
	fprintf(stderr, "Commands:\n");
	for (i = 0; i < grp->ncmds; i++)
		fprintf(stderr, "  %-14s %s\n", grp->cmds[i].name,
			grp->cmds[i].usage_short ? grp->cmds[i].usage_short : "");
}

int vtxfs_dispatch(const struct cmd_group *grp, int argc, char **argv)
{
	const char *sub;
	size_t i;

	if (argc == 0 || !strcmp(argv[0], "-h") || !strcmp(argv[0], "--help")) {
		vtxfs_print_group_usage(grp);
		return argc == 0 ? 1 : 0;
	}
	sub = argv[0];
	for (i = 0; i < grp->ncmds; i++) {
		const struct cmd_struct *c = &grp->cmds[i];

		if (strcmp(c->name, sub) != 0)
			continue;
		if (c->group)
			return vtxfs_dispatch(c->group, argc - 1, argv + 1);
		return c->fn(c, argc - 1, argv + 1);
	}
	fprintf(stderr, "vtxfs: lệnh không rõ '%s%s%s'\n",
		grp->name, grp->name[0] ? " " : "", sub);
	vtxfs_print_group_usage(grp);
	return 1;
}

static int run_internal_tool(const char *script_name, int argc, char **argv)
{
	char script_path[512];
	snprintf(script_path, sizeof(script_path), "/usr/local/lib/vtxfs/%s", script_name);
	if (access(script_path, R_OK) != 0) {
		snprintf(script_path, sizeof(script_path), "./tools/%s", script_name);
		if (access(script_path, R_OK) != 0)
			snprintf(script_path, sizeof(script_path), "../tools/%s", script_name);
	}

	char **exec_argv = malloc((size_t)(argc + 3) * sizeof(char *));
	if (!exec_argv)
		return 1;
	exec_argv[0] = "python3";
	exec_argv[1] = script_path;
	for (int i = 0; i < argc; i++)
		exec_argv[i + 2] = argv[i];
	exec_argv[argc + 2] = NULL;

	pid_t pid = fork();
	if (pid < 0) {
		free(exec_argv);
		return 1;
	}
	if (pid == 0) {
		execvp("python3", exec_argv);
		_exit(127);
	}
	free(exec_argv);
	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return 1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}

static int cmd_top_verify(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 2) {
		fprintf(stderr, "Usage: vtxfs verify <source> <destination> [--merkle] [--quick] [--threads=N] [--save-manifest=FILE] [--use-manifest=FILE]\n");
		return 1;
	}
	return run_internal_tool("vtxfs_verify.py", argc, argv);
}


static int cmd_top_gui(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	return run_internal_tool("vtxfs_gui.py", argc, argv);
}

static const struct cmd_struct top_cmds[] = {
	{ "check",      cmd_check,      NULL,        "<device> [--repair] [--dry-run] (Kiểm tra & sửa chữa fsck)" },
	{ "format",     cmd_format,     NULL,        "<device> [-L label] [-b blocks] (Định dạng hệ thống tệp)" },
	{ "mkfs",       cmd_format,     NULL,        "<device> [-L label] [-b blocks]" },
	{ "label",      cmd_label,      NULL,        "<device> [new_label] (Xem hoặc đổi nhãn phân vùng)" },
	{ "cp",         cmd_cp,         NULL,        "<src> <dst> [-f] (Truyền tệp tăng tốc siêu tốc với fallocate & 1MB buffer)" },
	{ "transfer",   cmd_cp,         NULL,        "<src> <dst> [-f] (Truyền tệp tăng tốc)" },
	{ "image",      NULL, &image_cmd_group,      "create/restore (Metadump create/restore)" },
	{ "filesystem", NULL, &filesystem_cmd_group, "info/sync/usage/defragment" },
	{ "verify",     cmd_top_verify, NULL,        "<src> <dst> [--merkle] [--quick]" },
	{ "stat",       cmd_stat,       NULL,        "<device>" },
	{ "map",        cmd_map,        NULL,        "<device> [--vag=N] [--compact]" },
	{ "block",      cmd_block,      NULL,        "<device> <block_id> [--vblk]" },
	{ "bitmap",     cmd_bitmap,     NULL,        "<device> [vag] [offset [count]]" },
	{ "scrub",      NULL, &scrub_cmd_group,      "start/status" },
	{ "frag",       cmd_frag,       NULL,        "<file|dir> [--details]" },
	{ "file",       cmd_file,       NULL,        "<file_path> | <device> <vtxfs_path> (Đọc cấu trúc & extents tệp)" },
	{ "sys-index",  cmd_sys_index,  NULL,        "<device> (Bản đồ & chỉ mục vùng hệ thống 128 MiB)" },
	{ "sys",        cmd_sys_index,  NULL,        "<device>" },
	{ "bench",      cmd_bench,      NULL,        "<device> [--profile=usb|nvme]" },
	{ "ctl",        cmd_ctl,        NULL,        "<image> <command> [args]" },
	{ "gui",        cmd_top_gui,    NULL,        "Giao diện đồ họa dashboard" },
	{ "subvolume",  NULL, &subvolume_cmd_group,  "create/delete/list" },
	{ "snapshot",   NULL, &snapshot_cmd_group,   "create/delete/list" },
	{ "inspect",    NULL, &inspect_cmd_group,    "stat/map/block/dump-super/dump-tree" },
	{ "rescue",     NULL, &rescue_cmd_group,     "super-recover/vag-recover/fix-data-checksum" },
	{ "property",   NULL, &property_cmd_group,   "get/set" },
	{ "balance",    NULL, &balance_cmd_group,    "start/status/cancel" },
	{ "defrag",     NULL, &defrag_cmd_group,     "scan/start" },
	{ "reaper",     NULL, &reaper_cmd_group,     "run" },
	{ "device",     NULL, &device_cmd_group,     "info/stats/add/remove/replace" },
	{ "restore",    NULL, &restore_cmd_group,    "list/extract/restore-all" },
	{ "quota",      NULL, &quota_cmd_group,      "enable/disable/status" },
	{ "reflink",    cmd_reflink,    NULL,        "<img> <src> <dst>" },
	{ "send",       cmd_send,       NULL,        "<img> [--subvol=N]" },
	{ "receive",    cmd_receive,    NULL,        "<img>" },
};

static const struct cmd_group top_group = {
	.name = "",
	.usage = "VTXFS v2.1 unified CLI",
	.cmds = top_cmds,
	.ncmds = sizeof(top_cmds) / sizeof(top_cmds[0]),
};

int main(int argc, char **argv)
{
	const char *prog = strrchr(argv[0], '/');
	prog = prog ? prog + 1 : argv[0];

	/* Busybox-style symlink routing:
	 * vtxfsctl -> vtxfs ctl
	 * vtxfs-<subcmd> -> vtxfs <subcmd>
	 */
	const char *subcmd = NULL;
	if (strcmp(prog, "vtxfsctl") == 0) {
		subcmd = "ctl";
	} else if (strcmp(prog, "fsck.vtxfs") == 0) {
		subcmd = "check";
	} else if (strcmp(prog, "mkfs.vtxfs") == 0) {
		subcmd = "format";
	} else if (strncmp(prog, "vtxfs-", 6) == 0 && strlen(prog) > 6) {
		subcmd = prog + 6;
		if (strcmp(subcmd, "info") == 0 || strcmp(subcmd, "sys-info") == 0)
			subcmd = "stat";
		else if (strcmp(subcmd, "dump") == 0)
			subcmd = "block";
		else if (strcmp(subcmd, "sys") == 0)
			subcmd = "sys-index";
	}

	if (subcmd) {
		int is_valid = 0;
		for (size_t i = 0; i < top_group.ncmds; i++) {
			if (strcmp(top_group.cmds[i].name, subcmd) == 0) {
				is_valid = 1;
				break;
			}
		}
		if (!is_valid)
			subcmd = NULL;
	}

	if (subcmd) {
		char **new_argv = malloc((argc + 2) * sizeof(char *));
		if (new_argv) {
			new_argv[0] = argv[0];
			new_argv[1] = (char *)subcmd;
			for (int i = 1; i < argc; i++) {
				new_argv[i + 1] = argv[i];
			}
			new_argv[argc + 1] = NULL;
			int ret = vtxfs_dispatch(&top_group, argc, new_argv + 1);
			free(new_argv);
			return ret;
		}
	}

	return vtxfs_dispatch(&top_group, argc - 1, argv + 1);
}
