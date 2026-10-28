// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * VTXFS v2.1 — Native C Terminal File Manager (vtxfs ctl / vtxfsctl)
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "commands.h"
#include "common.h"
#include "vtxfs_disk.h"
#include "vtxfs_engine.h"

#define TRASH_DIR "/.vtxfs-trash"
#define META_DIR  "/.vtxfs-trash-meta"

static int resolve_parent(struct vtx_fs *fs, const char *path,
			  __u64 *parent, char name[VTXFS_NAME_MAX + 1])
{
	const char *slash;
	size_t len, parent_len;
	char *parent_path;
	__u8 type = VTX_FT_DIR;
	int rc;

	if (!path || path[0] != '/' || !parent || !name)
		return -EINVAL;
	slash = strrchr(path, '/');
	if (!slash || !slash[1])
		return -EINVAL;
	len = strlen(slash + 1);
	if (len == 0 || len > VTXFS_NAME_MAX ||
	    strcmp(slash + 1, ".") == 0 || strcmp(slash + 1, "..") == 0)
		return -EINVAL;
	memcpy(name, slash + 1, len + 1);
	if (slash == path) {
		*parent = VTXFS_INO_ROOT_DIR;
		return 0;
	}
	parent_len = (size_t)(slash - path);
	parent_path = malloc(parent_len + 1);
	if (!parent_path)
		return -ENOMEM;
	memcpy(parent_path, path, parent_len);
	parent_path[parent_len] = 0;
	rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
			     parent_path, parent, &type);
	free(parent_path);
	return rc ? rc : (type == VTX_FT_DIR ? 0 : -ENOTDIR);
}

/* Recursive tree removal */
static int recursive_remove(struct vtx_fs *fs, const char *path, int depth)
{
	if (depth > 64) return -ELOOP;
	__u64 ino = 0;
	__u8 ftype = 0;
	int rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL, path, &ino, &ftype);
	if (rc != 0) return rc;

	if (ftype == VTX_FT_DIR) {
		__u64 cookie = 0;
		struct vtx_dirent ents[64];
		size_t count = 0;
		do {
			__u64 prev = cookie;
			rc = vtx_readdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, &cookie, ents, 64, &count);
			if (rc != 0) return rc;
			if (count && cookie <= prev) break;
			for (size_t i = 0; i < count; i++) {
				char child_path[1024];
				if (strcmp(path, "/") == 0)
					snprintf(child_path, sizeof(child_path), "/%s", ents[i].name);
				else
					snprintf(child_path, sizeof(child_path), "%s/%s", path, ents[i].name);
				rc = recursive_remove(fs, child_path, depth + 1);
				if (rc != 0) return rc;
			}
		} while (count == 64);

		__u64 parent = 0;
		char name[VTXFS_NAME_MAX + 1];
		rc = resolve_parent(fs, path, &parent, name);
		if (rc == 0)
			return vtx_rmdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name);
		return rc;
	} else {
		__u64 parent = 0;
		char name[VTXFS_NAME_MAX + 1];
		rc = resolve_parent(fs, path, &parent, name);
		if (rc == 0)
			return vtx_unlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name);
		return rc;
	}
}

/* Recursive search */
static void recursive_search(struct vtx_fs *fs, const char *dir_path, const char *keyword, int depth)
{
	if (depth > 64) return;
	__u64 dir_ino = 0;
	__u8 ftype = 0;
	int rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_path, &dir_ino, &ftype);
	if (rc != 0 || ftype != VTX_FT_DIR) return;

	__u64 cookie = 0;
	struct vtx_dirent ents[64];
	size_t count = 0;
	do {
		__u64 prev = cookie;
		rc = vtx_readdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_ino, &cookie, ents, 64, &count);
		if (rc != 0 || (count && cookie <= prev)) break;
		for (size_t i = 0; i < count; i++) {
			char full_path[1024];
			if (strcmp(dir_path, "/") == 0)
				snprintf(full_path, sizeof(full_path), "/%s", ents[i].name);
			else
				snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ents[i].name);

			if (strcasestr(ents[i].name, keyword) != NULL) {
				printf("%s\n", full_path);
			}
			if (ents[i].file_type == VTX_FT_DIR) {
				recursive_search(fs, full_path, keyword, depth + 1);
			}
		}
	} while (count == 64);
}

/* Copy tree inside VTXFS */
static int copy_tree(struct vtx_fs *fs, const char *src, const char *dst, int depth)
{
	if (depth > 64) return -ELOOP;
	__u64 src_ino = 0, dst_parent = 0;
	__u8 src_type = 0;
	char dst_name[VTXFS_NAME_MAX + 1];

	int rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL, src, &src_ino, &src_type);
	if (rc != 0) return rc;

	rc = resolve_parent(fs, dst, &dst_parent, dst_name);
	if (rc != 0) return rc;

	if (src_type == VTX_FT_DIR) {
		__u64 new_dir_ino = 0;
		rc = vtx_mkdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, dst_parent, dst_name, 0755, &new_dir_ino);
		if (rc != 0 && rc != -EEXIST) return rc;

		__u64 cookie = 0;
		struct vtx_dirent ents[64];
		size_t count = 0;
		do {
			__u64 prev = cookie;
			rc = vtx_readdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, src_ino, &cookie, ents, 64, &count);
			if (rc != 0) return rc;
			if (count && cookie <= prev) break;
			for (size_t i = 0; i < count; i++) {
				char child_src[1024], child_dst[1024];
				snprintf(child_src, sizeof(child_src), "%s/%s", src, ents[i].name);
				snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, ents[i].name);
				rc = copy_tree(fs, child_src, child_dst, depth + 1);
				if (rc != 0) return rc;
			}
		} while (count == 64);
		return 0;
	} else if (src_type == VTX_FT_REG) {
		struct vtx_inode_item st;
		rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, src_ino, &st);
		if (rc != 0) return rc;

		__u64 dst_ino = 0;
		rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL, dst_parent, dst_name, 0644, &dst_ino);
		if (rc != 0) return rc;

		__u64 sz = vtx_cpu64(st.size_bytes);
		__u64 off = 0;
		size_t chunk_sz = 64 * 1024;
		char *buf = malloc(chunk_sz);
		if (!buf) return -ENOMEM;

		while (off < sz) {
			size_t to_read = (sz - off > chunk_sz) ? chunk_sz : (size_t)(sz - off);
			size_t got = 0;
			rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, src_ino, off, buf, to_read, &got);
			if (rc != 0 || got == 0) break;
			rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, dst_ino, off, buf, got);
			if (rc != 0) break;
			off += got;
		}
		free(buf);
		return rc;
	} else if (src_type == VTX_FT_SYMLINK) {
		char target[1024];
		size_t t_len = 0;
		rc = vtx_readlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, src_ino, target, sizeof(target) - 1, &t_len);
		if (rc != 0) return rc;
		target[t_len] = '\0';
		__u64 sym_ino = 0;
		return vtx_symlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, dst_parent, dst_name, target, &sym_ino);
	}
	return -EINVAL;
}

static void ensure_dir_exists(struct vtx_fs *fs, const char *dir_path)
{
	__u64 ino = 0;
	__u8 ftype = 0;
	if (vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_path, &ino, &ftype) != 0) {
		__u64 parent = 0;
		char name[VTXFS_NAME_MAX + 1];
		if (resolve_parent(fs, dir_path, &parent, name) == 0) {
			__u64 new_ino = 0;
			vtx_mkdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name, 0755, &new_ino);
		}
	}
}

int cmd_ctl(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 2) {
		fprintf(stderr, "Cách dùng: vtxfs ctl <image> <lệnh> [tham_số...]\n"
		                "Các lệnh: info, disk-space, ls, cat, stat, create, mkdir, edit, rename, move,\n"
		                "          copy, delete/trash, restore, empty-trash, permanently-delete, search,\n"
		                "          hardlink, symlink, readlink, readonly\n");
		return 1;
	}

	const char *image = argv[0];
	const char *subcmd = argv[1];

	/* Determine whether command requires read-write mount */
	bool rw = false;
	if (strcmp(subcmd, "create") == 0 || strcmp(subcmd, "mkdir") == 0 ||
	    strcmp(subcmd, "edit") == 0 || strcmp(subcmd, "rename") == 0 ||
	    strcmp(subcmd, "move") == 0 || strcmp(subcmd, "copy") == 0 ||
	    strcmp(subcmd, "delete") == 0 || strcmp(subcmd, "trash") == 0 ||
	    strcmp(subcmd, "restore") == 0 || strcmp(subcmd, "empty-trash") == 0 ||
	    strcmp(subcmd, "permanently-delete") == 0 || strcmp(subcmd, "hardlink") == 0 ||
	    strcmp(subcmd, "symlink") == 0 || strcmp(subcmd, "shortcut") == 0 ||
	    strcmp(subcmd, "readonly") == 0 || strcmp(subcmd, "hide") == 0 ||
	    strcmp(subcmd, "unhide") == 0 || strcmp(subcmd, "paste") == 0) {
		rw = true;
	}

	struct vtxfs_handle h;
	int rc = vtxfs_cli_open(&h, image, rw ? 1 : 0);
	if (rc != 0) {
		fprintf(stderr, "vtxfs ctl: Không thể mở '%s': %s\n", image, strerror(-rc));
		return 1;
	}

	int ret = 0;

	if (strcmp(subcmd, "info") == 0) {
		printf("VTXFS v2.1 Filesystem Inspector\n");
		printf("  label             : %s\n", h.fs->super_blk.hdr.label);
		printf("  fsid_uuid         : ");
		vtxfs_cli_print_uuid(h.fs->super_blk.hdr.fsid_uuid);
		printf("\n  total_blocks      : %llu (%llu MiB)\n",
		       (unsigned long long)vtx_cpu64(h.fs->super_blk.hdr.total_blocks),
		       (unsigned long long)((vtx_cpu64(h.fs->super_blk.hdr.total_blocks) * VTXFS_BLOCK_SIZE) >> 20));
		printf("  committed_xid     : %llu\n", (unsigned long long)vtx_cpu64(h.fs->committed_xid));
		printf("  free_blocks       : %llu\n", (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.free_blocks));
		printf("  used_blocks       : %llu\n", (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.used_blocks));
		printf("  total_vags        : %u\n", h.fs->vag.total_vags);
	}
	else if (strcmp(subcmd, "disk-space") == 0 || strcmp(subcmd, "check-space") == 0) {
		__u64 total = vtx_cpu64(h.fs->super_blk.hdr.total_blocks);
		__u64 free_b = vtx_cpu64(h.fs->cp_blk.hdr.free_blocks);
		__u64 used_b = vtx_cpu64(h.fs->cp_blk.hdr.used_blocks);
		printf("Total image    : %llu B (%.1f MiB)\n", (unsigned long long)(total * 4096), (double)(total * 4096) / 1048576.0);
		printf("Free alloc     : %llu B (%.1f MiB)\n", (unsigned long long)(free_b * 4096), (double)(free_b * 4096) / 1048576.0);
		printf("Used alloc     : %llu B (%.1f MiB)\n", (unsigned long long)(used_b * 4096), (double)(used_b * 4096) / 1048576.0);
		printf("VAG count      : %u\n", h.fs->vag.total_vags);
		printf("Note: free/used are checkpoint allocation counters, not guaranteed new-file capacity.\n");
	}
	else if (strcmp(subcmd, "ls") == 0) {
		const char *path = (argc >= 3) ? argv[2] : "/";
		__u64 dir_ino = VTXFS_INO_ROOT_DIR;
		__u8 ftype = VTX_FT_DIR;
		if (strcmp(path, "/") != 0) {
			rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, path, &dir_ino, &ftype);
			if (rc != 0 || ftype != VTX_FT_DIR) {
				fprintf(stderr, "vtxfs ctl: ls '%s': Thư mục không tồn tại\n", path);
				ret = 1;
				goto out;
			}
		}
		__u64 cookie = 0;
		struct vtx_dirent ents[64];
		size_t count = 0;
		do {
			__u64 prev = cookie;
			rc = vtx_readdir(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_ino, &cookie, ents, 64, &count);
			if (rc != 0 || (count && cookie <= prev)) break;
			for (size_t i = 0; i < count; i++) {
				printf("ino=%llu type=%u name=%s\n",
				       (unsigned long long)ents[i].ino, ents[i].file_type, ents[i].name);
			}
		} while (count == 64);
	}
	else if (strcmp(subcmd, "cat") == 0 || strcmp(subcmd, "open") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> cat <path>\n");
			ret = 1;
			goto out;
		}
		const char *path = argv[2];
		__u64 ino = 0;
		__u8 ftype = 0;
		rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, path, &ino, &ftype);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: lookup '%s' thất bại: %s\n", path, strerror(-rc));
			ret = 1;
			goto out;
		}
		if (ftype == VTX_FT_DIR) {
			/* List directory */
			char *ls_args[3];
			ls_args[0] = (char *)image;
			ls_args[1] = "ls";
			ls_args[2] = (char *)path;
			vtxfs_cli_close(&h, 0);
			return cmd_ctl(cmd, 3, ls_args);
		} else if (ftype == VTX_FT_SYMLINK) {
			char target[1024];
			size_t len = 0;
			rc = vtx_readlink(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, target, sizeof(target) - 1, &len);
			if (rc == 0) {
				target[len] = '\0';
				printf("%s\n", target);
			}
		} else {
			struct vtx_inode_item st;
			rc = vtx_stat(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, &st);
			if (rc == 0) {
				__u64 sz = vtx_cpu64(st.size_bytes);
				__u64 off = 0;
				char buf[4096];
				while (off < sz) {
					size_t chunk = (sz - off > sizeof(buf)) ? sizeof(buf) : (size_t)(sz - off);
					size_t got = 0;
					rc = vtx_read(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, off, buf, chunk, &got);
					if (rc != 0 || got == 0) break;
					(void)fwrite(buf, 1, got, stdout);
					off += got;
				}
			}
		}
	}
	else if (strcmp(subcmd, "stat") == 0 || strcmp(subcmd, "properties") == 0 || strcmp(subcmd, "get-info") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> stat <path>\n");
			ret = 1;
			goto out;
		}
		const char *path = argv[2];
		__u64 ino = 0;
		__u8 ftype = 0;
		rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, path, &ino, &ftype);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: Không tìm thấy '%s'\n", path);
			ret = 1;
			goto out;
		}
		struct vtx_inode_item st;
		rc = vtx_stat(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, &st);
		if (rc == 0) {
			printf("path: %s\n", path);
			printf("  ino        : %llu\n", (unsigned long long)ino);
			printf("  mode       : 0%o\n", vtx_cpu32(st.mode));
			printf("  size_bytes : %llu\n", (unsigned long long)vtx_cpu64(st.size_bytes));
			printf("  alloc_bytes: %llu\n", (unsigned long long)vtx_cpu64(st.alloc_bytes));
			printf("  nlink      : %u\n", vtx_cpu32(st.nlink));
			printf("  update_xid : %llu\n", (unsigned long long)vtx_cpu64(st.update_xid));
			printf("  uid        : %u\n", vtx_cpu32(st.uid));
			printf("  gid        : %u\n", vtx_cpu32(st.gid));
			printf("  mtime_ns   : %llu\n", (unsigned long long)vtx_cpu64(st.mtime_ns));
		}
	}
	else if (strcmp(subcmd, "mkdir") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> mkdir <path>\n");
			ret = 1;
			goto out;
		}
		const char *path = argv[2];
		__u64 parent = 0, new_ino = 0;
		char name[VTXFS_NAME_MAX + 1];
		rc = resolve_parent(h.fs, path, &parent, name);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: Không thể xác định thư mục cha cho '%s'\n", path);
			ret = 1;
			goto out;
		}
		rc = vtx_mkdir(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name, 0755, &new_ino);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: mkdir '%s' thất bại: %s\n", path, strerror(-rc));
			ret = 1;
			goto out;
		}
		printf("CREATED DIR %s (ino=%llu)\n", path, (unsigned long long)new_ino);
	}
	else if (strcmp(subcmd, "create") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> create <path> [--dir]\n");
			ret = 1;
			goto out;
		}
		const char *path = argv[2];
		bool is_dir = (argc >= 4 && strcmp(argv[3], "--dir") == 0);
		__u64 parent = 0, new_ino = 0;
		char name[VTXFS_NAME_MAX + 1];
		rc = resolve_parent(h.fs, path, &parent, name);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: Lỗi đường dẫn '%s'\n", path);
			ret = 1;
			goto out;
		}
		if (is_dir)
			rc = vtx_mkdir(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name, 0755, &new_ino);
		else
			rc = vtx_create(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name, 0644, &new_ino);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: create '%s' thất bại: %s\n", path, strerror(-rc));
			ret = 1;
			goto out;
		}
		printf("CREATED %s\n", path);
	}
	else if (strcmp(subcmd, "edit") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> edit <path> < content\n");
			ret = 1;
			goto out;
		}
		const char *path = argv[2];
		__u64 parent = 0, ino = 0;
		__u8 ftype = VTX_FT_REG;
		char name[VTXFS_NAME_MAX + 1];
		rc = resolve_parent(h.fs, path, &parent, name);
		if (rc != 0) {
			ret = 1;
			goto out;
		}
		rc = vtx_lookup(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name, &ino, &ftype);
		if (rc == -ENOENT)
			rc = vtx_create(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name, 0644, &ino);
		else if (rc == 0 && ftype == VTX_FT_REG)
			rc = vtx_truncate(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, 0);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: edit '%s' thất bại: %s\n", path, strerror(-rc));
			ret = 1;
			goto out;
		}
		char buf[64 * 1024];
		size_t n;
		__u64 off = 0;
		while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
			rc = vtx_write(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, off, buf, n);
			if (rc != 0) break;
			off += n;
		}
		printf("UPDATED %s (%llu bytes)\n", path, (unsigned long long)off);
	}
	else if (strcmp(subcmd, "rename") == 0 || strcmp(subcmd, "move") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> %s <source> <destination>\n", subcmd);
			ret = 1;
			goto out;
		}
		const char *src = argv[2];
		const char *dst = argv[3];
		__u64 s_par = 0, d_par = 0;
		char s_name[VTXFS_NAME_MAX + 1], d_name[VTXFS_NAME_MAX + 1];
		rc = resolve_parent(h.fs, src, &s_par, s_name);
		if (rc != 0) { ret = 1; goto out; }
		rc = resolve_parent(h.fs, dst, &d_par, d_name);
		if (rc != 0) { ret = 1; goto out; }
		rc = vtx_rename(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, s_par, s_name, d_par, d_name);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: move '%s' -> '%s' thất bại: %s\n", src, dst, strerror(-rc));
			ret = 1;
			goto out;
		}
		printf("MOVED %s -> %s\n", src, dst);
	}
	else if (strcmp(subcmd, "copy") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> copy <source> <destination>\n");
			ret = 1;
			goto out;
		}
		rc = copy_tree(h.fs, argv[2], argv[3], 0);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: copy '%s' -> '%s' thất bại: %s\n", argv[2], argv[3], strerror(-rc));
			ret = 1;
			goto out;
		}
		printf("COPIED %s -> %s\n", argv[2], argv[3]);
	}
	else if (strcmp(subcmd, "delete") == 0 || strcmp(subcmd, "trash") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> delete <path>\n");
			ret = 1;
			goto out;
		}
		const char *path = argv[2];
		ensure_dir_exists(h.fs, TRASH_DIR);
		ensure_dir_exists(h.fs, META_DIR);

		/* Generate ticket */
		char ticket[64];
		snprintf(ticket, sizeof(ticket), "%lx%x", (unsigned long)time(NULL), rand() & 0xffff);
		const char *basename = strrchr(path, '/');
		basename = basename ? basename + 1 : path;

		char trash_dest[1024], meta_path[1024];
		snprintf(trash_dest, sizeof(trash_dest), "%s/%s-%s", TRASH_DIR, ticket, basename);
		snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", META_DIR, ticket);

		/* Write metadata */
		__u64 meta_parent = 0, meta_ino = 0;
		char meta_name[VTXFS_NAME_MAX + 1];
		if (resolve_parent(h.fs, meta_path, &meta_parent, meta_name) == 0) {
			if (vtx_create(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, meta_parent, meta_name, 0644, &meta_ino) == 0) {
				(void)vtx_write(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, meta_ino, 0, path, strlen(path));
			}
		}

		/* Move to trash */
		__u64 s_par = 0, d_par = 0;
		char s_name[VTXFS_NAME_MAX + 1], d_name[VTXFS_NAME_MAX + 1];
		if (resolve_parent(h.fs, path, &s_par, s_name) == 0 &&
		    resolve_parent(h.fs, trash_dest, &d_par, d_name) == 0) {
			rc = vtx_rename(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, s_par, s_name, d_par, d_name);
			if (rc == 0) {
				printf("TRASHED %s -> %s\n", path, trash_dest);
			} else {
				fprintf(stderr, "vtxfs ctl: trash thất bại: %s\n", strerror(-rc));
				ret = 1;
			}
		}
	}
	else if (strcmp(subcmd, "empty-trash") == 0) {
		recursive_remove(h.fs, TRASH_DIR, 0);
		recursive_remove(h.fs, META_DIR, 0);
		printf("EMPTY TRASH: all discarded VTXFS directory entries removed\n");
	}
	else if (strcmp(subcmd, "permanently-delete") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> permanently-delete <path>\n");
			ret = 1;
			goto out;
		}
		rc = recursive_remove(h.fs, argv[2], 0);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: permanently-delete '%s' thất bại: %s\n", argv[2], strerror(-rc));
			ret = 1;
			goto out;
		}
		printf("PERMANENTLY REMOVED (logical allocation only, not secure erase): %s\n", argv[2]);
	}
	else if (strcmp(subcmd, "search") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> search <keyword> [--in=DIR]\n");
			ret = 1;
			goto out;
		}
		const char *kw = argv[2];
		const char *in_dir = (argc >= 4 && strncmp(argv[3], "--in=", 5) == 0) ? (argv[3] + 5) : "/";
		recursive_search(h.fs, in_dir, kw, 0);
	}
	else if (strcmp(subcmd, "hardlink") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> hardlink <source> <destination>\n");
			ret = 1;
			goto out;
		}
		__u64 s_ino = 0, d_par = 0;
		__u8 s_ft = 0;
		char d_name[VTXFS_NAME_MAX + 1];
		rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, argv[2], &s_ino, &s_ft);
		if (rc != 0) { ret = 1; goto out; }
		rc = resolve_parent(h.fs, argv[3], &d_par, d_name);
		if (rc != 0) { ret = 1; goto out; }
		rc = vtx_link(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, s_ino, d_par, d_name);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: hardlink thất bại: %s\n", strerror(-rc));
			ret = 1;
			goto out;
		}
		printf("HARDLINK %s -> %s\n", argv[2], argv[3]);
	}
	else if (strcmp(subcmd, "symlink") == 0 || strcmp(subcmd, "shortcut") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> symlink <target> <linkpath>\n");
			ret = 1;
			goto out;
		}
		__u64 d_par = 0, new_ino = 0;
		char d_name[VTXFS_NAME_MAX + 1];
		rc = resolve_parent(h.fs, argv[3], &d_par, d_name);
		if (rc != 0) { ret = 1; goto out; }
		rc = vtx_symlink(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, d_par, d_name, argv[2], &new_ino);
		if (rc != 0) {
			fprintf(stderr, "vtxfs ctl: symlink thất bại: %s\n", strerror(-rc));
			ret = 1;
			goto out;
		}
		printf("SYMLINK %s -> %s\n", argv[3], argv[2]);
	}
	else if (strcmp(subcmd, "readlink") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> readlink <path>\n");
			ret = 1;
			goto out;
		}
		__u64 ino = 0;
		__u8 ft = 0;
		rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, argv[2], &ino, &ft);
		if (rc == 0 && ft == VTX_FT_SYMLINK) {
			char target[1024];
			size_t len = 0;
			rc = vtx_readlink(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, target, sizeof(target) - 1, &len);
			if (rc == 0) {
				target[len] = '\0';
				printf("%s\n", target);
			}
		} else {
			fprintf(stderr, "vtxfs ctl: Không phải symlink: %s\n", argv[2]);
			ret = 1;
		}
	}
	else if (strcmp(subcmd, "readonly") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Cách dùng: vtxfs ctl <image> readonly <path> [--off]\n");
			ret = 1;
			goto out;
		}
		bool off = (argc >= 4 && strcmp(argv[3], "--off") == 0);
		__u64 ino = 0;
		__u8 ft = 0;
		rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, argv[2], &ino, &ft);
		if (rc == 0) {
			rc = vtx_chmod(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, off ? 0644 : 0444);
			printf("CHMOD %s -> %s\n", argv[2], off ? "0644" : "0444");
		}
	}
	else {
		fprintf(stderr, "vtxfs ctl: Lệnh không hỗ trợ: '%s'\n", subcmd);
		ret = 1;
	}

out:
	vtxfs_cli_close(&h, rw ? 1 : 0);
	return ret;
}
