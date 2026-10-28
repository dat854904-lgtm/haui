// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-restore.c — khôi phục nội dung file từ ảnh hỏng bằng cách đọc thô
 * các block dữ liệu qua VAG (KHÔNG kiểm tra CRC32C), dùng RO walker. Hữu ích
 * khi fsck báo lỗi CRC nhưng vẫn muốn lấy lại dữ liệu.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "commands.h"
#include "common.h"

/* Đọc thô một block dữ liệu theo vblk (không kiểm tra CRC). */
static int read_data_block_raw(struct vtx_fs *fs, __u64 vblk, void *buf)
{
	__u64 pblk = 0;
	int ret = vtx_vag_vblk_to_pblk(&fs->vag, vblk, &pblk);

	if (ret)
		return ret;
	return fs->bdev->ops->read_blocks(fs->bdev, pblk, 1, buf);
}

/* Trích xuất một file (ino) ra outfile, đọc thô extent, tôn trọng size. */
static int restore_one_file(struct vtx_fs *fs, __u64 ino, const char *outfile)
{
	struct vtx_inode_item st;
	struct vtx_key cur, found;
	FILE *out;
	__u8 *ebuf, *dbuf;
	size_t bytes;
	__u64 size, written = 0;
	int first = 1, ret;

	ret = vtx_stat(fs, fs->active_subvol_id, ino, &st);
	if (ret)
		return ret;
	if ((vtx_cpu32(st.mode) & 0170000U) != 0100000U)
		return -EINVAL;
	size = vtx_cpu64(st.size_bytes);
	out = fopen(outfile, "wb");
	if (!out)
		return -EIO;
	ebuf = (__u8 *)malloc(VTXFS_BLOCK_SIZE);
	dbuf = (__u8 *)malloc(VTXFS_BLOCK_SIZE);
	if (!ebuf || !dbuf) {
		free(ebuf);
		free(dbuf);
		fclose(out);
		return -ENOMEM;
	}
	cur.oid_and_type =
		vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
	cur.offset = 0;
	while (written < size) {
		const struct vtx_extent_item *ext;
		__u64 ext_start;

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
		if (VTX_KEY_GET_OID(vtx_cpu64(found.oid_and_type)) != ino ||
		    VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_EXTENT)
			break;
		ext = (const struct vtx_extent_item *)ebuf;
		ext_start = vtx_cpu64(found.offset);
		if (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE) {
			__u64 to_write = vtx_cpu32(ext->payload_bytes);

			if (ext_start + to_write > size)
				to_write = size - ext_start;
			fwrite(ext->tail, 1, (size_t)to_write, out);
			written += to_write;
		} else {
			__u32 nb = vtx_cpu32(ext->physical_blocks), b;

			for (b = 0; b < nb && written < size; b++) {
				__u64 bstart =
					ext_start + (__u64)b * VTXFS_BLOCK_SIZE;
				size_t chunk = VTXFS_BLOCK_SIZE;

				if (bstart >= size)
					break;
				ret = read_data_block_raw(fs,
						vtx_cpu64(ext->vblk) + b, dbuf);
				if (ret)
					goto done;
				if (bstart + chunk > size)
					chunk = (size_t)(size - bstart);
				fwrite(dbuf, 1, chunk, out);
				written += chunk;
			}
		}
	}
done:
	free(ebuf);
	free(dbuf);
	fclose(out);
	return (written == size) ? 0 : -EIO;
}

static int list_dir_recursive(struct vtx_fs *fs, __u64 dir_ino,
			      const char *prefix)
{
	__u64 cookie = 0;
	struct vtx_dirent *ents = (struct vtx_dirent *)malloc(sizeof(struct vtx_dirent) * 64);
	size_t count, i;
	int ret = 0;

	if (!ents)
		return -ENOMEM;

	for (;;) {
		ret = vtx_readdir(fs, fs->active_subvol_id, dir_ino,
				  &cookie, ents, 64, &count);
		if (ret != 0 || count == 0)
			break;
		for (i = 0; i < count; i++) {
			char path[512];

			snprintf(path, sizeof(path), "%s/%s",
				 prefix, ents[i].name);
			if (ents[i].file_type == VTX_FT_DIR) {
				printf("DIR  %s\n", path);
				list_dir_recursive(fs, ents[i].ino, path);
			} else if (ents[i].file_type == VTX_FT_REG) {
				struct vtx_inode_item st;

				if (vtx_stat(fs, fs->active_subvol_id,
					     ents[i].ino, &st) == 0)
					printf("FILE %s (ino=%llu size=%llu)\n",
					       path,
					       (unsigned long long)ents[i].ino,
					       (unsigned long long)
					       vtx_cpu64(st.size_bytes));
			} else {
				printf("OTH  %s (type=%u)\n",
				       path, ents[i].file_type);
			}
		}
	}
	free(ents);
	return ret;
}

static int cmd_restore_list(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u64 subvol = 0;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs restore list <image> [--subvol=N]\n");
		return 1;
	}
	vtxfs_cli_parse_subvol(argc - 1, argv + 1, &subvol);
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	if (subvol)
		rc = vtx_subvol_switch(h.fs, subvol);
	if (rc == 0)
		rc = list_dir_recursive(h.fs, VTXFS_INO_ROOT_DIR, "");
	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

static int cmd_restore_extract(const struct cmd_struct *c, int argc,
			       char **argv)
{
	struct vtxfs_handle h;
	__u64 subvol = 0, ino = 0;
	__u8 ftype = 0;
	int rc;

	(void)c;
	if (argc < 3) {
		fprintf(stderr,
			"usage: vtxfs restore extract <image> <path> <outfile> [--subvol=N]\n");
		return 1;
	}
	vtxfs_cli_parse_subvol(argc - 3, argv + 3, &subvol);
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	if (subvol) {
		rc = vtx_subvol_switch(h.fs, subvol);
		if (rc) {
			vtxfs_cli_close(&h, 0);
			return 1;
		}
	}
	rc = vtx_lookup_path(h.fs, h.fs->active_subvol_id, argv[1],
			     &ino, &ftype);
	if (rc == 0)
		rc = restore_one_file(h.fs, ino, argv[2]);
	printf(rc ? "restore '%s' lỗi: %s\n"
		  : "Đã khôi phục '%s' -> '%s'\n",
	       rc ? strerror(-rc) : argv[1], rc ? "" : argv[2]);
	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

/* Tái tạo đệ quy cây thư mục từ dir_ino ra outdir. */
static int restore_dir_recursive(struct vtx_fs *fs, __u64 dir_ino,
				 const char *outdir)
{
	__u64 cookie = 0;
	struct vtx_dirent *ents = (struct vtx_dirent *)malloc(sizeof(struct vtx_dirent) * 64);
	size_t count, i;
	int ret = 0;

	if (!ents)
		return -ENOMEM;

	for (;;) {
		ret = vtx_readdir(fs, fs->active_subvol_id, dir_ino,
				  &cookie, ents, 64, &count);
		if (ret != 0 || count == 0)
			break;
		for (i = 0; i < count; i++) {
			char path[1024];

			snprintf(path, sizeof(path), "%s/%s",
				 outdir, ents[i].name);
			if (ents[i].file_type == VTX_FT_DIR) {
				mkdir(path, 0755);
				restore_dir_recursive(fs, ents[i].ino, path);
			} else if (ents[i].file_type == VTX_FT_REG) {
				restore_one_file(fs, ents[i].ino, path);
			} else if (ents[i].file_type == VTX_FT_SYMLINK) {
				char tgt[VTXFS_MAX_INLINE_DATA + 1];
				size_t tlen = 0;

				if (vtx_readlink(fs, fs->active_subvol_id,
						 ents[i].ino, tgt,
						 sizeof(tgt), &tlen) == 0)
					(void)symlink(tgt, path);
			} else {
				printf("  (bỏ qua mục đặc biệt '%s' type=%u)\n",
				       ents[i].name, ents[i].file_type);
			}
		}
	}
	free(ents);
	return ret;
}

static int cmd_restore_restoreall(const struct cmd_struct *c, int argc,
				  char **argv)
{
	struct vtxfs_handle h;
	__u64 subvol = 0;
	int rc;

	(void)c;
	if (argc < 2) {
		fprintf(stderr,
			"usage: vtxfs restore restore-all <image> <outdir> [--subvol=N]\n");
		return 1;
	}
	vtxfs_cli_parse_subvol(argc - 2, argv + 2, &subvol);
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	if (subvol) {
		rc = vtx_subvol_switch(h.fs, subvol);
		if (rc) {
			vtxfs_cli_close(&h, 0);
			return 1;
		}
	}
	mkdir(argv[1], 0755);
	rc = restore_dir_recursive(h.fs, VTXFS_INO_ROOT_DIR, argv[1]);
	printf(rc ? "restore-all thất bại: %s\n"
		  : "Đã tái tạo cây thư mục vào '%s'\n",
	       rc ? strerror(-rc) : argv[1]);
	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

static const struct cmd_struct restore_cmds[] = {
	{ "list",        cmd_restore_list,       NULL, "<image> [--subvol=N]" },
	{ "extract",     cmd_restore_extract,    NULL,
	  "<image> <path> <outfile> [--subvol=N]" },
	{ "restore-all", cmd_restore_restoreall, NULL,
	  "<image> <outdir> [--subvol=N]" },
};

const struct cmd_group restore_cmd_group = {
	.name = "restore",
	.usage = "Khôi phục dữ liệu từ ảnh hỏng bằng RO walker (bỏ qua CRC)",
	.cmds = restore_cmds,
	.ncmds = sizeof(restore_cmds) / sizeof(restore_cmds[0]),
};
