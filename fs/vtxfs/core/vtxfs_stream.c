// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs_stream.c — Động cơ dòng truyền phát nhị phân VTXFS (VTXSEND1).
 * Hỗ trợ vtxfs send (tuần tự hóa) và vtxfs receive (giải tuần tự hóa và phục hồi).
 */
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <time.h>
#include <libgen.h>

#include "vtxfs_stream.h"
#include "vtxfs_csum.h"

static inline void *vtx_zalloc(size_t size) { return calloc(1, size); }
static inline void vtx_free(void *ptr) { free(ptr); }

/* Nối đường dẫn an toàn không gây format truncation */
static int join_path(char *dst, size_t dst_size, const char *base, const char *sub)
{
	size_t blen = (base && base[0]) ? strlen(base) : 0;
	size_t slen = (sub && sub[0]) ? strlen(sub) : 0;

	if (blen == 0 && slen == 0) {
		if (dst_size < 1)
			return -ENAMETOOLONG;
		dst[0] = '\0';
		return 0;
	}
	if (blen == 0) {
		if (slen + 1 > dst_size)
			return -ENAMETOOLONG;
		memcpy(dst, sub, slen);
		dst[slen] = '\0';
		return 0;
	}
	if (slen == 0) {
		if (blen + 1 > dst_size)
			return -ENAMETOOLONG;
		memcpy(dst, base, blen);
		dst[blen] = '\0';
		return 0;
	}
	if (blen + 1 + slen + 1 > dst_size)
		return -ENAMETOOLONG;

	memcpy(dst, base, blen);
	dst[blen] = '/';
	memcpy(dst + blen + 1, sub, slen);
	dst[blen + 1 + slen] = '\0';
	return 0;
}

/* Cấu trúc theo dõi hardlink trong quá trình send */
struct hardlink_entry {
	dev_t dev;
	ino_t ino;
	char *rel_path;
	struct hardlink_entry *next;
};

/* Cấu trúc trì hoãn cập nhật mốc thời gian của thư mục */
struct dir_utime_entry {
	char *rel_path;
	__u64 atime_sec;
	__u32 atime_nsec;
	__u64 mtime_sec;
	__u32 mtime_nsec;
	struct dir_utime_entry *next;
};

/* Ghi chính xác n byte vào file descriptor */
static int write_exact(int fd, const void *buf, size_t count)
{
	const __u8 *p = (const __u8 *)buf;
	size_t left = count;

	while (left > 0) {
		ssize_t ret = write(fd, p, left);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			return -EIO;
		left -= (size_t)ret;
		p += ret;
	}
	return 0;
}

/* Đọc chính xác n byte từ file descriptor */
static int read_exact(int fd, void *buf, size_t count)
{
	__u8 *p = (__u8 *)buf;
	size_t left = count;

	while (left > 0) {
		ssize_t ret = read(fd, p, left);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0) {
			if (left == count)
				return 0;
			return -EPIPE;
		}
		left -= (size_t)ret;
		p += ret;
	}
	return (int)count;
}

/* Gửi một gói lệnh vào stream */
static int send_packet(int out_fd, __u16 cmd, __u16 flags,
		       const void *payload, __u32 len,
		       __u32 *running_crc, struct vtx_stream_stats *stats)
{
	struct vtx_stream_cmd_hdr hdr;
	int ret;

	hdr.cmd = vtx_le16(cmd);
	hdr.flags = vtx_le16(flags);
	hdr.payload_len = vtx_le32(len);
	hdr.crc32c = vtx_le32(len > 0 ? vtx_crc32c(payload, len) : 0);

	ret = write_exact(out_fd, &hdr, sizeof(hdr));
	if (ret)
		return ret;

	if (len > 0) {
		ret = write_exact(out_fd, payload, len);
		if (ret)
			return ret;
		if (running_crc)
			*running_crc = vtx_crc32c_update(*running_crc, payload, len);
	}

	if (stats) {
		stats->total_packets++;
		stats->total_bytes += sizeof(hdr) + len;
	}
	return 0;
}

/* Đọc một gói lệnh từ stream */
static int read_packet(int in_fd, struct vtx_stream_cmd_hdr *hdr_out,
		       __u8 **payload_out, __u32 *running_crc)
{
	struct vtx_stream_cmd_hdr hdr;
	int ret;
	__u32 len, crc;
	__u8 *buf = NULL;

	ret = read_exact(in_fd, &hdr, sizeof(hdr));
	if (ret == 0)
		return 0; /* Clean EOF */
	if (ret < 0)
		return ret;

	hdr_out->cmd = vtx_cpu16(hdr.cmd);
	hdr_out->flags = vtx_cpu16(hdr.flags);
	hdr_out->payload_len = vtx_cpu32(hdr.payload_len);
	hdr_out->crc32c = vtx_cpu32(hdr.crc32c);

	len = hdr_out->payload_len;
	crc = hdr_out->crc32c;

	if (len > VTXFS_STREAM_MAX_PAYLOAD) {
		fprintf(stderr, "vtxfs_stream: payload quá lớn: %u bytes\n", len);
		return -E2BIG;
	}

	if (len > 0) {
		buf = (__u8 *)vtx_zalloc(len);
		if (!buf)
			return -ENOMEM;
		ret = read_exact(in_fd, buf, len);
		if (ret != (int)len) {
			vtx_free(buf);
			return ret < 0 ? ret : -EPIPE;
		}

		/* Kiểm tra mã băm CRC32c bảo vệ dữ liệu */
		if (vtx_crc32c(buf, len) != crc) {
			fprintf(stderr, "vtxfs_stream: lỗi sai lệch CRC32c gói lệnh %u!\n",
				hdr_out->cmd);
			vtx_free(buf);
			return -EBADMSG;
		}

		if (running_crc)
			*running_crc = vtx_crc32c_update(*running_crc, buf, len);
	}

	*payload_out = buf;
	return 1; /* Read 1 packet */
}

/* Kiểm tra và ghi nhận hardlink */
static const char *check_hardlink(struct hardlink_entry **head, dev_t dev,
				  ino_t ino, const char *rel_path)
{
	struct hardlink_entry *cur = *head;

	while (cur) {
		if (cur->dev == dev && cur->ino == ino)
			return cur->rel_path;
		cur = cur->next;
	}

	struct hardlink_entry *node = vtx_zalloc(sizeof(*node));
	if (node) {
		node->dev = dev;
		node->ino = ino;
		node->rel_path = strdup(rel_path);
		node->next = *head;
		*head = node;
	}
	return NULL;
}

static void free_hardlinks(struct hardlink_entry *head)
{
	while (head) {
		struct hardlink_entry *next = head->next;
		free(head->rel_path);
		vtx_free(head);
		head = next;
	}
}

/* Thêm thư mục vào danh sách cập nhật utimes sau cùng */
static void add_dir_utime(struct dir_utime_entry **head, const char *rel_path,
			  const struct stat *st)
{
	struct dir_utime_entry *node = vtx_zalloc(sizeof(*node));
	if (!node)
		return;
	node->rel_path = strdup(rel_path);
	node->atime_sec = (__u64)st->st_atim.tv_sec;
	node->atime_nsec = (__u32)st->st_atim.tv_nsec;
	node->mtime_sec = (__u64)st->st_mtim.tv_sec;
	node->mtime_nsec = (__u32)st->st_mtim.tv_nsec;
	node->next = *head;
	*head = node;
}

static void free_dir_utimes(struct dir_utime_entry *head)
{
	while (head) {
		struct dir_utime_entry *next = head->next;
		free(head->rel_path);
		vtx_free(head);
		head = next;
	}
}

/* Gửi các xattr của tệp/thư mục */
static void send_xattrs(int out_fd, const char *full_path, const char *rel_path,
			__u32 *running_crc, struct vtx_stream_stats *stats)
{
	char list[4096];
	ssize_t list_len = llistxattr(full_path, list, sizeof(list));

	if (list_len <= 0)
		return;

	const char *name = list;
	while (name < list + list_len) {
		char val[4096];
		ssize_t val_len = lgetxattr(full_path, name, val, sizeof(val));

		if (val_len >= 0) {
			size_t plen = strlen(rel_path);
			size_t nlen = strlen(name);
			size_t payload_len = 2 + 2 + 4 + plen + nlen + (size_t)val_len;
			__u8 *buf = (__u8 *)vtx_zalloc(payload_len);

			if (buf) {
				__u8 *p = buf;
				*(__le16 *)p = vtx_le16((__u16)plen); p += 2;
				*(__le16 *)p = vtx_le16((__u16)nlen); p += 2;
				*(__le32 *)p = vtx_le32((__u32)val_len); p += 4;
				memcpy(p, rel_path, plen); p += plen;
				memcpy(p, name, nlen); p += nlen;
				if (val_len > 0)
					memcpy(p, val, (size_t)val_len);

				send_packet(out_fd, VTX_CMD_SET_XATTR, 0, buf,
					    (__u32)payload_len, running_crc, stats);
				if (stats)
					stats->total_xattrs++;
				vtx_free(buf);
			}
		}
		name += strlen(name) + 1;
	}
}

/* Gửi mốc thời gian utimes của một đối tượng */
static int send_utimes(int out_fd, const char *rel_path,
		       __u64 atime_sec, __u32 atime_nsec,
		       __u64 mtime_sec, __u32 mtime_nsec,
		       __u32 *running_crc, struct vtx_stream_stats *stats)
{
	size_t plen = strlen(rel_path);
	size_t payload_len = 8 + 4 + 8 + 4 + 2 + plen;
	__u8 *buf = (__u8 *)vtx_zalloc(payload_len);
	int ret;

	if (!buf)
		return -ENOMEM;

	__u8 *p = buf;
	*(__le64 *)p = vtx_le64(atime_sec); p += 8;
	*(__le32 *)p = vtx_le32(atime_nsec); p += 4;
	*(__le64 *)p = vtx_le64(mtime_sec); p += 8;
	*(__le32 *)p = vtx_le32(mtime_nsec); p += 4;
	*(__le16 *)p = vtx_le16((__u16)plen); p += 2;
	memcpy(p, rel_path, plen);

	ret = send_packet(out_fd, VTX_CMD_UTIMES, 0, buf, (__u32)payload_len,
			  running_crc, stats);
	vtx_free(buf);
	return ret;
}

/* Đệ quy duyệt cây thư mục và gửi qua stream */
static int send_tree_recursive(int out_fd, const char *root_dir, const char *rel_path,
			       struct hardlink_entry **hlinks,
			       struct dir_utime_entry **dir_utimes,
			       __u32 *running_crc, struct vtx_stream_stats *stats,
			       int verbose)
{
	char full_path[4096];
	struct stat st;
	int ret = 0;

	if (join_path(full_path, sizeof(full_path), root_dir, rel_path) != 0)
		return -ENAMETOOLONG;

	if (lstat(full_path, &st) != 0)
		return -errno;

	/* 1. Xử lý Thư mục (Directory) */
	if (S_ISDIR(st.st_mode)) {
		if (rel_path[0] != '\0') {
			size_t plen = strlen(rel_path);
			size_t payload_len = 4 + 4 + 4 + 2 + plen;
			__u8 *buf = (__u8 *)vtx_zalloc(payload_len);
			if (!buf)
				return -ENOMEM;

			__u8 *p = buf;
			*(__le32 *)p = vtx_le32((__u32)st.st_mode); p += 4;
			*(__le32 *)p = vtx_le32((__u32)st.st_uid);  p += 4;
			*(__le32 *)p = vtx_le32((__u32)st.st_gid);  p += 4;
			*(__le16 *)p = vtx_le16((__u16)plen);       p += 2;
			memcpy(p, rel_path, plen);

			ret = send_packet(out_fd, VTX_CMD_MKDIR, 0, buf,
					  (__u32)payload_len, running_crc, stats);
			vtx_free(buf);
			if (ret)
				return ret;

			if (stats)
				stats->total_dirs++;
			if (verbose)
				printf("  [MKDIR]   %s (mode: 0%o)\n",
				       rel_path, (unsigned int)(st.st_mode & 07777));

			send_xattrs(out_fd, full_path, rel_path, running_crc, stats);
			add_dir_utime(dir_utimes, rel_path, &st);
		}

		DIR *d = opendir(full_path);
		if (!d)
			return -errno;

		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;

			char sub_rel[4096];
			if (join_path(sub_rel, sizeof(sub_rel), rel_path, de->d_name) != 0)
				continue;

			ret = send_tree_recursive(out_fd, root_dir, sub_rel,
						  hlinks, dir_utimes,
						  running_crc, stats, verbose);
			if (ret)
				break;
		}
		closedir(d);
		return ret;
	}

	/* 2. Xử lý Liên kết mềm (Symlink) */
	if (S_ISLNK(st.st_mode)) {
		char target[4096];
		ssize_t tlen = readlink(full_path, target, sizeof(target) - 1);
		if (tlen < 0)
			return -errno;
		target[tlen] = '\0';

		size_t plen = strlen(rel_path);
		size_t payload_len = 2 + 2 + plen + (size_t)tlen;
		__u8 *buf = (__u8 *)vtx_zalloc(payload_len);
		if (!buf)
			return -ENOMEM;

		__u8 *p = buf;
		*(__le16 *)p = vtx_le16((__u16)plen); p += 2;
		*(__le16 *)p = vtx_le16((__u16)tlen); p += 2;
		memcpy(p, rel_path, plen); p += plen;
		memcpy(p, target, (size_t)tlen);

		ret = send_packet(out_fd, VTX_CMD_SYMLINK, 0, buf,
				  (__u32)payload_len, running_crc, stats);
		vtx_free(buf);
		if (ret)
			return ret;

		if (stats)
			stats->total_symlinks++;
		if (verbose)
			printf("  [SYMLINK] %s -> %s\n", rel_path, target);

		send_xattrs(out_fd, full_path, rel_path, running_crc, stats);
		send_utimes(out_fd, rel_path,
			    (__u64)st.st_atim.tv_sec, (__u32)st.st_atim.tv_nsec,
			    (__u64)st.st_mtim.tv_sec, (__u32)st.st_mtim.tv_nsec,
			    running_crc, stats);
		return 0;
	}

	/* 3. Xử lý Tệp thông thường (Regular File) */
	if (S_ISREG(st.st_mode)) {
		if (st.st_nlink > 1) {
			const char *target_rel = check_hardlink(hlinks, st.st_dev,
								st.st_ino, rel_path);
			if (target_rel) {
				size_t plen = strlen(rel_path);
				size_t tlen = strlen(target_rel);
				size_t payload_len = 2 + 2 + plen + tlen;
				__u8 *buf = (__u8 *)vtx_zalloc(payload_len);
				if (!buf)
					return -ENOMEM;

				__u8 *p = buf;
				*(__le16 *)p = vtx_le16((__u16)plen); p += 2;
				*(__le16 *)p = vtx_le16((__u16)tlen); p += 2;
				memcpy(p, rel_path, plen); p += plen;
				memcpy(p, target_rel, tlen);

				ret = send_packet(out_fd, VTX_CMD_LINK, 0, buf,
						  (__u32)payload_len, running_crc, stats);
				vtx_free(buf);
				if (stats)
					stats->total_hardlinks++;
				if (verbose)
					printf("  [HARDLINK] %s -> %s\n", rel_path, target_rel);
				return ret;
			}
		}

		size_t plen = strlen(rel_path);
		size_t payload_len = 4 + 4 + 4 + 8 + 2 + plen;
		__u8 *buf = (__u8 *)vtx_zalloc(payload_len);
		if (!buf)
			return -ENOMEM;

		__u8 *p = buf;
		*(__le32 *)p = vtx_le32((__u32)st.st_mode);     p += 4;
		*(__le32 *)p = vtx_le32((__u32)st.st_uid);      p += 4;
		*(__le32 *)p = vtx_le32((__u32)st.st_gid);      p += 4;
		*(__le64 *)p = vtx_le64((__u64)st.st_size);     p += 8;
		*(__le16 *)p = vtx_le16((__u16)plen);           p += 2;
		memcpy(p, rel_path, plen);

		ret = send_packet(out_fd, VTX_CMD_MKFILE, 0, buf,
				  (__u32)payload_len, running_crc, stats);
		vtx_free(buf);
		if (ret)
			return ret;

		if (stats)
			stats->total_files++;
		if (verbose)
			printf("  [MKFILE]  %s (size: %llu bytes)\n",
			       rel_path, (unsigned long long)st.st_size);

		if (st.st_size > 0) {
			int in_fd = open(full_path, O_RDONLY);
			if (in_fd >= 0) {
				__u64 offset = 0;
				__u8 chunk[VTXFS_STREAM_CHUNK_SIZE];

				while (offset < (__u64)st.st_size) {
					ssize_t r = pread(in_fd, chunk, sizeof(chunk), (off_t)offset);
					if (r <= 0)
						break;

					size_t w_len = 8 + 4 + 2 + plen + (size_t)r;
					__u8 *w_buf = (__u8 *)vtx_zalloc(w_len);
					if (w_buf) {
						__u8 *wp = w_buf;
						*(__le64 *)wp = vtx_le64(offset);         wp += 8;
						*(__le32 *)wp = vtx_le32((__u32)r);       wp += 4;
						*(__le16 *)wp = vtx_le16((__u16)plen);    wp += 2;
						memcpy(wp, rel_path, plen);               wp += plen;
						memcpy(wp, chunk, (size_t)r);

						ret = send_packet(out_fd, VTX_CMD_WRITE, 0,
								  w_buf, (__u32)w_len,
								  running_crc, stats);
						vtx_free(w_buf);
						if (ret)
							break;
					}
					offset += (size_t)r;
				}
				close(in_fd);
				if (ret)
					return ret;
			}
		}

		send_xattrs(out_fd, full_path, rel_path, running_crc, stats);
		send_utimes(out_fd, rel_path,
			    (__u64)st.st_atim.tv_sec, (__u32)st.st_atim.tv_nsec,
			    (__u64)st.st_mtim.tv_sec, (__u32)st.st_mtim.tv_nsec,
			    running_crc, stats);
		return 0;
	}

	return 0;
}

/* Thực thi vtxfs send */
int vtx_stream_send(const struct vtx_send_options *opts,
		    struct vtx_stream_stats *stats_out)
{
	struct vtx_stream_stats stats;
	struct vtx_stream_header hdr;
	struct hardlink_entry *hlinks = NULL;
	struct dir_utime_entry *dir_utimes = NULL;
	struct timespec t_start, t_end;
	int out_fd = STDOUT_FILENO;
	int ret = 0;
	__u32 running_crc = 0;

	memset(&stats, 0, sizeof(stats));
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	if (opts->out_file) {
		out_fd = open(opts->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out_fd < 0) {
			fprintf(stderr, "vtxfs_send: không thể mở file xuất '%s': %s\n",
				opts->out_file, strerror(errno));
			return -errno;
		}
	}

	/* Chuẩn bị Stream Header */
	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, VTXFS_SEND_STREAM_MAGIC, 8);
	hdr.version = vtx_le32(VTXFS_SEND_STREAM_VERSION);
	hdr.flags = 0;
	hdr.subvol_id = vtx_le64(opts->subvol_id ? opts->subvol_id : 256);
	hdr.parent_subvol_id = 0;
	hdr.root_xid = vtx_le64(1);

	char *tmp = strdup(opts->source_path);
	snprintf(hdr.subvol_name, sizeof(hdr.subvol_name), "%s", basename(tmp));
	free(tmp);

	hdr.header_crc = vtx_le32(vtx_crc32c(&hdr, sizeof(hdr) - sizeof(__le32)));

	/* Ghi stream header */
	ret = write_exact(out_fd, &hdr, sizeof(hdr));
	if (ret) {
		fprintf(stderr, "vtxfs_send: ghi stream header thất bại: %s\n",
			strerror(-ret));
		goto out;
	}
	stats.total_bytes += sizeof(hdr);

	/* Gửi VTX_CMD_SUBVOL */
	{
		size_t nlen = strlen(hdr.subvol_name);
		size_t plen = 8 + 8 + 8 + 2 + nlen;
		__u8 *buf = (__u8 *)vtx_zalloc(plen);
		if (buf) {
			__u8 *p = buf;
			*(__le64 *)p = hdr.subvol_id;        p += 8;
			*(__le64 *)p = hdr.root_xid;         p += 8;
			*(__le64 *)p = 0;                    p += 8;
			*(__le16 *)p = vtx_le16((__u16)nlen);p += 2;
			memcpy(p, hdr.subvol_name, nlen);

			send_packet(out_fd, VTX_CMD_SUBVOL, 0, buf,
				    (__u32)plen, &running_crc, &stats);
			vtx_free(buf);
		}
	}

	/* Duyệt và gửi toàn bộ cây tệp/thư mục */
	ret = send_tree_recursive(out_fd, opts->source_path, "",
				  &hlinks, &dir_utimes,
				  &running_crc, &stats, opts->verbose);
	if (ret) {
		fprintf(stderr, "vtxfs_send: lỗi trong khi duyệt gửi tệp: %s\n",
			strerror(-ret));
		goto out;
	}

	/* Áp dụng mốc thời gian utimes cho các thư mục */
	struct dir_utime_entry *due = dir_utimes;
	while (due) {
		send_utimes(out_fd, due->rel_path,
			    due->atime_sec, due->atime_nsec,
			    due->mtime_sec, due->mtime_nsec,
			    &running_crc, &stats);
		due = due->next;
	}

	/* Gửi gói kết thúc VTX_CMD_END */
	{
		size_t plen = 8 + 8 + 4;
		__u8 buf[20];
		__u8 *p = buf;
		*(__le64 *)p = vtx_le64(stats.total_files); p += 8;
		*(__le64 *)p = vtx_le64(stats.total_bytes); p += 8;
		*(__le32 *)p = vtx_le32(running_crc);

		send_packet(out_fd, VTX_CMD_END, 0, buf,
			    (__u32)plen, &running_crc, &stats);
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);
	stats.elapsed_sec = (double)(t_end.tv_sec - t_start.tv_sec) +
			    (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;
	stats.stream_crc = running_crc;

	if (stats_out)
		*stats_out = stats;

out:
	free_hardlinks(hlinks);
	free_dir_utimes(dir_utimes);
	if (opts->out_file && out_fd >= 0)
		close(out_fd);
	return ret;
}

/* Đảm bảo đường dẫn cha tồn tại trước khi tạo tệp/thư mục con */
static int ensure_parent_dir(const char *path)
{
	char *tmp = strdup(path);
	char *dir = dirname(tmp);
	struct stat st;

	if (stat(dir, &st) == 0) {
		free(tmp);
		return 0;
	}

	ensure_parent_dir(dir);
	mkdir(dir, 0755);
	free(tmp);
	return 0;
}

/* Kiểm tra bảo mật tránh tấn công path traversal và chuẩn hóa đường dẫn */
static const char *sanitize_rel_path(const char *path)
{
	if (!path)
		return NULL;
	while (*path == '/')
		path++;
	if (*path == '\0')
		return NULL;

	if (!strcmp(path, "..") || strstr(path, "../") || strstr(path, "/.."))
		return NULL;

	return path;
}

/* Thực thi vtxfs receive */
int vtx_stream_receive(const struct vtx_receive_options *opts,
		       struct vtx_stream_stats *stats_out)
{
	struct vtx_stream_stats stats;
	struct vtx_stream_header hdr;
	struct timespec t_start, t_end;
	int in_fd = STDIN_FILENO;
	int ret = 0;
	__u32 running_crc = 0;

	memset(&stats, 0, sizeof(stats));
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	if (opts->in_file) {
		in_fd = open(opts->in_file, O_RDONLY);
		if (in_fd < 0) {
			fprintf(stderr, "vtxfs_receive: không thể mở file nhập '%s': %s\n",
				opts->in_file, strerror(errno));
			return -errno;
		}
	}

	/* Đọc và kiểm tra Stream Header */
	ret = read_exact(in_fd, &hdr, sizeof(hdr));
	if (ret != sizeof(hdr)) {
		fprintf(stderr, "vtxfs_receive: không thể đọc stream header (file quá ngắn/rỗng)\n");
		ret = -EBADF;
		goto out;
	}

	if (memcmp(hdr.magic, VTXFS_SEND_STREAM_MAGIC, 8) != 0) {
		fprintf(stderr, "vtxfs_receive: magic không hợp lệ, không phải stream VTXFS!\n");
		ret = -EINVAL;
		goto out;
	}

	if (vtx_cpu32(hdr.version) > VTXFS_SEND_STREAM_VERSION) {
		fprintf(stderr, "vtxfs_receive: phiên bản stream %u không được hỗ trợ\n",
			vtx_cpu32(hdr.version));
		ret = -EPROTONOSUPPORT;
		goto out;
	}

	__u32 exp_hdr_crc = vtx_crc32c(&hdr, sizeof(hdr) - sizeof(__le32));
	if (vtx_cpu32(hdr.header_crc) != exp_hdr_crc) {
		fprintf(stderr, "vtxfs_receive: kiểm tra checksum stream header thất bại!\n");
		ret = -EBADMSG;
		goto out;
	}

	stats.total_bytes += sizeof(hdr);

	ensure_parent_dir(opts->target_path);
	mkdir(opts->target_path, 0755);

	if (opts->verbose) {
		printf(">>> Đang nhận stream subvolume '%s' (ID %llu, gen %llu)...\n",
		       hdr.subvol_name,
		       (unsigned long long)vtx_cpu64(hdr.subvol_id),
		       (unsigned long long)vtx_cpu64(hdr.root_xid));
	}

	/* Vòng lặp nhận và phục hồi từng gói lệnh */
	for (;;) {
		struct vtx_stream_cmd_hdr chdr;
		__u8 *payload = NULL;

		ret = read_packet(in_fd, &chdr, &payload, &running_crc);
		if (ret == 0)
			break;
		if (ret < 0) {
			fprintf(stderr, "vtxfs_receive: lỗi đọc gói lệnh: %s\n",
				strerror(-ret));
			goto out;
		}

		stats.total_packets++;
		stats.total_bytes += sizeof(chdr) + chdr.payload_len;

		switch (chdr.cmd) {
		case VTX_CMD_SUBVOL:
			break;

		case VTX_CMD_MKDIR: {
			if (chdr.payload_len < 14)
				break;
			const __u8 *p = payload;
			__u32 mode = vtx_cpu32(*(__le32 *)p); p += 4;
			__u32 uid  = vtx_cpu32(*(__le32 *)p); p += 4;
			__u32 gid  = vtx_cpu32(*(__le32 *)p); p += 4;
			__u16 plen = vtx_cpu16(*(__le16 *)p); p += 2;

			if (plen > 0) {
				char raw[4096];
				snprintf(raw, sizeof(raw), "%.*s", (int)plen, (const char *)p);
				const char *rel = sanitize_rel_path(raw);
				char full[4096];
				if (rel && join_path(full, sizeof(full), opts->target_path, rel) == 0) {
					ensure_parent_dir(full);
					mkdir(full, (mode_t)mode);
					chmod(full, (mode_t)mode);
					(void)lchown(full, (uid_t)uid, (gid_t)gid);

					stats.total_dirs++;
					if (opts->verbose)
						printf("  [RESTORE DIR]     %s\n", rel);
				}
			}
			break;
		}

		case VTX_CMD_MKFILE: {
			if (chdr.payload_len < 22)
				break;
			const __u8 *p = payload;
			__u32 mode = vtx_cpu32(*(__le32 *)p); p += 4;
			__u32 uid  = vtx_cpu32(*(__le32 *)p); p += 4;
			__u32 gid  = vtx_cpu32(*(__le32 *)p); p += 4;
			__u64 size = vtx_cpu64(*(__le64 *)p); p += 8;
			__u16 plen = vtx_cpu16(*(__le16 *)p); p += 2;

			if (plen > 0) {
				char raw[4096];
				snprintf(raw, sizeof(raw), "%.*s", (int)plen, (const char *)p);
				const char *rel = sanitize_rel_path(raw);
				char full[4096];
				if (rel && join_path(full, sizeof(full), opts->target_path, rel) == 0) {
					ensure_parent_dir(full);
					int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
					if (fd >= 0) {
						if (size > 0)
							(void)ftruncate(fd, (off_t)size);
						close(fd);
					}
					chmod(full, (mode_t)mode);
					(void)lchown(full, (uid_t)uid, (gid_t)gid);

					stats.total_files++;
					if (opts->verbose)
						printf("  [RESTORE FILE]    %s (%llu bytes)\n",
						       rel, (unsigned long long)size);
				}
			}
			break;
		}

		case VTX_CMD_WRITE: {
			if (chdr.payload_len < 14)
				break;
			const __u8 *p = payload;
			__u64 offset   = vtx_cpu64(*(__le64 *)p); p += 8;
			__u32 data_len = vtx_cpu32(*(__le32 *)p); p += 4;
			__u16 plen     = vtx_cpu16(*(__le16 *)p); p += 2;

			if (plen > 0) {
				char raw[4096];
				snprintf(raw, sizeof(raw), "%.*s", (int)plen, (const char *)p);
				p += plen;
				const char *rel = sanitize_rel_path(raw);
				char full[4096];
				if (rel && join_path(full, sizeof(full), opts->target_path, rel) == 0) {
					int fd = open(full, O_WRONLY);
					if (fd >= 0) {
						pwrite(fd, p, (size_t)data_len, (off_t)offset);
						close(fd);
					}
				}
			}
			break;
		}

		case VTX_CMD_SYMLINK: {
			if (chdr.payload_len < 4)
				break;
			const __u8 *p = payload;
			__u16 plen = vtx_cpu16(*(__le16 *)p); p += 2;
			__u16 tlen = vtx_cpu16(*(__le16 *)p); p += 2;

			if (plen > 0 && tlen > 0) {
				char raw[4096];
				snprintf(raw, sizeof(raw), "%.*s", (int)plen, (const char *)p);
				p += plen;
				const char *rel = sanitize_rel_path(raw);

				char target[4096];
				snprintf(target, sizeof(target), "%.*s", (int)tlen, (const char *)p);

				char full[4096];
				if (rel && join_path(full, sizeof(full), opts->target_path, rel) == 0) {
					ensure_parent_dir(full);
					unlink(full);
					symlink(target, full);

					stats.total_symlinks++;
					if (opts->verbose)
						printf("  [RESTORE SYMLINK] %s -> %s\n", rel, target);
				}
			}
			break;
		}

		case VTX_CMD_LINK: {
			if (chdr.payload_len < 4)
				break;
			const __u8 *p = payload;
			__u16 plen = vtx_cpu16(*(__le16 *)p); p += 2;
			__u16 tlen = vtx_cpu16(*(__le16 *)p); p += 2;

			if (plen > 0 && tlen > 0) {
				char raw_new[4096];
				snprintf(raw_new, sizeof(raw_new), "%.*s", (int)plen, (const char *)p);
				p += plen;
				const char *rel_new = sanitize_rel_path(raw_new);

				char raw_old[4096];
				snprintf(raw_old, sizeof(raw_old), "%.*s", (int)tlen, (const char *)p);
				const char *rel_old = sanitize_rel_path(raw_old);

				char full_new[4096];
				char full_old[4096];
				if (rel_new && rel_old &&
				    join_path(full_new, sizeof(full_new), opts->target_path, rel_new) == 0 &&
				    join_path(full_old, sizeof(full_old), opts->target_path, rel_old) == 0) {
					ensure_parent_dir(full_new);
					unlink(full_new);
					link(full_old, full_new);

					stats.total_hardlinks++;
					if (opts->verbose)
						printf("  [RESTORE HARDLINK] %s -> %s\n", rel_new, rel_old);
				}
			}
			break;
		}

		case VTX_CMD_SET_XATTR: {
			if (chdr.payload_len < 8)
				break;
			const __u8 *p = payload;
			__u16 plen = vtx_cpu16(*(__le16 *)p); p += 2;
			__u16 nlen = vtx_cpu16(*(__le16 *)p); p += 2;
			__u32 vlen = vtx_cpu32(*(__le32 *)p); p += 4;

			if (plen > 0 && nlen > 0) {
				char raw[4096];
				snprintf(raw, sizeof(raw), "%.*s", (int)plen, (const char *)p);
				p += plen;
				const char *rel = sanitize_rel_path(raw);

				char name[256];
				snprintf(name, sizeof(name), "%.*s", (int)nlen, (const char *)p);
				p += nlen;

				char full[4096];
				if (rel && join_path(full, sizeof(full), opts->target_path, rel) == 0) {
					lsetxattr(full, name, p, (size_t)vlen, 0);
					stats.total_xattrs++;
				}
			}
			break;
		}

		case VTX_CMD_UTIMES: {
			if (chdr.payload_len < 26)
				break;
			const __u8 *p = payload;
			__u64 atime_sec  = vtx_cpu64(*(__le64 *)p); p += 8;
			__u32 atime_nsec = vtx_cpu32(*(__le32 *)p); p += 4;
			__u64 mtime_sec  = vtx_cpu64(*(__le64 *)p); p += 8;
			__u32 mtime_nsec = vtx_cpu32(*(__le32 *)p); p += 4;
			__u16 plen       = vtx_cpu16(*(__le16 *)p); p += 2;

			if (plen > 0) {
				char raw[4096];
				snprintf(raw, sizeof(raw), "%.*s", (int)plen, (const char *)p);
				const char *rel = sanitize_rel_path(raw);

				char full[4096];
				if (rel && join_path(full, sizeof(full), opts->target_path, rel) == 0) {
					struct timespec ts[2];
					ts[0].tv_sec  = (time_t)atime_sec;
					ts[0].tv_nsec = (long)atime_nsec;
					ts[1].tv_sec  = (time_t)mtime_sec;
					ts[1].tv_nsec = (long)mtime_nsec;

					utimensat(AT_FDCWD, full, ts, AT_SYMLINK_NOFOLLOW);
				}
			}
			break;
		}

		case VTX_CMD_END: {
			if (chdr.payload_len >= 20) {
				const __u8 *p = payload;
				p += 16;
				__u32 end_crc = vtx_cpu32(*(__le32 *)p);
				(void)end_crc;
			}
			vtx_free(payload);
			goto finish;
		}

		default:
			break;
		}

		vtx_free(payload);
	}

finish:
	ret = 0;
	clock_gettime(CLOCK_MONOTONIC, &t_end);
	stats.elapsed_sec = (double)(t_end.tv_sec - t_start.tv_sec) +
			    (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;
	stats.stream_crc = running_crc;

	if (stats_out)
		*stats_out = stats;

out:
	if (opts->in_file && in_fd >= 0)
		close(in_fd);
	return ret;
}
