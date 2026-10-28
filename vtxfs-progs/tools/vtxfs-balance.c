// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs-balance — SCAFFOLD. Cân bằng/định vị lại block giữa các VAG/thiết bị.
 * Yêu cầu VTXFS_FEAT_INCOMPAT_MULTIDEV (DEFERRED) nên P0 chỉ kiểm tra
 * feature gate và báo trạng thái, KHÔNG di chuyển dữ liệu.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "vtxfs_engine.h"

int main(int argc, char **argv)
{
	struct vtxfs_handle_local {
		struct vtx_bdev bdev;
		struct vtx_fs *fs;
	} h;
	const char *img = argc > 1 ? argv[1] : NULL;
	int rc;

	if (!img) {
		fprintf(stderr, "usage: vtxfs-balance <image> [--full|--usage=N]\n");
		return 1;
	}
	rc = vtx_file_bdev_open(&h.bdev, img, 1, 0);
	if (rc)
		return 1;
	h.fs = vtx_fs_alloc();
	if (!h.fs) {
		vtx_file_bdev_close(&h.bdev);
		return 1;
	}
	rc = vtx_mount(h.fs, &h.bdev, 1);
	if (rc) {
		fprintf(stderr, "vtxfs-balance: mount thất bại: %s\n",
			strerror(-rc));
		goto out;
	}
	if (!(vtx_cpu64(h.fs->super_blk.hdr.incompat_flags) &
	      VTXFS_FEAT_INCOMPAT_MULTIDEV)) {
		printf("vtxfs-balance: thiết bị đơn, không có gì để cân bằng.\n");
		printf("  free_blocks=%llu used_blocks=%llu total_vags=%u\n",
		       (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.free_blocks),
		       (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.used_blocks),
		       h.fs->vag.total_vags);
		printf("  (MULTIDEV/TIERING là DEFERRED — chưa thực thi)\n");
		rc = 0;
	} else {
		fprintf(stderr, "vtxfs-balance: MULTIDEV chưa được hỗ trợ ở P0\n");
		rc = -EOPNOTSUPP;
	}
	vtx_unmount(h.fs);
out:
	vtx_fs_free(h.fs);
	vtx_file_bdev_close(&h.bdev);
	return rc ? 1 : 0;
}
