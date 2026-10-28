// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-property.c — Đọc và cấu hình thuộc tính hệ thống tệp: label, ro, compression.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "common.h"
#include "vtxfs_csum.h"

static int set_subvol_ro(struct vtx_fs *fs, __u64 subvol_id, int ro)
{
	struct vtx_key k;
	struct vtx_subvol_root_item item;
	size_t bytes = sizeof(item);
	__u64 flags;
	int ret;

	k.oid_and_type =
		vtx_le64(VTX_MAKE_OID_TYPE(subvol_id, VTX_TYPE_SUBVOL_ROOT));
	k.offset = 0;
	ret = vtx_rw_lookup(&fs->root_tree, &k, &item, &bytes);
	if (ret)
		return ret;
	flags = vtx_cpu64(item.flags);
	if (ro)
		flags |= VTX_SUBVOL_RDONLY;
	else
		flags &= ~VTX_SUBVOL_RDONLY;
	item.flags = vtx_le64(flags);
	return vtx_rw_replace(&fs->root_tree, &k, &item, sizeof(item));
}

static int cmd_prop_get(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	int rc;

	(void)c;
	if (argc < 2) {
		fprintf(stderr, "Cách dùng: vtxfs property get <image|device> <label|ro|compression> [subvol]\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;

	if (!strcmp(argv[1], "label")) {
		printf("%s\n", h.fs->super_blk.hdr.label);
	} else if (!strcmp(argv[1], "ro") && argc >= 3) {
		__u64 id = strtoull(argv[2], NULL, 10);
		struct vtx_key k;
		struct vtx_subvol_root_item sub;
		size_t bytes = sizeof(sub);

		k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(id, VTX_TYPE_SUBVOL_ROOT));
		k.offset = 0;
		rc = vtx_rw_lookup(&h.fs->root_tree, &k, &sub, &bytes);
		if (!rc)
			printf("ro=%s\n",
			       (vtx_cpu64(sub.flags) & VTX_SUBVOL_RDONLY)
			       ? "true" : "false");
	} else if (!strcmp(argv[1], "compression")) {
		__u64 incompat = vtx_cpu64(h.fs->super_blk.hdr.incompat_flags);
		if (incompat & VTXFS_FEAT_INCOMPAT_COMPRESS) {
			__u8 algo = h.fs->super_blk.default_compression;
			const char *name = (algo == VTX_COMPR_LZ4) ? "lz4" :
			                   (algo == VTX_COMPR_ZLIB) ? "zlib" : "zstd";
			printf("compression=%s (hỗ trợ zstd, lz4, zlib)\n", name);
		} else {
			printf("compression=none (chưa bật nén)\n");
		}
	} else {
		fprintf(stderr, "Thuộc tính không hợp lệ: '%s' (hỗ trợ: label, ro, compression)\n", argv[1]);
		rc = -EINVAL;
	}

	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

static int cmd_prop_set(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	int rc = 0;

	(void)c;
	if (argc < 2) {
		fprintf(stderr,
			"Cách dùng:\n"
			"  vtxfs property set <image|device> label <tên_nhãn>\n"
			"  vtxfs property set <image|device> ro <subvol_id> <true|false>\n"
			"  vtxfs property set <image|device> compression <zstd|lz4|zlib|none>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;

	if (strcmp(argv[1], "label") == 0 && argc >= 3) {
		size_t n = strlen(argv[2]);

		if (n >= VTXFS_LABEL_SIZE)
			n = VTXFS_LABEL_SIZE - 1U;
		memset(h.fs->super_blk.hdr.label, 0, VTXFS_LABEL_SIZE);
		memcpy(h.fs->super_blk.hdr.label, argv[2], n);
		h.fs->super_blk.hdr.super_generation = vtx_le64(
			vtx_cpu64(h.fs->super_blk.hdr.super_generation) + 1);
		vtx_super_checksum_compute(&h.fs->super_blk);
		rc = h.bdev.ops->write_blocks(&h.bdev, VTXFS_SUPER_RING_PBLK,
					      1, &h.fs->super_blk);
		if (rc == 0)
			rc = h.bdev.ops->write_blocks(&h.bdev,
				VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK,
				1, &h.fs->super_blk);
	} else if (strcmp(argv[1], "ro") == 0 && argc >= 4) {
		__u64 subvol_id = strtoull(argv[2], NULL, 10);
		int ro = (strcmp(argv[3], "true") == 0 ||
			  strcmp(argv[3], "1") == 0);

		rc = set_subvol_ro(h.fs, subvol_id, ro);
		if (rc == 0)
			rc = vtx_sync_checkpoint(h.fs);
	} else if (strcmp(argv[1], "compression") == 0 && argc >= 3) {
		const char *val = argv[2];

		if (!strcmp(val, "none") || !strcmp(val, "off") || !strcmp(val, "0")) {
			rc = vtx_migrate_disable_compression(h.fs);
			if (rc == -EBUSY) {
				fprintf(stderr, "Lỗi: Không thể tắt nén: vẫn còn extent nén trên hệ thống tập tin (hãy defrag/sao chép trước).\n");
			} else if (rc == 0) {
				printf("Đã tắt nén dữ liệu và gỡ bỏ VTXFS_FEAT_INCOMPAT_COMPRESS (compression=none).\n");
			}
		} else {
			__u8 algo = 0;
			if (!strcmp(val, "zstd"))
				algo = VTX_COMPR_ZSTD;
			else if (!strcmp(val, "lz4"))
				algo = VTX_COMPR_LZ4;
			else if (!strcmp(val, "zlib"))
				algo = VTX_COMPR_ZLIB;
			else {
				fprintf(stderr, "Thuật toán nén '%s' không hỗ trợ (hỗ trợ: zstd, lz4, zlib, none)\n", val);
				rc = -EINVAL;
			}
			if (rc == 0) {
				rc = vtx_migrate_enable_compression(h.fs, algo);
				if (rc == 0)
					printf("Đã kích hoạt nén trong suốt '%s' (VTXFS_FEAT_INCOMPAT_COMPRESS).\n", val);
			}
		}
	} else {
		rc = -EINVAL;
	}

	if (rc)
		printf("property set lỗi: %s\n", strerror(-rc));
	else
		printf("property set OK\n");

	vtxfs_cli_close(&h, rc ? 0 : 1);
	return rc ? 1 : 0;
}

static const struct cmd_struct prop_cmds[] = {
	{ "get", cmd_prop_get, NULL, "<image|device> <label|ro|compression> [subvol]" },
	{ "set", cmd_prop_set, NULL, "<image|device> <label <val>|ro <subvol> <bool>|compression <algo>>" },
};

const struct cmd_group property_cmd_group = {
	.name = "property",
	.usage = "Đọc và thiết lập thuộc tính label, ro và compression",
	.cmds = prop_cmds,
	.ncmds = sizeof(prop_cmds) / sizeof(prop_cmds[0]),
};
