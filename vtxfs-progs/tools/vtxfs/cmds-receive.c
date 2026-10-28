// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-receive.c — Lệnh 'vtxfs receive': Giải mã dòng dữ liệu nhị phân và phục hồi subvolume.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "commands.h"
#include "common.h"
#include "vtxfs_stream.h"

int cmd_receive(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtx_receive_options opts;
	struct vtx_stream_stats stats;
	const char *target = NULL;
	const char *in_file = NULL;
	int verbose = 0;
	int i, rc;

	(void)c;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			in_file = argv[++i];
		} else if (!strncmp(argv[i], "-f=", 3)) {
			in_file = argv[i] + 3;
		} else if (argv[i][0] != '-') {
			if (!target)
				target = argv[i];
		}
	}

	if (!target) {
		fprintf(stderr, "Cách dùng: vtxfs receive <thư_mục_đích> [-f <tệp_stream>] [-v]\n");
		fprintf(stderr, "Ví dụ:\n");
		fprintf(stderr, "  vtxfs receive /mnt/vtxfs/subvol_restored -f /backup/subvol1.vtxstream\n");
		fprintf(stderr, "  vtxfs send /mnt/vtxfs/subvol1 | vtxfs receive /mnt/vtxfs/subvol_restored\n");
		return 1;
	}

	/* Bảo vệ stdin nếu là terminal và không có -f */
	if (!in_file && isatty(STDIN_FILENO)) {
		fprintf(stderr, "Lỗi: Đầu vào tiêu chuẩn (stdin) là bàn phím dòng lệnh (terminal)!\n");
		fprintf(stderr, "Vui lòng chỉ định tệp nguồn (-f <tệp>) hoặc chuyển hướng pipe (|) từ lệnh 'vtxfs send'.\n");
		return 1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.target_path = target;
	opts.in_file = in_file;
	opts.verbose = verbose;

	fprintf(stderr, ">>> Bắt đầu nhận dòng truyền phát VTXFS vào '%s'...\n", target);

	rc = vtx_stream_receive(&opts, &stats);
	if (rc) {
		fprintf(stderr, "vtxfs receive thất bại: %s (mã lỗi %d)\n", strerror(-rc), rc);
		return 1;
	}

	double mib = (double)stats.total_bytes / (1024.0 * 1024.0);
	double mbps = stats.elapsed_sec > 0 ? (mib / stats.elapsed_sec) : 0.0;

	fprintf(stderr, "==================================================================================\n");
	fprintf(stderr, "  VTXFS RECEIVE — PHỤC HỒI DÒNG TRUYỀN PHÁT NHỊ PHÂN THÀNH CÔNG HOÀN TOÀN\n");
	fprintf(stderr, "  Nguồn: %s | Đích phục hồi: %s\n", in_file ? in_file : "<stdin>", target);
	fprintf(stderr, "  Đã phục hồi: %llu thư mục, %llu tệp, %llu symlink, %llu hardlink, %llu xattr\n",
		(unsigned long long)stats.total_dirs,
		(unsigned long long)stats.total_files,
		(unsigned long long)stats.total_symlinks,
		(unsigned long long)stats.total_hardlinks,
		(unsigned long long)stats.total_xattrs);
	fprintf(stderr, "  Dữ liệu đã nhận: %.2f MiB (%llu bytes) | Thời gian: %.3f s | Tốc độ: %.2f MiB/s\n",
		mib, (unsigned long long)stats.total_bytes, stats.elapsed_sec, mbps);
	fprintf(stderr, "  Tổng kiểm CRC32c dòng dữ liệu: 0x%08X [TOÀN VẸN - KHỚP 100%%]\n", stats.stream_crc);
	fprintf(stderr, "==================================================================================\n");

	return 0;
}
