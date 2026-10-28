// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#define _GNU_SOURCE
/*
 * cmds-frag.c — Phân tích độ phân mảnh trực tuyến (Online FIEMAP) thuần C
 * Thay thế hoàn toàn vtxfs_frag.py:
 *   1. Quét ánh xạ extent vật lý trực tiếp qua FS_IOC_FIEMAP ioctl.
 *   2. Đếm số điểm đứt gãy (discontinuities) và tính tỷ lệ phân mảnh.
 *   3. Phân tích tệp đơn lẻ hoặc đệ quy toàn bộ thư mục.
 *   4. Báo cáo bảng ánh xạ PBLK và danh sách các tệp bị phân mảnh nhiều nhất.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include "commands.h"

#define MAX_EXTENTS 2048
#define VTX_FRAG_BLOCK_SIZE 4096

struct file_frag_stat {
	char path[1024];
	uint64_t size_bytes;
	uint32_t blocks;
	uint32_t extents;
	uint32_t discontinuities;
	double frag_pct;
};

static int compare_frag_stat(const void *a, const void *b)
{
	const struct file_frag_stat *fa = (const struct file_frag_stat *)a;
	const struct file_frag_stat *fb = (const struct file_frag_stat *)b;

	if (fa->discontinuities != fb->discontinuities)
		return (fb->discontinuities > fa->discontinuities) ? 1 : -1;
	if (fa->extents != fb->extents)
		return (fb->extents > fa->extents) ? 1 : -1;
	return (fb->size_bytes > fa->size_bytes) ? 1 : -1;
}

static int query_fiemap(int fd, uint64_t file_size, struct fiemap **out_fiemap)
{
	size_t map_sz = sizeof(struct fiemap) + MAX_EXTENTS * sizeof(struct fiemap_extent);
	struct fiemap *map = malloc(map_sz);

	if (!map)
		return -ENOMEM;

	memset(map, 0, map_sz);
	map->fm_start = 0;
	map->fm_length = file_size;
	map->fm_flags = FIEMAP_FLAG_SYNC;
	map->fm_extent_count = MAX_EXTENTS;

	if (ioctl(fd, FS_IOC_FIEMAP, map) < 0) {
		int err = -errno;
		free(map);
		return err;
	}

	*out_fiemap = map;
	return 0;
}

static int analyze_single_file(const char *filepath, int verbose)
{
	int fd = open(filepath, O_RDONLY);
	struct stat st;
	struct fiemap *map = NULL;
	uint32_t blocks, discontinuities = 0;
	double frag_pct = 0.0;
	__u32 i;

	if (fd < 0) {
		fprintf(stderr, "Lỗi mở tệp '%s': %s\n", filepath, strerror(errno));
		return 1;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		return 1;
	}

	if (st.st_size == 0) {
		printf("Tệp tin rỗng (0 bytes, 0 blocks).\n");
		close(fd);
		return 0;
	}

	blocks = (st.st_size + VTX_FRAG_BLOCK_SIZE - 1) / VTX_FRAG_BLOCK_SIZE;

	if (query_fiemap(fd, (uint64_t)st.st_size, &map) < 0) {
		fprintf(stderr, "Không thể đọc FIEMAP của '%s' (có thể filesystem không hỗ trợ hoặc quyền hạn thấp)\n", filepath);
		close(fd);
		return 1;
	}

	close(fd);

	for (i = 1; i < map->fm_mapped_extents; i++) {
		__u64 prev_phys_end = map->fm_extents[i - 1].fe_physical + map->fm_extents[i - 1].fe_length;
		if (map->fm_extents[i].fe_physical != prev_phys_end)
			discontinuities++;
	}

	if (blocks > 1)
		frag_pct = ((double)discontinuities / (double)blocks) * 100.0;

	printf("==================================================================================\n");
	printf("  VTXFS — PHÂN TÍCH ĐỘ PHÂN MẢNH TRỰC TUYẾN (NATIVE C FIEMAP)\n");
	printf("==================================================================================\n");
	printf("  Tệp tin           : %s\n", filepath);
	printf("  Dung lượng        : %llu bytes (%u khối 4KB)\n",
	       (unsigned long long)st.st_size, blocks);
	printf("  Số đoạn extents   : %u đoạn\n", map->fm_mapped_extents);
	printf("  Điểm đứt gãy      : %u điểm\n", discontinuities);
	printf("  Tỷ lệ phân mảnh   : %.2f%%\n", frag_pct);
	printf("  Trạng thái        : %s\n",
	       discontinuities == 0 ? "HOÀN HẢO (100% Liền mạch / Contiguous)" : "BỊ PHÂN MẢNH");

	if (verbose || map->fm_mapped_extents <= 32) {
		printf("\n  BẢNG ÁNH XẠ EXTENT VẬT LÝ CHI TIẾT:\n");
		printf("  --------------------------------------------------------------------------------\n");
		printf("  Ext   Logical Offset (4K)      Physical Offset (PBLK)      Khối     Cờ (Flags)\n");
		printf("  --------------------------------------------------------------------------------\n");
		for (i = 0; i < map->fm_mapped_extents; i++) {
			struct fiemap_extent *e = &map->fm_extents[i];
			uint64_t log_blks = e->fe_length / VTX_FRAG_BLOCK_SIZE;
			uint64_t l_start = e->fe_logical / VTX_FRAG_BLOCK_SIZE;
			uint64_t l_end = l_start + (log_blks ? log_blks - 1 : 0);
			uint64_t p_start = e->fe_physical / VTX_FRAG_BLOCK_SIZE;
			uint64_t p_end = p_start + (log_blks ? log_blks - 1 : 0);

			printf("  %-4u  %6llu .. %-6llu      ->  pblk %7llu .. %-7llu   %5llu blks  %s%s%s\n",
			       (unsigned int)i,
			       (unsigned long long)l_start, (unsigned long long)l_end,
			       (unsigned long long)p_start, (unsigned long long)p_end,
			       (unsigned long long)log_blks,
			       (e->fe_flags & FIEMAP_EXTENT_LAST) ? "LAST " : "",
			       (e->fe_flags & FIEMAP_EXTENT_UNKNOWN) ? "UNKNOWN " : "",
			       (e->fe_flags & FIEMAP_EXTENT_DELALLOC) ? "DELALLOC " : "");
		}
		printf("  --------------------------------------------------------------------------------\n");
	}

	free(map);
	return 0;
}

struct dir_walk_ctx {
	struct file_frag_stat *stats;
	size_t count;
	size_t capacity;
	uint64_t total_blocks;
	uint64_t total_extents;
	uint32_t fragmented_files;
};

static void walk_dir_recursive(const char *dirpath, struct dir_walk_ctx *ctx)
{
	DIR *d = opendir(dirpath);
	struct dirent *de;

	if (!d)
		return;

	while ((de = readdir(d)) != NULL) {
		char full[1024];
		struct stat st;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		snprintf(full, sizeof(full), "%s/%s", dirpath, de->d_name);
		if (lstat(full, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			walk_dir_recursive(full, ctx);
		} else if (S_ISREG(st.st_mode) && st.st_size > 0) {
			int fd = open(full, O_RDONLY);
			struct fiemap *map = NULL;

			if (fd < 0)
				continue;

			if (query_fiemap(fd, (uint64_t)st.st_size, &map) == 0 && map->fm_mapped_extents > 0) {
				uint32_t blocks = (st.st_size + VTX_FRAG_BLOCK_SIZE - 1) / VTX_FRAG_BLOCK_SIZE;
				uint32_t disc = 0;
				__u32 i;

				for (i = 1; i < map->fm_mapped_extents; i++) {
					__u64 prev_end = map->fm_extents[i - 1].fe_physical + map->fm_extents[i - 1].fe_length;
					if (map->fm_extents[i].fe_physical != prev_end)
						disc++;
				}

				if (ctx->count == ctx->capacity) {
					size_t ncap = ctx->capacity ? ctx->capacity * 2 : 128;
					struct file_frag_stat *np = realloc(ctx->stats, ncap * sizeof(*np));
					if (np) {
						ctx->stats = np;
						ctx->capacity = ncap;
					}
				}

				if (ctx->count < ctx->capacity) {
					struct file_frag_stat *s = &ctx->stats[ctx->count++];
					snprintf(s->path, sizeof(s->path), "%s", full);
					s->size_bytes = st.st_size;
					s->blocks = blocks;
					s->extents = map->fm_mapped_extents;
					s->discontinuities = disc;
					s->frag_pct = (blocks > 1) ? ((double)disc / (double)blocks) * 100.0 : 0.0;

					ctx->total_blocks += blocks;
					ctx->total_extents += map->fm_mapped_extents;
					if (disc > 0)
						ctx->fragmented_files++;
				}
				free(map);
			}
			close(fd);
		}
	}
	closedir(d);
}

static int analyze_directory(const char *dirpath, int verbose)
{
	struct dir_walk_ctx ctx;
	double overall_rate = 0.0;
	double avg_extents = 0.0;
	size_t top_n, i;

	(void)verbose;
	memset(&ctx, 0, sizeof(ctx));
	walk_dir_recursive(dirpath, &ctx);

	if (ctx.count == 0) {
		printf("Không tìm thấy tệp tin dữ liệu hợp lệ trong thư mục '%s'\n", dirpath);
		return 0;
	}

	qsort(ctx.stats, ctx.count, sizeof(struct file_frag_stat), compare_frag_stat);

	overall_rate = ((double)ctx.fragmented_files / (double)ctx.count) * 100.0;
	avg_extents = (double)ctx.total_extents / (double)ctx.count;

	printf("==================================================================================\n");
	printf("  VTXFS — BÁO CÁO PHÂN TÍCH ĐỘ PHÂN MẢNH THƯ MỤC (NATIVE C FIEMAP)\n");
	printf("==================================================================================\n");
	printf("  Đường dẫn mục tiêu        : %s\n", dirpath);
	printf("  Tổng số tệp quét được     : %zu tệp\n", ctx.count);
	printf("  Tổng dung lượng dữ liệu   : %llu khối 4KB (%.2f MiB)\n",
	       (unsigned long long)ctx.total_blocks,
	       (double)(ctx.total_blocks * VTX_FRAG_BLOCK_SIZE) / (1024.0 * 1024.0));
	printf("  Tổng số đoạn extents      : %llu đoạn\n", (unsigned long long)ctx.total_extents);
	printf("  Số tệp bị phân mảnh       : %u / %zu (%.1f%%)\n",
	       ctx.fragmented_files, ctx.count, overall_rate);
	printf("  Trung bình extents/tệp    : %.2f\n", avg_extents);

	top_n = ctx.count < 10 ? ctx.count : 10;
	printf("\n  TOP %zu TỆP PHÂN MẢNH NHIỀU NHẤT:\n", top_n);
	printf("  --------------------------------------------------------------------------------\n");
	printf("  %-40s %10s %8s %8s %10s\n", "Tên tệp", "Dung lượng", "Extents", "Đứt gãy", "Phân mảnh");
	printf("  --------------------------------------------------------------------------------\n");

	for (i = 0; i < top_n; i++) {
		const struct file_frag_stat *r = &ctx.stats[i];
		const char *fname = strrchr(r->path, '/');
		char display_name[41];

		fname = fname ? fname + 1 : r->path;
		if (strlen(fname) > 38) {
			strncpy(display_name, fname, 35);
			strcpy(display_name + 35, "...");
		} else {
			snprintf(display_name, sizeof(display_name), "%s", fname);
		}

		printf("  %-40s %8.2fMB %8u %8u %9.1f%%\n",
		       display_name,
		       (double)r->size_bytes / (1024.0 * 1024.0),
		       r->extents,
		       r->discontinuities,
		       r->frag_pct);
	}
	printf("  --------------------------------------------------------------------------------\n");

	free(ctx.stats);
	return 0;
}

int cmd_frag(const struct cmd_struct *cmd, int argc, char **argv)
{
	const char *target = NULL;
	int verbose = 0;
	struct stat st;
	int i;

	(void)cmd;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose") || !strcmp(argv[i], "--details"))
			verbose = 1;
		else if (!target)
			target = argv[i];
	}

	if (!target) {
		fprintf(stderr, "Usage: vtxfs frag <file|dir> [--details|-v]\n");
		return 1;
	}

	if (stat(target, &st) < 0) {
		fprintf(stderr, "Lỗi: Không tìm thấy đường dẫn '%s': %s\n", target, strerror(errno));
		return 1;
	}

	if (S_ISDIR(st.st_mode))
		return analyze_directory(target, verbose);
	else if (S_ISREG(st.st_mode))
		return analyze_single_file(target, verbose);

	fprintf(stderr, "'%s' không phải là tệp hoặc thư mục hợp lệ.\n", target);
	return 1;
}

const struct cmd_struct frag_cmds[] = {
	{ "scan",    cmd_frag, NULL, "<file|dir> [--details]" },
};

const struct cmd_group frag_cmd_group = {
	.name = "frag",
	.usage = "Phân tích độ phân mảnh trực tuyến (Online FIEMAP)",
	.cmds = frag_cmds,
	.ncmds = sizeof(frag_cmds) / sizeof(frag_cmds[0]),
};
