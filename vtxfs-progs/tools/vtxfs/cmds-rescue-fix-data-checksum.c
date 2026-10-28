// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-rescue-fix-data-checksum.c
 * Tính lại CRC32C từ block dữ liệu thực trên đĩa và ghi đè checksum đã lưu
 * trong vtx_extent_item. Giả định DỮ LIỆU đúng, chỉ checksum bị sai/lỗi thời
 * (giống `btrfs rescue fix-data-checksum`). Nếu dữ liệu thật sự hỏng, lệnh này
 * sẽ "hợp thức hoá" CRC của dữ liệu hỏng — hãy đọc kỹ cảnh báo đầu ra.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "common.h"

struct fix_entry {
	struct vtx_key key;
	__u16 len;
	__u8 *item;
};

/* Tính lại CRC cho một subvolume; trả số extent đã sửa qua *fixed_out. */
static int fix_subvol_checksums(struct vtx_fs *fs, __u64 subvol_id,
				__u64 *fixed_out)
{
	struct vtx_key cur = {0}, found;
	struct fix_entry *fixes = NULL;
	size_t nfix = 0, capfix = 0;
	__u8 *ebuf, *dbuf;
	size_t bytes;
	int first = 1, ret = 0;
	__u64 fixed = 0;

	ret = vtx_subvol_switch(fs, subvol_id);
	if (ret)
		return ret;
	ebuf = (__u8 *)malloc(VTXFS_BLOCK_SIZE);
	dbuf = (__u8 *)malloc(VTXFS_BLOCK_SIZE);
	if (!ebuf || !dbuf) {
		free(ebuf);
		free(dbuf);
		return -ENOMEM;
	}

	/* Pass 1: duyệt mọi extent, tính lại CRC, gom các item lệch. */
	cur.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(1, 0));
	cur.offset = 0;
	for (;;) {
		const struct vtx_extent_item *ext;
		__u32 nb, b;
		int changed = 0;

		bytes = VTXFS_BLOCK_SIZE;
		ret = vtx_rw_next(&fs->active_fs_tree, &cur, first,
				  &found, ebuf, &bytes);
		if (ret == -ENOENT) {
			ret = 0;
			break;
		}
		if (ret)
			break;
		first = 0;
		cur = found;
		if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_EXTENT)
			continue;
		ext = (const struct vtx_extent_item *)ebuf;
		if (!vtx_extent_p0_value_valid(ext, bytes) ||
		    (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE))
			continue;
		nb = vtx_cpu32(ext->physical_blocks);
		{
			const __le32 *stored = (const __le32 *)ext->tail;

			for (b = 0; b < nb; b++) {
				__u64 pblk;

				if (vtx_vag_vblk_to_pblk(&fs->vag,
						vtx_cpu64(ext->vblk) + b,
						&pblk) != 0)
					break;
				if (fs->bdev->ops->read_blocks(fs->bdev,
						pblk, 1, dbuf) != 0)
					break;
				if (vtx_crc32c(dbuf, VTXFS_BLOCK_SIZE) !=
				    vtx_cpu32(stored[b])) {
					changed = 1;
					break;
				}
			}
		}
		if (!changed)
			continue;
		/* Dựng bản sao đã hiệu đính checksum. */
		if (nfix >= capfix) {
			struct fix_entry *tmp;

			capfix = capfix ? capfix * 2U : 16U;
			tmp = (struct fix_entry *)realloc(fixes,
					capfix * sizeof(*fixes));
			if (!tmp) {
				ret = -ENOMEM;
				break;
			}
			fixes = tmp;
		}
		fixes[nfix].item = (__u8 *)malloc(bytes);
		if (!fixes[nfix].item) {
			ret = -ENOMEM;
			break;
		}
		memcpy(fixes[nfix].item, ebuf, bytes);
		{
			struct vtx_extent_item *ne =
				(struct vtx_extent_item *)fixes[nfix].item;
			__le32 *ntail = (__le32 *)ne->tail;

			for (b = 0; b < nb; b++) {
				__u64 pblk;

				vtx_vag_vblk_to_pblk(&fs->vag,
					vtx_cpu64(ext->vblk) + b, &pblk);
				fs->bdev->ops->read_blocks(fs->bdev,
					pblk, 1, dbuf);
				ntail[b] = vtx_le32(
					vtx_crc32c(dbuf, VTXFS_BLOCK_SIZE));
			}
		}
		fixes[nfix].key = found;
		fixes[nfix].len = (__u16)bytes;
		nfix++;
	}

	/* Pass 2: áp dụng các bản hiệu đính (cùng kích thước, dùng upsert). */
	if (ret == 0) {
		size_t i;

		for (i = 0; i < nfix; i++) {
			ret = vtx_rw_upsert(&fs->active_fs_tree,
					    &fixes[i].key, fixes[i].item,
					    fixes[i].len);
			if (ret == 0)
				fixed++;
			else
				break;
		}
	}
	{
		size_t i;

		for (i = 0; i < nfix; i++)
			free(fixes[i].item);
		free(fixes);
	}
	free(ebuf);
	free(dbuf);
	*fixed_out = fixed;
	return ret;
}

struct collect_ctx {
	__u64 *ids;
	int n, cap;
};

static int collect_subvol_cb(const struct vtx_subvol_root_item *it, __u64 id,
			     void *arg)
{
	struct collect_ctx *c = (struct collect_ctx *)arg;

	(void)it;
	if (c->n < c->cap)
		c->ids[c->n++] = id;
	return 0;
}

int cmd_rescue_fix_data_checksum(const struct cmd_struct *cmd,
				 int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 want_subvol = 0, total_fixed = 0;
	int has_subvol, rc, i, apply = 0, yes = 0;

	(void)cmd;
	if (argc < 1) {
		fprintf(stderr,
			"usage: vtxfs rescue fix-data-checksum <image> [--subvol=N] --apply --yes\n");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--apply") == 0)
			apply = 1;
		else if (strcmp(argv[i], "--yes") == 0)
			yes = 1;
		else if (strncmp(argv[i], "--subvol=", 9) != 0) {
			fprintf(stderr, "fix-data-checksum: unknown option '%s'\n",
				argv[i]);
			return 1;
		}
	}
	if (!apply || !yes) {
		fprintf(stderr,
			"fix-data-checksum: REFUSED. Replacing checksums can legitimise "
			"CORRUPTED FILE DATA. Make a verified offline backup first. "
			"Explicit --apply --yes is required; no repair was attempted.\n");
		return 2;
	}
	fprintf(stderr,
		"WARNING: overwriting stored CRC based on current disk bytes; "
		"data corruption will NOT be repaired. Keep an offline backup.\n");
	has_subvol = vtxfs_cli_parse_subvol(argc - 1, argv + 1, &want_subvol);
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;

	if (has_subvol) {
		__u64 f = 0;

		rc = fix_subvol_checksums(h.fs, want_subvol, &f);
		total_fixed = f;
	} else {
		__u64 ids[256];
		struct collect_ctx c = { ids, 0, 256 };
		int i;

		rc = vtxfs_cli_for_each_subvol(&h, collect_subvol_cb, &c);
		for (i = 0; rc == 0 && i < c.n; i++) {
			__u64 f = 0;

			rc = fix_subvol_checksums(h.fs, ids[i], &f);
			total_fixed += f;
		}
	}
	if (rc == 0)
		rc = vtx_sync_checkpoint(h.fs);
	if (rc) {
		printf("fix-data-checksum lỗi: %s\n", strerror(-rc));
	} else {
		printf("Đã ghi lại CRC32C cho %llu extent "
		       "(giả định dữ liệu đúng; nếu dữ liệu thật sự hỏng, "
		       "CRC mới chỉ hợp thức hoá dữ liệu hỏng).\n",
		       (unsigned long long)total_fixed);
	}
	vtxfs_cli_close(&h, 1);
	return rc ? 1 : 0;
}
