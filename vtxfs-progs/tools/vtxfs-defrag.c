// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs-defrag — SCAFFOLD. Viết lại các extent bị phân mảnh thành extent
 * liên tục. P0 duyệt extent để BÁO cáo mức phân mảnh, chưa ghi lại.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vtxfs_engine.h"

int main(int argc, char **argv)
{
	struct vtx_bdev bdev;
	struct vtx_fs *fs;
	const char *img = argc > 1 ? argv[1] : NULL;
	struct vtx_key cur = {0}, found;
	__u8 buf[4096];
	size_t bytes;
	int first = 1, rc;
	__u64 extents = 0, fragmented = 0;

	if (!img) {
		fprintf(stderr, "usage: vtxfs-defrag <image> [path]\n");
		return 1;
	}
	rc = vtx_file_bdev_open(&bdev, img, 1, 0);
	if (rc)
		return 1;
	fs = vtx_fs_alloc();
	if (!fs) {
		vtx_file_bdev_close(&bdev);
		return 1;
	}
	rc = vtx_mount(fs, &bdev, 0);
	if (rc) {
		fprintf(stderr, "vtxfs-defrag: mount thất bại: %s\n",
			strerror(-rc));
		goto out;
	}
	rc = vtx_subvol_switch(fs, VTXFS_OID_FIRST_USER_SUBVOL);
	if (rc)
		goto unmount;
	/* Đếm extent và heuristic phân mảnh (extent < 4 block coi là mảnh). */
	for (;;) {
		const struct vtx_extent_item *ext;

		bytes = sizeof(buf);
		rc = vtx_rw_next(&fs->active_fs_tree, &cur, first, &found,
				 buf, &bytes);
		if (rc == -ENOENT) {
			rc = 0;
			break;
		}
		if (rc)
			break;
		first = 0;
		cur = found;
		if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_EXTENT)
			continue;
		ext = (const struct vtx_extent_item *)buf;
		if (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)
			continue;
		extents++;
		if (vtx_cpu32(ext->physical_blocks) < 4)
			fragmented++;
	}
	printf("vtxfs-defrag: %llu extent, %llu có thể phân mảnh\n",
	       (unsigned long long)extents,
	       (unsigned long long)fragmented);
	printf("  (Ghi lại để chống phân mảnh là TODO — chưa thực thi ở P0)\n");
unmount:
	vtx_unmount(fs);
out:
	vtx_fs_free(fs);
	vtx_file_bdev_close(&bdev);
	return rc ? 1 : 0;
}
