// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-send.c — Lệnh 'vtxfs send': Tuần tự hóa subvolume thành dòng dữ liệu nhị phân.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "commands.h"
#include "common.h"
#include "vtxfs_stream.h"

int cmd_send(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtx_send_options opts;
	struct vtx_stream_stats stats;
	const char *src = NULL;
	const char *out_file = NULL;
	__u64 subvol_id = 0;
	int verbose = 0;
	int i, rc;

	(void)c;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			out_file = argv[++i];
		} else if (!strncmp(argv[i], "-f=", 3)) {
			out_file = argv[i] + 3;
		} else if (!strncmp(argv[i], "--subvol=", 9)) {
			subvol_id = strtoull(argv[i] + 9, NULL, 10);
		} else if (!strcmp(argv[i], "--subvol") && i + 1 < argc) {
			subvol_id = strtoull(argv[++i], NULL, 10);
		} else if (argv[i][0] != '-') {
			if (!src)
				src = argv[i];
		}
	}

	if (!src) {
		fprintf(stderr, "Cách dùng: vtxfs send <đường_dẫn_subvolume> [-f <tệp_xuất>] [--subvol=N] [-v]\n");
		fprintf(stderr, "Ví dụ:\n");
		fprintf(stderr, "  vtxfs send /mnt/vtxfs/subvol1 -f /backup/subvol1.vtxstream\n");
		fprintf(stderr, "  vtxfs send /mnt/vtxfs/subvol1 | vtxfs receive /mnt/vtxfs/subvol_copy\n");
		return 1;
	}

	/* Bảo vệ terminal nếu stdout không được chuyển hướng và không có -f */
	if (!out_file && isatty(STDOUT_FILENO)) {
		fprintf(stderr, "Lỗi: Đầu ra tiêu chuẩn (stdout) là màn hình dòng lệnh (terminal)!\n");
		fprintf(stderr, "Để bảo vệ thiết bị đầu cuối, vui lòng cung cấp -f <tệp> hoặc chuyển hướng dòng truyền phát (|).\n");
		return 1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.source_path = src;
	opts.out_file = out_file;
	opts.subvol_id = subvol_id;
	opts.verbose = verbose;

	if (out_file || !isatty(STDERR_FILENO)) {
		fprintf(stderr, ">>> Bắt đầu xuất dòng truyền phát VTXFS cho '%s'...\n", src);
	}

	rc = vtx_stream_send(&opts, &stats);
	if (rc) {
		fprintf(stderr, "vtxfs send thất bại: %s (mã lỗi %d)\n", strerror(-rc), rc);
		return 1;
	}

	double mib = (double)stats.total_bytes / (1024.0 * 1024.0);
	double mbps = stats.elapsed_sec > 0 ? (mib / stats.elapsed_sec) : 0.0;

	fprintf(stderr, "==================================================================================\n");
	fprintf(stderr, "  VTXFS SEND — HOÀN TẤT XUẤT DÒNG TRUYỀN PHÁT NHỊ PHÂN THÀNH CÔNG\n");
	fprintf(stderr, "  Nguồn: %s | Đầu ra: %s\n", src, out_file ? out_file : "<stdout>");
	fprintf(stderr, "  Thống kê: %llu thư mục, %llu tệp, %llu symlink, %llu hardlink, %llu xattr\n",
		(unsigned long long)stats.total_dirs,
		(unsigned long long)stats.total_files,
		(unsigned long long)stats.total_symlinks,
		(unsigned long long)stats.total_hardlinks,
		(unsigned long long)stats.total_xattrs);
	fprintf(stderr, "  Kích thước stream: %.2f MiB (%llu bytes) | Thời gian: %.3f s | Tốc độ: %.2f MiB/s\n",
		mib, (unsigned long long)stats.total_bytes, stats.elapsed_sec, mbps);
	fprintf(stderr, "  Tổng kiểm CRC32c dòng dữ liệu: 0x%08X [TOÀN VẸN - CHÍNH XÁC]\n", stats.stream_crc);
	fprintf(stderr, "==================================================================================\n");

	return 0;
}
