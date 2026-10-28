// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

static int snap_list_cb(const struct vtx_subvol_root_item *it, __u64 id,
			void *arg)
{
	char flags[64];

	(void)arg;
	/* Chỉ liệt kê các subvolume là snapshot (có last_snap_xid khác 0). */
	if (!vtx_cpu64(it->last_snap_xid))
		return 0;
	printf("snap ID %llu  src %-5llu %s last_snap_xid %llu\n",
	       (unsigned long long)id,
	       (unsigned long long)vtx_cpu64(it->parent_subvol_id),
	       vtxfs_cli_subvol_flags_str(vtx_cpu64(it->flags),
					  flags, sizeof(flags)),
	       (unsigned long long)vtx_cpu64(it->last_snap_xid));
	return 0;
}

int cmd_snapshot(int argc, char **argv)
{
	struct vtxfs_handle h;
	const char *sub = argc > 1 ? argv[1] : "list";
	const char *img = argc > 2 ? argv[2] : NULL;
	int rc;

	if (!img) {
		fprintf(stderr, "usage: vtxfs snapshot <list|create|delete> <image> ...\n");
		return 1;
	}

	if (strcmp(sub, "list") == 0) {
		rc = vtxfs_cli_open(&h, img, 0);
		if (rc)
			return 1;
		printf("Snapshots trên '%s':\n", img);
		rc = vtxfs_cli_for_each_subvol(&h, snap_list_cb, NULL);
		vtxfs_cli_close(&h, 0);
		return rc ? 1 : 0;
	}

	if (strcmp(sub, "create") == 0) {
		__u64 src = VTXFS_OID_FIRST_USER_SUBVOL, snap_id = 0;
		int read_only = 0;

		if (argc < 4) {
			fprintf(stderr,
				"usage: vtxfs snapshot create <image> <src_id> [-r]\n");
			return 1;
		}
		src = strtoull(argv[3], NULL, 10);
		if (argc > 4 && strcmp(argv[4], "-r") == 0)
			read_only = 1;
		rc = vtxfs_cli_open(&h, img, 1);
		if (rc)
			return 1;
		rc = vtx_snapshot_create(h.fs, src, NULL, read_only, &snap_id);
		if (rc) {
			fprintf(stderr, "vtxfs: tạo snapshot thất bại: %s\n",
				strerror(-rc));
			vtxfs_cli_close(&h, 0);
			return 1;
		}
		vtx_sync_checkpoint(h.fs);
		printf("Đã tạo snapshot %s từ ID %llu -> ID %llu\n",
		       read_only ? "RO" : "RW",
		       (unsigned long long)src,
		       (unsigned long long)snap_id);
		vtxfs_cli_close(&h, 1);
		return 0;
	}

	if (strcmp(sub, "delete") == 0) {
		__u64 id;

		if (argc < 4) {
			fprintf(stderr, "usage: vtxfs snapshot delete <image> <id>\n");
			return 1;
		}
		id = strtoull(argv[3], NULL, 10);
		rc = vtxfs_cli_open(&h, img, 1);
		if (rc)
			return 1;
		rc = vtx_subvol_destroy(h.fs, id);
		if (!rc)
			rc = vtx_reaper_run_all(h.fs);
		if (rc) {
			fprintf(stderr, "vtxfs: xoá snapshot %llu thất bại: %s\n",
				(unsigned long long)id, strerror(-rc));
			vtxfs_cli_close(&h, 0);
			return 1;
		}
		vtx_sync_checkpoint(h.fs);
		printf("Đã xoá snapshot ID %llu\n", (unsigned long long)id);
		vtxfs_cli_close(&h, 1);
		return 0;
	}

	fprintf(stderr, "vtxfs: lệnh con không rõ '%s'\n", sub);
	return 1;
}
