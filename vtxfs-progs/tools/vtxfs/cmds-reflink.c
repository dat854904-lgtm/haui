// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <stdio.h>
#include <string.h>
#include "commands.h"
#include "common.h"

int cmd_reflink(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 src_ino = 0, dst_ino = 0, sz = 0;
	__u8 ft = 0;
	struct vtx_inode_item st;
	int rc;

	(void)c;
	if (argc < 3) {
		fprintf(stderr, "usage: vtxfs reflink <image> <src> <dst>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;
	rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL,
			     argv[1], &src_ino, &ft);
	if (!rc)
		rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL,
				     argv[2], &dst_ino, &ft);
	if (!rc)
		rc = vtx_stat(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, src_ino, &st);
	if (!rc) {
		sz = vtx_cpu64(st.size_bytes);
		sz &= ~(__u64)(VTXFS_BLOCK_SIZE - 1); /* block-aligned */
		rc = vtx_reflink(h.fs, VTXFS_OID_FIRST_USER_SUBVOL,
				 src_ino, 0, dst_ino, 0, sz);
	}
	if (!rc)
		rc = vtx_sync_checkpoint(h.fs);
	if (rc)
		printf("reflink lỗi: %s\n", strerror(-rc));
	else
		printf("reflink OK (%llu bytes)\n", (unsigned long long)sz);
	vtxfs_cli_close(&h, 1);
	return rc ? 1 : 0;
}
