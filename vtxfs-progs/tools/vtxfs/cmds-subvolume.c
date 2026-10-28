// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "common.h"

static int list_cb(const struct vtx_subvol_root_item *it, __u64 id, void *arg)
{
	struct vtxfs_handle *h = arg;
	char name[VTXFS_NAME_MAX + 1];
	int nret = vtx_subvol_get_name(h->fs, id, name, sizeof(name));

	printf("ID %-6llu name %-24s parent %-6llu inodes %-8llu flags 0x%llx\n",
	       (unsigned long long)id,
	       nret == 0 ? name : "(default)",
	       (unsigned long long)vtx_cpu64(it->parent_subvol_id),
	       (unsigned long long)vtx_cpu64(it->num_inodes),
	       (unsigned long long)vtx_cpu64(it->flags));
	return 0;
}

static int snap_list_cb(const struct vtx_subvol_root_item *it, __u64 id, void *arg)
{
	struct vtxfs_handle *h = arg;
	char name[VTXFS_NAME_MAX + 1];
	int nret;

	if (!vtx_cpu64(it->last_snap_xid))
		return 0;
	nret = vtx_subvol_get_name(h->fs, id, name, sizeof(name));
	printf("snap ID %-6llu name %-20s src %-5llu flags 0x%llx last_snap_xid %llu\n",
	       (unsigned long long)id,
	       nret == 0 ? name : "(snapshot)",
	       (unsigned long long)vtx_cpu64(it->parent_subvol_id),
	       (unsigned long long)vtx_cpu64(it->flags),
	       (unsigned long long)vtx_cpu64(it->last_snap_xid));
	return 0;
}

static int cmd_subvol_list(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs subvolume list <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	rc = vtxfs_cli_for_each_subvol(&h, list_cb, &h);
	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

static int cmd_subvol_create(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 new_id = 0;
	int rc;

	(void)c;
	if (argc < 2) {
		fprintf(stderr, "usage: vtxfs subvolume create <image> <name>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;
	rc = vtx_subvol_create(h.fs, VTXFS_OID_FIRST_USER_SUBVOL,
			       argv[1], &new_id);
	printf(rc ? "create thất bại: %s\n" : "Đã tạo subvolume '%s' ID %llu\n",
	       rc ? strerror(-rc) : argv[1],
	       rc ? 0ULL : (unsigned long long)new_id);
	vtxfs_cli_close(&h, rc ? 0 : 1);
	return rc ? 1 : 0;
}

static int cmd_subvol_delete(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 id;
	int no_reap = vtxfs_cli_flag(argc, argv, "no-reap");
	int rc;

	(void)c;
	if (argc < 2) {
		fprintf(stderr,
			"usage: vtxfs subvolume delete <image> <id> [--no-reap]\n");
		return 1;
	}
	id = strtoull(argv[1], NULL, 10);
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;
	rc = vtx_subvol_destroy(h.fs, id);
	if (!rc && !no_reap)
		rc = vtx_reaper_run_all(h.fs);
	if (rc)
		printf("delete thất bại: %s\n", strerror(-rc));
	else
		printf("Đã xoá subvolume %llu%s\n", (unsigned long long)id,
		       no_reap ? " (REAPING, chưa chạy Reaper)" : "");
	vtxfs_cli_close(&h, rc ? 0 : 1);
	return rc ? 1 : 0;
}

static const struct cmd_struct subvol_cmds[] = {
	{ "list",   cmd_subvol_list,   NULL, "<image>" },
	{ "create", cmd_subvol_create, NULL, "<image> <name>" },
	{ "delete", cmd_subvol_delete, NULL, "<image> <id> [--no-reap]" },
};

const struct cmd_group subvolume_cmd_group = {
	.name = "subvolume",
	.usage = "Quản lý subvolume",
	.cmds = subvol_cmds,
	.ncmds = sizeof(subvol_cmds) / sizeof(subvol_cmds[0]),
};

static int cmd_snap_create(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 src = VTXFS_OID_FIRST_USER_SUBVOL, snap = 0;
	int ro = 0, rc;

	(void)c;
	if (argc < 2) {
		fprintf(stderr,
			"usage: vtxfs snapshot create <image> <src_id> [-r]\n");
		return 1;
	}
	src = strtoull(argv[1], NULL, 10);
	if (argc > 2 && !strcmp(argv[2], "-r"))
		ro = 1;
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;
	rc = vtx_snapshot_create(h.fs, src, NULL, ro, &snap);
	printf(rc ? "snap thất bại: %s\n" : "Đã tạo snapshot %s ID %llu\n",
	       rc ? strerror(-rc) : (ro ? "RO" : "RW"),
	       (unsigned long long)snap);
	vtxfs_cli_close(&h, rc ? 0 : 1);
	return rc ? 1 : 0;
}

static int cmd_snap_list(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs snapshot list <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	rc = vtxfs_cli_for_each_subvol(&h, snap_list_cb, &h);
	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

static const struct cmd_struct snap_cmds[] = {
	{ "create", cmd_snap_create,   NULL, "<image> <src_id> [-r]" },
	{ "list",   cmd_snap_list,     NULL, "<image>" },
	{ "delete", cmd_subvol_delete, NULL, "<image> <id> [--no-reap]" },
};

const struct cmd_group snapshot_cmd_group = {
	.name = "snapshot",
	.usage = "Quản lý snapshot",
	.cmds = snap_cmds,
	.ncmds = sizeof(snap_cmds) / sizeof(snap_cmds[0]),
};
