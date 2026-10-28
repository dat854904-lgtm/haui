// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs-inspect — VTXFS v2.1 Filesystem Inspector & CLI Explorer
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vtxfs_engine.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s <image> [info]\n"
		"  %s <image> ls [path]\n"
		"  %s <image> stat <path>\n"
		"  %s <image> cat <path>\n"
		"  %s <image> mkdir <parent_ino> <name>\n"
		"  %s <image> write <parent_ino> <name> <text>\n"
		"  %s <image> snap <name>\n"
		"  %s <image> bitmap <vag> [offset [count]]\n"
		"  %s <image> put-stdin <absolute_vtxfs_path> < binary_input\n"
		"  %s <image> copy-file <source_path> <new_path>\n"
		"  %s <image> move-safe <source_path> <new_path>\n"
        "  %s <image> symlink <target> <new_link_path>\n"
        "  %s <image> readlink <link_path>\n"
        "  %s <image> chmod <octal_mode> <path>\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog);
}

static void print_uuid(const __u8 u[16])
{
	printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	       u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
	       u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static int cmd_info(struct vtx_fs *fs)
{
	__u32 i;

	printf("VTXFS v2.1 Filesystem Inspector\n");
	printf("  label             : %s\n", fs->super_blk.hdr.label);
	printf("  fsid_uuid         : ");
	print_uuid(fs->super_blk.hdr.fsid_uuid);
	printf("\n  metadata_uuid     : ");
	print_uuid(fs->super_blk.hdr.metadata_uuid);
	printf("\n  total_blocks      : %llu (%llu MiB)\n",
	       (unsigned long long)vtx_cpu64(fs->super_blk.hdr.total_blocks),
	       (unsigned long long)((vtx_cpu64(fs->super_blk.hdr.total_blocks) *
				     VTXFS_BLOCK_SIZE) >> 20));
	printf("  committed_xid     : %llu\n",
	       (unsigned long long)fs->committed_xid);
	printf("  next_oid          : %llu\n",
	       (unsigned long long)fs->next_oid);
	printf("  free_blocks       : %llu\n",
	       (unsigned long long)vtx_cpu64(fs->cp_blk.hdr.free_blocks));
	printf("  used_blocks       : %llu\n",
	       (unsigned long long)vtx_cpu64(fs->cp_blk.hdr.used_blocks));
	printf("  root_tree_vblk    : 0x%llx (level %u)\n",
	       (unsigned long long)fs->root_tree.root_vblk,
	       fs->root_tree.root_level);
	printf("  omap_meta_vblk    : 0x%llx (level %u)\n",
	       (unsigned long long)fs->omap_meta_tree.root_vblk,
	       fs->omap_meta_tree.root_level);
	printf("  ref_tree_vblk     : 0x%llx (level %u)\n",
	       (unsigned long long)fs->ref_tree.root_vblk,
	       fs->ref_tree.root_level);
	printf("  sfq_tree_vblk     : 0x%llx (level %u)\n",
	       (unsigned long long)fs->sfq_tree.root_vblk,
	       fs->sfq_tree.root_level);
	printf("  reaper_tree_vblk  : 0x%llx (level %u)\n",
	       (unsigned long long)fs->reaper_tree.root_vblk,
	       fs->reaper_tree.root_level);
	printf("  total_vags        : %u\n", fs->vag.total_vags);
	for (i = 0; i < fs->vag.total_vags; i++) {
		printf("    VAG[%u]: start_pblk=%llu free_blocks=%u\n",
		       i,
		       (unsigned long long)vtx_cpu64(fs->vag.descs[i].start_pblk),
		       vtx_cpu32(fs->vag.descs[i].free_blocks));
	}
	return 0;
}

static int cmd_ls(struct vtx_fs *fs, const char *path)
{
	__u64 dir_ino = VTXFS_INO_ROOT_DIR;
	__u8 ftype = VTX_FT_DIR;
	__u64 cookie = 0;
	struct vtx_dirent ents[64];
	size_t count = 0, i;
	int rc;

	if (path && strcmp(path, "/") != 0) {
		rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				     path, &dir_ino, &ftype);
		if (rc != 0) {
			fprintf(stderr, "vtxfs-inspect: lookup '%s' failed: %s\n",
				path, strerror(-rc));
			return 1;
		}
		if (ftype != VTX_FT_DIR) {
			fprintf(stderr, "vtxfs-inspect: ls '%s': Not a directory\n", path);
			return 1;
		}
	}
	/* Readdir is paged by cookie. The old one-shot implementation
	 * silently hid entries after the 64th item in large directories. */
	do {
		__u64 previous = cookie;

		rc = vtx_readdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_ino,
				 &cookie, ents, 64, &count);
		if (rc) {
			fprintf(stderr, "vtxfs-inspect: readdir failed: %s\n",
				strerror(-rc));
			return 1;
		}
		if (count && cookie <= previous)
			return 1; /* invalid cookie; avoid infinite iteration */
		for (i = 0; i < count; i++) {
			printf("ino=%llu type=%u name=%s\n",
			       (unsigned long long)ents[i].ino,
			       ents[i].file_type,
			       ents[i].name);
		}
	} while (count == 64);
	return 0;
}

static int cmd_stat(struct vtx_fs *fs, const char *path)
{
	__u64 ino = VTXFS_INO_ROOT_DIR;
	__u8 ftype = VTX_FT_DIR;
	struct vtx_inode_item st;
	int rc;

	if (strcmp(path, "/") != 0) {
		rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				     path, &ino, &ftype);
		if (rc != 0) {
			fprintf(stderr, "vtxfs-inspect: lookup '%s' failed: %s\n",
				path, strerror(-rc));
			return 1;
		}
	}
	rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, &st);
	if (rc != 0) {
		fprintf(stderr, "vtxfs-inspect: stat failed: %s\n",
			strerror(-rc));
		return 1;
	}
	printf("path: %s\n", path);
	printf("  ino        : %llu\n", (unsigned long long)ino);
	printf("  mode       : 0%o\n", vtx_cpu32(st.mode));
	printf("  size_bytes : %llu\n",
	       (unsigned long long)vtx_cpu64(st.size_bytes));
	printf("  alloc_bytes: %llu\n",
	       (unsigned long long)vtx_cpu64(st.alloc_bytes));
	printf("  nlink      : %u\n", vtx_cpu32(st.nlink));
	printf("  update_xid : %llu\n",
	       (unsigned long long)vtx_cpu64(st.update_xid));
    printf("  uid        : %u\n", vtx_cpu32(st.uid));
    printf("  gid        : %u\n", vtx_cpu32(st.gid));
    printf("  atime_ns   : %llu\n", (unsigned long long)vtx_cpu64(st.atime_ns));
    printf("  mtime_ns   : %llu\n", (unsigned long long)vtx_cpu64(st.mtime_ns));
    printf("  ctime_ns   : %llu\n", (unsigned long long)vtx_cpu64(st.ctime_ns));
	return 0;
}

static int cmd_cat(struct vtx_fs *fs, const char *path)
{
	enum { READ_CHUNK = 128U * 1024U };
	__u64 ino = 0;
	__u8 ftype = 0;
	struct vtx_inode_item st;
	__u64 sz, off = 0;
	__u8 *buf;
	int rc;

	rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
			     path, &ino, &ftype);
	if (rc != 0) {
		fprintf(stderr, "vtxfs-inspect: lookup '%s' failed: %s\n",
			path, strerror(-rc));
		return 1;
	}
	if (ftype != VTX_FT_REG) {
		fprintf(stderr, "vtxfs-inspect: cat '%s': Not a regular file\n", path);
		return 1;
	}
	rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, &st);
	if (rc != 0)
		return 1;
	buf = malloc(READ_CHUNK);
	if (!buf)
		return 1;
	sz = vtx_cpu64(st.size_bytes);
	while (off < sz) {
		size_t chunk = (sz - off > READ_CHUNK) ?
			       READ_CHUNK : (size_t)(sz - off);
		size_t got = 0;

		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino,
			      off, buf, chunk, &got);
		if (rc != 0 || got == 0) {
			fprintf(stderr, "vtxfs-inspect: read failed: %s\n",
				rc ? strerror(-rc) : "short read");
			free(buf);
			return 1;
		}
		if (fwrite(buf, 1, got, stdout) != got) {
			fprintf(stderr, "vtxfs-inspect: output write failed\n");
			free(buf);
			return 1;
		}
		off += got;
	}
	free(buf);
	return 0;
}


/* Only absolute paths within the opened VTXFS image are accepted. */
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

/* A symbolic link is a distinct VTXFS inode. It does not copy data. */
static int cmd_symlink(struct vtx_fs *fs, const char *target,
                       const char *linkpath)
{
    __u64 parent, ino = 0;
    char name[VTXFS_NAME_MAX + 1];
    int ret = resolve_parent(fs, linkpath, &parent, name);
    if (ret)
        return ret;
    return vtx_symlink(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                       parent, name, target, &ino);
}

/* True hard link: another directory entry to the SAME inode and extent set. */
static int cmd_hardlink(struct vtx_fs *fs, const char *source,
                        const char *newpath)
{
    __u64 ino = 0, parent;
    __u8 type = 0;
    char name[VTXFS_NAME_MAX + 1];
    int rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                             source, &ino, &type);
    if (rc)
        return rc;
    if (type != VTX_FT_REG)
        return -EPERM;
    rc = resolve_parent(fs, newpath, &parent, name);
    if (rc)
        return rc;
    return vtx_link(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, parent, name);
}

static int cmd_readlink(struct vtx_fs *fs, const char *path)
{
    __u64 ino;
    __u8 ft;
    char target[VTXFS_MAX_INLINE_DATA + 1];
    size_t len = 0;
    int ret = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                              path, &ino, &ft);
    if (ret)
        return ret;
    if (ft != VTX_FT_SYMLINK)
        return -EINVAL;
    ret = vtx_readlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino,
                       target, sizeof(target), &len);
    if (!ret) {
        if (fwrite(target, 1, len, stdout) != len || putchar('\n') == EOF)
            return -EIO;
    }
    return ret;
}

/* Mode is an inode metadata value. User-space engine does not automatically
 * enforce a security policy based on mode, do not promise access isolation. */
static int cmd_chmod(struct vtx_fs *fs, const char *mode_text,
                     const char *path)
{
    char *tail;
    unsigned long mode;
    __u64 ino;
    __u8 type;
    int ret;
    if (!mode_text || !*mode_text || *mode_text == '-')
        return -EINVAL;
    errno = 0;
    mode = strtoul(mode_text, &tail, 8);
    if (errno || !tail || *tail || mode > 07777UL)
        return -EINVAL;
    ret = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                          path, &ino, &type);
    if (ret)
        return ret;
    return vtx_chmod(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, (__u32)mode);
}

static int cmd_remove(struct vtx_fs *fs, const char *path)
{
	__u64 parent, ino;
	__u8 type;
	char name[VTXFS_NAME_MAX + 1];
	int rc = resolve_parent(fs, path, &parent, name);

	if (rc)
		return rc;
	rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
			parent, name, &ino, &type);
	if (rc)
		return rc;
	if (type == VTX_FT_DIR)
		return vtx_rmdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name);
	return vtx_unlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, parent, name);
}

static int cmd_rename(struct vtx_fs *fs, const char *oldpath,
		      const char *newpath)
{
	__u64 oldparent, newparent;
	char oldname[VTXFS_NAME_MAX + 1], newname[VTXFS_NAME_MAX + 1];
	int rc = resolve_parent(fs, oldpath, &oldparent, oldname);

	if (rc)
		return rc;
	rc = resolve_parent(fs, newpath, &newparent, newname);
	if (rc)
		return rc;
	return vtx_rename(fs, VTXFS_OID_FIRST_USER_SUBVOL,
			  oldparent, oldname, newparent, newname);
}

/* Non-overwriting move. vtx_rename() follows POSIX replacement rules,
 * so a separate command is needed for the GUI's safe Cut/Paste semantics. */
static int cmd_move_safe(struct vtx_fs *fs, const char *oldpath,
                         const char *newpath)
{
    __u64 oldparent, newparent, old_ino, existing;
    __u8 oldtype;
    char oldname[VTXFS_NAME_MAX + 1], newname[VTXFS_NAME_MAX + 1];
    size_t oldlen;
    int rc;

    if (!oldpath || !newpath || oldpath[0] != '/' || newpath[0] != '/')
        return -EINVAL;
    if (strcmp(oldpath, newpath) == 0)
        return 0;
    rc = resolve_parent(fs, oldpath, &oldparent, oldname);
    if (rc)
        return rc;
    rc = resolve_parent(fs, newpath, &newparent, newname);
    if (rc)
        return rc;
    rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                    oldparent, oldname, &old_ino, &oldtype);
    if (rc)
        return rc;
    /* No moving a folder inside itself (would create a directory cycle). */
    oldlen = strlen(oldpath);
    if (oldtype == VTX_FT_DIR &&
        strncmp(oldpath, newpath, oldlen) == 0 &&
        newpath[oldlen] == '/')
        return -EINVAL;
    rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                    newparent, newname, &existing, NULL);
    if (rc != -ENOENT)
        return rc ? rc : -EEXIST;
    return vtx_rename(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                      oldparent, oldname, newparent, newname);
}

/* Stream-copy a regular file entirely inside VTXFS, without overwriting.
 * No physical host path is used. Checkpoint periodically to respect bounded
 * queues. On ENOSPC/error the destination may be incomplete and is reported.
 */
static int cmd_copy_file(struct vtx_fs *fs, const char *src,
                         const char *dest)
{
    /* 1 MiB keeps pending/CoW bounded while amortizing CPD FUA flushes. */
    enum { BLOCK_CHUNK = 64 * 1024, CHECKPOINT_INTERVAL = 1024 * 1024 };
    __u64 ino = 0, parent = 0, dst_ino = 0;
    __u8 src_type = 0;
    __u8 *buf = NULL;
    char name[VTXFS_NAME_MAX + 1];
    struct vtx_inode_item stat;
    __u64 total, offset = 0;
    size_t uncommitted = 0;
    int rc;

    if (!src || !dest || src[0] != '/' || dest[0] != '/')
        return -EINVAL;
    if (strcmp(src, dest) == 0)
        return -EINVAL;
    rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                         src, &ino, &src_type);
    if (rc)
        return rc;
    if (src_type != VTX_FT_REG)
        return -EISDIR; /* directory recursion is deliberately unsupported */
    rc = resolve_parent(fs, dest, &parent, name);
    if (rc)
        return rc;
    rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                    parent, name, &dst_ino, NULL);
    if (rc != -ENOENT)
        return rc ? rc : -EEXIST;
    rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, &stat);
    if (rc)
        return rc;
    total = vtx_cpu64(stat.size_bytes);
    buf = malloc(BLOCK_CHUNK);
    if (!buf)
        return -ENOMEM;
    rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                    parent, name, 0644, &dst_ino);
    if (rc)
        goto done;
    while (offset < total) {
        size_t count = total - offset > BLOCK_CHUNK ?
                       BLOCK_CHUNK : (size_t)(total - offset);
        size_t got = 0;

        rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino,
                      offset, buf, count, &got);
        if (rc)
            goto done;
        if (got != count) {
            rc = -EIO;
            goto done;
        }
        rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                       dst_ino, offset, buf, got);
        if (rc)
            goto done;
        offset += got;
        uncommitted += got;
        if (uncommitted >= CHECKPOINT_INTERVAL) {
            rc = vtx_sync_checkpoint(fs);
            if (rc)
                goto done;
            uncommitted = 0;
        }
    }
    rc = vtx_sync_checkpoint(fs);
    if (!rc)
        printf("copied_bytes=%llu\n", (unsigned long long)total);
done:
    free(buf);
    if (rc)
        fprintf(stderr, "copy stopped at byte %llu: %s\n",
                (unsigned long long)offset, strerror(-rc));
    return rc;
}

static int cmd_replace(struct vtx_fs *fs, const char *path, const char *text)
{
	__u64 parent, ino;
	__u8 type = VTX_FT_REG;
	char name[VTXFS_NAME_MAX + 1];
	int rc = resolve_parent(fs, path, &parent, name);

	if (rc)
		return rc;
	rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
			parent, name, &ino, &type);
	if (rc == -ENOENT)
		rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				parent, name, 0644, &ino);
	else if (!rc && type == VTX_FT_REG)
		rc = vtx_truncate(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, 0);
	else if (!rc)
		rc = -EISDIR;
	if (rc)
		return rc;
	return vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL,
			 ino, 0, text, strlen(text));
}

/*
 * Stream binary contents from standard input. In contrast to "replace",
 * this avoids Linux MAX_ARG_STRLEN and works with arbitrary binary data.
 * The portable engine's pending write / delayed-ref caches are bounded:
 * checkpoint after a small batch to keep memory and journal pressure low.
 *
 * This is a REPLACE operation, not an append. Errors may leave a partial
 * file: the caller must be told and must not assume successful completion.
 */
static int cmd_put_stdin(struct vtx_fs *fs, const char *path)
{
    /* Batch 1 MiB per checkpoint (<=256 data blocks, cache max 1024). */
    enum { CHUNK_BYTES = 64 * 1024, CHECKPOINT_BYTES = 1024 * 1024 };
    __u8 *buf = NULL;
    __u64 parent, ino = 0, offset = 0;
    __u8 type = VTX_FT_REG;
    char name[VTXFS_NAME_MAX + 1];
    size_t uncommitted = 0, n;
    int ret;

    ret = resolve_parent(fs, path, &parent, name);
    if (ret)
        return ret;
    ret = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                     parent, name, &ino, &type);
    if (ret == -ENOENT)
        ret = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
                         parent, name, 0644, &ino);
    else if (!ret && type == VTX_FT_REG)
        ret = vtx_truncate(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, 0);
    else if (!ret)
        ret = -EISDIR;
    if (ret)
        return ret;

    buf = malloc(CHUNK_BYTES);
    if (!buf)
        return -ENOMEM;
    while ((n = fread(buf, 1, CHUNK_BYTES, stdin)) != 0) {
        if (offset > UINT64_MAX - n) {
            ret = -EFBIG;
            goto done;
        }
        ret = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, ino,
                        offset, buf, n);
        if (ret)
            goto done;
        offset += n;
        uncommitted += n;
        if (uncommitted >= CHECKPOINT_BYTES) {
            ret = vtx_sync_checkpoint(fs);
            if (ret)
                goto done;
            uncommitted = 0;
        }
    }
    if (ferror(stdin)) {
        ret = -EIO;
        goto done;
    }
    ret = vtx_sync_checkpoint(fs);
    if (!ret)
        printf("put_bytes=%llu\n", (unsigned long long)offset);
done:
    free(buf);
    if (ret)
        fprintf(stderr, "put-stdin stopped at byte %llu: %s\n",
                (unsigned long long)offset, strerror(-ret));
    return ret;
}

/*
 * Read-only VAG allocation bitmap inspector.
 * One bit per VBLK: 1 = allocated, 0 = free.
 * The VAG starts at desc.start_pblk and the corresponding data PBLK is
 * start_pblk + offset. This does NOT identify ownership or file extents.
 *
 * Output format is intentionally stable and machine-readable for GTK:
 * key=value header and little-bit-order packed bitmap_hex.
 */
static int cmd_bitmap(struct vtx_fs *fs, int argc, char **argv)
{
    unsigned long vag_id;
    char *tail;
    __u32 off = 0, count = VTXFS_VAG_BLOCKS, i;
    __u32 allocated = 0;
    __u64 pblk_start;
    const __u8 *bitmap;

    if (argc < 4 || argc > 6 || !argv[3][0] || argv[3][0] == '-')
        return -EINVAL;
    errno = 0;
    vag_id = strtoul(argv[3], &tail, 10);
    if (errno || !tail || *tail || vag_id >= fs->vag.total_vags)
        return -ERANGE;
    if (argc >= 5) {
        unsigned long val;

        if (!argv[4][0] || argv[4][0] == '-')
            return -EINVAL;
        errno = 0;
        val = strtoul(argv[4], &tail, 10);
        if (errno || !tail || *tail || val >= VTXFS_VAG_BLOCKS)
            return -ERANGE;
        off = (__u32)val;
        count = VTXFS_VAG_BLOCKS - off;
    }
    if (argc >= 6) {
        unsigned long val;

        if (!argv[5][0] || argv[5][0] == '-')
            return -EINVAL;
        errno = 0;
        val = strtoul(argv[5], &tail, 10);
        if (errno || !tail || *tail ||
            val == 0 || val > VTXFS_VAG_BLOCKS - off)
            return -ERANGE;
        count = (__u32)val;
    }

    bitmap = fs->vag.bitmaps[vag_id];
    if (!bitmap)
        return -EUCLEAN;
    pblk_start = vtx_cpu64(fs->vag.descs[vag_id].start_pblk);
    for (i = 0; i < VTXFS_VAG_BLOCKS; i++)
        allocated += (bitmap[i >> 3] >> (i & 7U)) & 1U;

    printf("format=VTXFS_BITMAP_V1\n");
    printf("vag=%lu\n", vag_id);
    printf("total_vags=%u\n", fs->vag.total_vags);
    printf("vag_blocks=%u\n", VTXFS_VAG_BLOCKS);
    printf("block_bytes=%u\n", VTXFS_BLOCK_SIZE);
    printf("start_pblk=%llu\n", (unsigned long long)pblk_start);
    printf("start_offset=%u\n", off);
    printf("count=%u\n", count);
    printf("allocated=%u\n", allocated);
    printf("free_blocks=%u\n",
           vtx_cpu32(fs->vag.descs[vag_id].free_blocks));
    printf("xid=%llu\n", (unsigned long long)fs->committed_xid);
    printf("bitmap_hex=");
    for (i = 0; i < count; i += 8) {
        __u8 byte = 0;
        __u32 bit;

        for (bit = 0; bit < 8 && i + bit < count; bit++) {
            __u32 index = off + i + bit;

            if (bitmap[index >> 3] & ((__u8)1U << (index & 7U)))
                byte |= (__u8)1U << bit;
        }
        printf("%02x", byte);
    }
    putchar('\n');
    return 0;
}

int main(int argc, char **argv)
{
	const char *img_path;
	const char *cmd = "info";
	int is_rw = 0;
	struct vtx_bdev bdev;
	struct vtx_fs *fs = NULL;
	int rc = 0, exit_code = 0;

	if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
	    strcmp(argv[1], "--help") == 0) {
		usage(argv[0]);
		return (argc < 2) ? 1 : 0;
	}

	img_path = argv[1];
	if (argc >= 3)
		cmd = argv[2];

	if (strcmp(cmd, "mkdir") == 0 || strcmp(cmd, "write") == 0 ||
	    strcmp(cmd, "snap") == 0 || strcmp(cmd, "remove") == 0 ||
	    strcmp(cmd, "rename") == 0 || strcmp(cmd, "replace") == 0 ||
	    strcmp(cmd, "put-stdin") == 0 ||
	    strcmp(cmd, "move-safe") == 0 || strcmp(cmd, "copy-file") == 0 ||
	    strcmp(cmd, "symlink") == 0 || strcmp(cmd, "hardlink") == 0 ||
	    strcmp(cmd, "chmod") == 0)
		is_rw = 1;

	rc = vtx_file_bdev_open(&bdev, img_path, is_rw ? 1 : 0, 0);
	if (rc != 0) {
		fprintf(stderr, "vtxfs-inspect: open '%s' failed: %s\n",
			img_path, strerror(-rc));
		return 1;
	}

	fs = vtx_fs_alloc();
	if (!fs) {
		vtx_file_bdev_close(&bdev);
		return 1;
	}

	rc = vtx_mount(fs, &bdev, is_rw ? 0 : 1);
	if (rc != 0) {
		fprintf(stderr, "vtxfs-inspect: mount '%s' failed: %s\n",
			img_path, strerror(-rc));
		vtx_fs_free(fs);
		vtx_file_bdev_close(&bdev);
		return 1;
	}

	if (strcmp(cmd, "info") == 0) {
		exit_code = cmd_info(fs);
	} else if (strcmp(cmd, "bitmap") == 0) {
		rc = cmd_bitmap(fs, argc, argv);
		if (rc)
			exit_code = 1;
	} else if (strcmp(cmd, "ls") == 0) {
		exit_code = cmd_ls(fs, argc >= 4 ? argv[3] : "/");
	} else if (strcmp(cmd, "stat") == 0 && argc >= 4) {
		exit_code = cmd_stat(fs, argv[3]);
	} else if (strcmp(cmd, "cat") == 0 && argc >= 4) {
		exit_code = cmd_cat(fs, argv[3]);
	} else if (strcmp(cmd, "mkdir") == 0 && argc >= 5) {
		__u64 pino = VTXFS_INO_ROOT_DIR;
		__u8 ftype = VTX_FT_DIR;
		__u64 nino = 0;

		if (strcmp(argv[3], "/") == 0) {
			pino = VTXFS_INO_ROOT_DIR;
		} else if (argv[3][0] == '/') {
			rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					     argv[3], &pino, &ftype);
		} else {
			pino = (__u64)strtoull(argv[3], NULL, 10);
		}
		if (rc == 0) {
			rc = vtx_mkdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, pino,
				       argv[4], 0755, &nino);
		}
		if (rc != 0) {
			fprintf(stderr, "mkdir failed: %s\n", strerror(-rc));
			exit_code = 1;
		} else {
			printf("mkdir '%s' -> ino=%llu\n",
			       argv[4], (unsigned long long)nino);
		}
	} else if (strcmp(cmd, "write") == 0 && argc >= 6) {
		__u64 pino = VTXFS_INO_ROOT_DIR;
		__u8 ftype = VTX_FT_DIR;
		__u64 fino = 0;

		if (strcmp(argv[3], "/") == 0) {
			pino = VTXFS_INO_ROOT_DIR;
		} else if (argv[3][0] == '/') {
			rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					     argv[3], &pino, &ftype);
		} else {
			pino = (__u64)strtoull(argv[3], NULL, 10);
		}
		if (rc == 0) {
			rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL, pino,
					argv[4], &fino, NULL);
			if (rc == -ENOENT) {
				rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
						pino, argv[4], 0644, &fino);
			}
		}
		if (rc == 0) {
			rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, fino,
				       0, argv[5], strlen(argv[5]));
		}
		if (rc != 0) {
			fprintf(stderr, "write failed: %s\n", strerror(-rc));
			exit_code = 1;
		} else {
			printf("wrote %zu bytes to '%s' (ino=%llu)\n",
			       strlen(argv[5]), argv[4],
			       (unsigned long long)fino);
		}
    } else if (strcmp(cmd, "symlink") == 0 && argc == 5) {
        rc = cmd_symlink(fs, argv[3], argv[4]);
        if (rc) exit_code = 1;
    } else if (strcmp(cmd, "hardlink") == 0 && argc == 5) {
        rc = cmd_hardlink(fs, argv[3], argv[4]);
        if (rc) exit_code = 1;
    } else if (strcmp(cmd, "readlink") == 0 && argc == 4) {
        rc = cmd_readlink(fs, argv[3]);
        if (rc) exit_code = 1;
    } else if (strcmp(cmd, "chmod") == 0 && argc == 5) {
        rc = cmd_chmod(fs, argv[3], argv[4]);
        if (rc) exit_code = 1;
	} else if (strcmp(cmd, "remove") == 0 && argc >= 4) {
		rc = cmd_remove(fs, argv[3]);
		if (rc)
			exit_code = 1;
 	} else if (strcmp(cmd, "copy-file") == 0 && argc == 5) {
		rc = cmd_copy_file(fs, argv[3], argv[4]);
		if (rc)
			exit_code = 1;
	} else if (strcmp(cmd, "move-safe") == 0 && argc == 5) {
		rc = cmd_move_safe(fs, argv[3], argv[4]);
		if (rc)
			exit_code = 1;
	} else if (strcmp(cmd, "rename") == 0 && argc >= 5) {
		rc = cmd_rename(fs, argv[3], argv[4]);
		if (rc)
			exit_code = 1;
 	} else if (strcmp(cmd, "put-stdin") == 0 && argc == 4) {
		rc = cmd_put_stdin(fs, argv[3]);
		if (rc)
			exit_code = 1;
	} else if (strcmp(cmd, "replace") == 0 && argc >= 5) {
		rc = cmd_replace(fs, argv[3], argv[4]);
		if (rc)
			exit_code = 1;
	} else if (strcmp(cmd, "snap") == 0 && argc >= 4) {
		__u64 snap_id = 0;

		rc = vtx_snapshot_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					 argv[3], 1, &snap_id);
		if (rc != 0) {
			fprintf(stderr, "snap failed: %s\n", strerror(-rc));
			exit_code = 1;
		} else {
			printf("snapshot '%s' -> subvol_id=%llu\n",
			       argv[3], (unsigned long long)snap_id);
		}
	} else {
		usage(argv[0]);
		exit_code = 1;
	}

	if (exit_code && rc < 0)
		fprintf(stderr, "%s failed: %s\n", cmd, strerror(-rc));
	rc = vtx_unmount(fs);
	if (rc != 0 && exit_code == 0)
		exit_code = 1;
	vtx_fs_free(fs);
	vtx_file_bdev_close(&bdev);
	return exit_code;
}
