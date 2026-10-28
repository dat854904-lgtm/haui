// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-defrag.c — SCAFFOLD.
 *   scan (mặc định): duyệt EXTENT của subvolume, phân loại inline/shared/
 *                    fragmented và in báo cáo. CHỈ ĐỌC.
 *   start          : nếu có --force thì từ chối -EOPNOTSUPP vì engine P0
 *                    chưa có giao dịch "read -> alloc mới -> ghi -> thay
 *                    extent item" một cách nguyên tử qua VFS.
 *
 * Heuristic: block extent có physical_blocks < 4 được coi là dễ phân mảnh.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "common.h"

struct frag_stats {
	__u64 extents;
	__u64 inline_extents;
	__u64 shared_extents;
	__u64 fragmented;
	__u64 logical_bytes;
	__u64 fragmented_bytes;
	__u64 max_extent_blocks;
};

static int scan_extents(struct vtx_fs *fs, struct frag_stats *st)
{
	struct vtx_key cur = {0}, found;
	__u8 buf[VTXFS_BLOCK_SIZE];
	int first = 1, ret;

	memset(st, 0, sizeof(*st));
	for (;;) {
		const struct vtx_extent_item *e;
		size_t bytes = sizeof(buf);
		__u32 fl, pb;

		ret = vtx_rw_next(&fs->active_fs_tree, &cur, first,
				  &found, buf, &bytes);
		if (ret == -ENOENT)
			return 0;
		if (ret)
			return ret;
		first = 0;
		cur = found;
		if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_EXTENT)
			continue;
		if (bytes < sizeof(*e))
			return -EUCLEAN;
		e = (const struct vtx_extent_item *)buf;
		if (!vtx_extent_p0_value_valid(e, bytes))
			return -EUCLEAN;

		fl = vtx_cpu32(e->flags);
		pb = vtx_cpu32(e->physical_blocks);
		st->extents++;
		st->logical_bytes += vtx_cpu32(e->logical_bytes);

		if (fl & VTX_EXTENT_INLINE) {
			st->inline_extents++;
			continue;
		}
		if (fl & VTX_EXTENT_SHARED)
			st->shared_extents++;

		if (pb > st->max_extent_blocks)
			st->max_extent_blocks = pb;
		if (pb < 4U) {
			st->fragmented++;
			st->fragmented_bytes += vtx_cpu32(e->logical_bytes);
		}
	}
}

static int cmd_defrag_scan(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct frag_stats st;
	__u64 subvol;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr,
			"usage: vtxfs defrag scan <image> [--subvol=<id>]\n");
		return 1;
	}
	subvol = vtxfs_cli_opt_u64(argc - 1, argv + 1, "subvol",
				   VTXFS_OID_FIRST_USER_SUBVOL);
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	rc = vtxfs_cli_select_subvol(h.fs, subvol, VTXFS_SELECT_VIEW, NULL);
	if (rc)
		goto out;

	rc = scan_extents(h.fs, &st);
	if (rc) {
		fprintf(stderr, "defrag: scan lỗi: %s\n", strerror(-rc));
		goto out;
	}
	printf("Defrag scan '%s' subvol=%llu:\n", argv[0],
	       (unsigned long long)subvol);
	printf("  total extents     : %llu\n", (unsigned long long)st.extents);
	printf("  inline extents    : %llu\n", (unsigned long long)st.inline_extents);
	printf("  shared extents    : %llu (snapshot/reflink — KHÔNG ép)\n",
	       (unsigned long long)st.shared_extents);
	printf("  fragmented (<4blk): %llu (%llu byte)\n",
	       (unsigned long long)st.fragmented,
	       (unsigned long long)st.fragmented_bytes);
	printf("  logical bytes     : %llu\n", (unsigned long long)st.logical_bytes);
	printf("  max extent blocks : %llu\n",
	       (unsigned long long)st.max_extent_blocks);
	printf("  chế độ            : DRY-RUN (chưa ghi lại dữ liệu)\n");
out:
	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

static int cmd_defrag_start(const struct cmd_struct *c, int argc, char **argv)
{
	(void)c;
	(void)argv;
	if (argc < 1) {
		fprintf(stderr,
			"usage: vtxfs defrag start <image> [--subvol=<id>] [--force]\n");
		return 1;
	}
	fprintf(stderr,
		"defrag: ghi lại extent để chống phân mảnh là TODO (P1) — "
		"cần CoW write transaction + shared-extent unshare.\n"
		"        Dùng 'vtxfs defrag scan' để xem báo cáo.\n");
	return 1;	/* feature gate đóng, bất kể --force */
}

/* Cho phép gọi từ 'vtxfs filesystem defragment <image> [path]' */
int cmd_defragment(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	const char *img = argc >= 1 ? argv[0] : NULL;
	const char *path = argc >= 2 ? argv[1] : NULL;
	struct frag_stats st;
	int rc;

	(void)c;
	if (!img) {
		fprintf(stderr,
			"usage: vtxfs filesystem defragment <image> [path]\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, img, 0);
	if (rc)
		return 1;
	rc = vtxfs_cli_select_subvol(h.fs, VTXFS_OID_FIRST_USER_SUBVOL,
				     VTXFS_SELECT_VIEW, NULL);
	if (rc) {
		vtxfs_cli_close(&h, 0);
		return 1;
	}
	rc = scan_extents(h.fs, &st);
	if (rc) {
		fprintf(stderr, "defrag: scan lỗi: %s\n", strerror(-rc));
		vtxfs_cli_close(&h, 0);
		return 1;
	}
	printf("vtxfs defragment%s%s:\n", path ? " '" : "", path ? path : "");
	if (path)
		printf("  (lưu ý: scaffold quét TOÀN bộ subvolume, chưa lọc theo path)\n");
	printf("  block_extents   : %llu\n", (unsigned long long)st.extents);
	printf("  total_blocks    : %llu\n", (unsigned long long)st.logical_bytes / VTXFS_BLOCK_SIZE);
	printf("  fragmented (<4) : %llu (%.1f%%)\n",
	       (unsigned long long)st.fragmented,
	       st.extents ? 100.0 * (double)st.fragmented / (double)st.extents : 0.0);
	printf("  (Ghi lại để chống phân mảnh là TODO — chưa thực thi ở P0)\n");
	vtxfs_cli_close(&h, 0);
	return 0;
}

static const struct cmd_struct defrag_cmds[] = {
	{ "scan",  cmd_defrag_scan,  NULL, "<image> [--subvol=<id>]" },
	{ "start", cmd_defrag_start, NULL, "<image> [--subvol=<id>] [--force]" },
};

const struct cmd_group defrag_cmd_group = {
	.name = "defrag",
	.usage = "Scaffold: scan thật (dry-run); start còn TODO",
	.cmds = defrag_cmds,
	.ncmds = sizeof(defrag_cmds) / sizeof(defrag_cmds[0]),
};
