// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

int vtxfs_cli_open(struct vtxfs_handle *h, const char *path, int rw)
{
	int rc;

	memset(h, 0, sizeof(*h));
	h->path = path;
	h->rw = rw;
	rc = vtx_file_bdev_open(&h->bdev, path, rw ? 1 : 0, 0);
	if (rc) {
		fprintf(stderr, "vtxfs: mở '%s' thất bại: %s\n",
			path, strerror(-rc));
		return rc;
	}
	h->fs = vtx_fs_alloc();
	if (!h->fs) {
		vtx_file_bdev_close(&h->bdev);
		return -ENOMEM;
	}
	rc = vtx_mount(h->fs, &h->bdev, rw ? 0 : 1);
	if (rc) {
		fprintf(stderr, "vtxfs: mount '%s' thất bại: %s\n",
			path, strerror(-rc));
		vtx_fs_free(h->fs);
		vtx_file_bdev_close(&h->bdev);
		h->fs = NULL;
		return rc;
	}
	return 0;
}

void vtxfs_cli_close(struct vtxfs_handle *h, int commit)
{
	if (!h->fs)
		return;
	if (!commit)
		h->fs->read_only = 1;
	vtx_unmount(h->fs);
	vtx_fs_free(h->fs);
	vtx_file_bdev_close(&h->bdev);
	h->fs = NULL;
}

void vtxfs_cli_print_uuid(const __u8 u[16])
{
	printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-"
	       "%02x%02x-%02x%02x%02x%02x%02x%02x",
	       u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
	       u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

const char *vtxfs_cli_subvol_flags_str(__u64 flags, char *buf, size_t n)
{
	buf[0] = '\0';
	if (flags & VTX_SUBVOL_RDONLY)
		strncat(buf, "ro,", n - strlen(buf) - 1);
	if (flags & VTX_SUBVOL_REAPING)
		strncat(buf, "reaping,", n - strlen(buf) - 1);
	if (flags & VTX_SUBVOL_QUOTA_ENABLED)
		strncat(buf, "quota,", n - strlen(buf) - 1);
	if (buf[0])
		buf[strlen(buf) - 1] = '\0';
	else
		snprintf(buf, n, "-");
	return buf;
}

int vtxfs_cli_for_each_subvol(struct vtxfs_handle *h,
	int (*cb)(const struct vtx_subvol_root_item *, __u64, void *),
	void *arg)
{
	struct vtx_fs *fs = h->fs;
	struct vtx_key cur = {0}, found;
	struct vtx_subvol_root_item item;
	size_t bytes;
	int first = 1, ret, stop = 0;

	for (;;) {
		bytes = sizeof(item);
		ret = vtx_rw_next(&fs->root_tree, &cur, first, &found,
				  &item, &bytes);
		if (ret == -ENOENT)
			return 0;
		if (ret)
			return ret;
		first = 0;
		cur = found;
		if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_SUBVOL_ROOT || bytes != sizeof(item))
			continue;
		stop = cb(&item,
			  VTX_KEY_GET_OID(vtx_cpu64(found.oid_and_type)), arg);
		if (stop)
			return stop;
	}
}

struct find_uuid_ctx {
	const __u8 *uuid;
	__u64 found;
};

static int match_uuid_cb(const struct vtx_subvol_root_item *it, __u64 id,
			 void *arg)
{
	struct find_uuid_ctx *c = arg;

	(void)id;
	if (memcmp(it->uuid, c->uuid, VTXFS_UUID_SIZE) == 0) {
		c->found = vtx_cpu64(it->subvol_id);
		return 1;
	}
	return 0;
}

int vtxfs_cli_find_subvol_by_uuid(struct vtxfs_handle *h,
	const __u8 uuid[16], __u64 *id_out)
{
	struct find_uuid_ctx ctx = { uuid, 0 };
	int ret = vtxfs_cli_for_each_subvol(h, match_uuid_cb, &ctx);
	if (ret == 1) {
		*id_out = ctx.found;
		return 0;
	}
	return ret ? ret : -ENOENT;
}

const char *vtxfs_cli_ftype_name(__u8 t)
{
	switch (t) {
	case VTX_FT_REG: return "reg";
	case VTX_FT_DIR: return "dir";
	case VTX_FT_CHRDEV: return "chr";
	case VTX_FT_BLKDEV: return "blk";
	case VTX_FT_FIFO: return "fifo";
	case VTX_FT_SOCK: return "sock";
	case VTX_FT_SYMLINK: return "symlink";
	case VTX_FT_SUBVOL: return "subvol";
	default: return "?";
	}
}

const char *vtxfs_cli_opt(int argc, char **argv, const char *key)
{
	size_t klen = strlen(key);
	int i;

	for (i = 0; i < argc; i++) {
		if (!strncmp(argv[i], "--", 2) &&
		    !strncmp(argv[i] + 2, key, klen) &&
		    argv[i][2 + klen] == '=')
			return argv[i] + 2 + klen + 1;
		if (!strcmp(argv[i], "--") ||
		    strncmp(argv[i], "--", 2))
			continue;
		if (!strcmp(argv[i] + 2, key) && i + 1 < argc)
			return argv[i + 1];
	}
	return NULL;
}

__u64 vtxfs_cli_opt_u64(int argc, char **argv, const char *key,
			__u64 fallback)
{
	const char *s = vtxfs_cli_opt(argc, argv, key);
	char *end = NULL;
	unsigned long long v;

	if (!s || !*s)
		return fallback;
	v = strtoull(s, &end, 10);
	if (!end || *end != '\0') {
		fprintf(stderr, "vtxfs: giá trị --%s không hợp lệ: '%s'\n",
			key, s);
		return fallback;
	}
	return (__u64)v;
}

int vtxfs_cli_flag(int argc, char **argv, const char *key)
{
	int i;

	for (i = 0; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == '-' &&
		    strcmp(argv[i] + 2, key) == 0)
			return 1;
	}
	return 0;
}

int vtxfs_cli_parse_subvol(int argc, char **argv, __u64 *subvol_out)
{
	int i;

	*subvol_out = 0;
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--subvol=", 9) == 0) {
			*subvol_out = strtoull(argv[i] + 9, NULL, 10);
			return 1;
		}
		if (strcmp(argv[i], "--subvol") == 0 && i + 1 < argc) {
			*subvol_out = strtoull(argv[i + 1], NULL, 10);
			return 1;
		}
	}
	return 0;
}

int vtxfs_cli_select_subvol(struct vtx_fs *fs, __u64 subvol_id,
			    unsigned int flags,
			    struct vtx_subvol_root_item *item_out)
{
	struct vtx_key k;
	struct vtx_subvol_root_item item;
	size_t bytes = sizeof(item);
	int ret;

	if (!fs || !fs->mounted)
		return -EINVAL;
	if (subvol_id < VTXFS_OID_FIRST_USER_SUBVOL) {
		fprintf(stderr, "vtxfs: subvol_id phải >= %llu\n",
			(unsigned long long)VTXFS_OID_FIRST_USER_SUBVOL);
		return -EINVAL;
	}
	k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(subvol_id,
					VTX_TYPE_SUBVOL_ROOT));
	k.offset = 0;
	ret = vtx_rw_lookup(&fs->root_tree, &k, &item, &bytes);
	if (ret) {
		fprintf(stderr, "vtxfs: không tìm thấy subvolume %llu\n",
			(unsigned long long)subvol_id);
		return ret == 1 ? -ENOENT : ret;
	}
	if (bytes != sizeof(item))
		return -EUCLEAN;

	if ((flags & VTXFS_SELECT_WRITE) &&
	    (vtx_cpu64(item.flags) & VTX_SUBVOL_RDONLY)) {
		fprintf(stderr, "vtxfs: subvolume %llu là snapshot RO\n",
			(unsigned long long)subvol_id);
		return -EROFS;
	}
	if ((vtx_cpu64(item.flags) & VTX_SUBVOL_REAPING) &&
	    !(flags & VTXFS_SELECT_ALLOW_REAPING)) {
		fprintf(stderr,
			"vtxfs: subvolume %llu đang được Reaper dọn; "
			"thêm --allow-reaping để đọc trạng thái đã commit\n",
			(unsigned long long)subvol_id);
		return -ENOENT;
	}
	/* Đọc REAPING bắt buộc phải side-effect-free. */
	if (vtx_cpu64(item.flags) & VTX_SUBVOL_REAPING)
		flags |= VTXFS_SELECT_VIEW;

	ret = (flags & VTXFS_SELECT_VIEW)
		? vtx_subvol_switch_view(fs, subvol_id)
		: vtx_subvol_switch(fs, subvol_id);
	if (ret) {
		fprintf(stderr, "vtxfs: switch sang subvolume %llu lỗi: %s\n",
			(unsigned long long)subvol_id, strerror(-ret));
		return ret;
	}
	if (item_out)
		*item_out = item;
	return 0;
}
