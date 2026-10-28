// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

static int list_cb(const struct vtx_subvol_root_item *it, __u64 id, void *arg)
{
	struct vtxfs_handle *h = arg;
	char flags[64];
	char name[VTXFS_NAME_MAX + 1];
	int nret = vtx_subvol_get_name(h->fs, id, name, sizeof(name));

	printf("ID %-6llu name %-24s parent %-6llu inodes %-8llu %s\n",
	       (unsigned long long)id,
	       nret == 0 ? name : "(default)",
	       (unsigned long long)vtx_cpu64(it->parent_subvol_id),
	       (unsigned long long)vtx_cpu64(it->num_inodes),
	       vtxfs_cli_subvol_flags_str(vtx_cpu64(it->flags),
					  flags, sizeof(flags)));
	printf("        uuid ");
	vtxfs_cli_print_uuid(it->uuid);
	printf("  create_xid %llu\n",
	       (unsigned long long)vtx_cpu64(it->create_xid));
	return 0;
}

int cmd_subvolume(int argc, char **argv)
{
	struct vtxfs_handle h;
	const char *sub = argc > 1 ? argv[1] : "list";
	const char *img = argc > 2 ? argv[2] : NULL;
	int rc;

	if (!img) {
		fprintf(stderr, "usage: vtxfs subvolume <list|create|delete> <image> [id|name]\n");
		return 1;
	}

	if (strcmp(sub, "list") == 0) {
		rc = vtxfs_cli_open(&h, img, 0);
		if (rc)
			return 1;
		printf("Subvolumes trên '%s':\n", img);
		rc = vtxfs_cli_for_each_subvol(&h, list_cb, &h);
		vtxfs_cli_close(&h, 0);
		return rc ? 1 : 0;
	}

	if (strcmp(sub, "create") == 0) {
		const char *name = argc > 3 ? argv[3] : "subvol";
		__u64 new_id = 0;

		rc = vtxfs_cli_open(&h, img, 1);
		if (rc)
			return 1;
		rc = vtx_subvol_create(h.fs, VTXFS_OID_FIRST_USER_SUBVOL,
				       name, &new_id);
		if (rc) {
			fprintf(stderr, "vtxfs: tạo subvolume thất bại: %s\n",
				strerror(-rc));
			vtxfs_cli_close(&h, 0);
			return 1;
		}
		vtx_sync_checkpoint(h.fs);
		printf("Đã tạo subvolume '%s' -> ID %llu\n", name,
		       (unsigned long long)new_id);
		vtxfs_cli_close(&h, 1);
		return 0;
	}

	if (strcmp(sub, "delete") == 0) {
		__u64 id;

		if (argc < 4) {
			fprintf(stderr, "usage: vtxfs subvolume delete <image> <id>\n");
			return 1;
		}
		id = strtoull(argv[3], NULL, 10);
		rc = vtxfs_cli_open(&h, img, 1);
		if (rc)
			return 1;
		rc = vtx_subvol_destroy(h.fs, id);
		if (rc) {
			fprintf(stderr, "vtxfs: xoá subvolume %llu thất bại: %s\n",
				(unsigned long long)id, strerror(-rc));
			vtxfs_cli_close(&h, 0);
			return 1;
		}
		/* Chạy Reaper để thu hồi dứt điểm. */
		rc = vtx_reaper_run_all(h.fs);
		if (rc)
			fprintf(stderr, "vtxfs: cảnh báo reaper: %s\n",
				strerror(-rc));
		vtx_sync_checkpoint(h.fs);
		printf("Đã xoá subvolume ID %llu\n", (unsigned long long)id);
		vtxfs_cli_close(&h, 1);
		return 0;
	}

	fprintf(stderr, "vtxfs: lệnh con không rõ '%s'\n", sub);
	return 1;
}
