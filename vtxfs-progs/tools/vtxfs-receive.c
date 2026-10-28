// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs-receive — Công cụ phục hồi stream nhị phân độc lập của VTXFS (VTXSEND1).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vtxfs_stream.h"

static void print_usage(const char *prog)
{
	fprintf(stderr, "Cách dùng: %s <thư_mục_đích> [-f <tệp_stream>] [-v]\n", prog);
	fprintf(stderr, "Tùy chọn:\n");
	fprintf(stderr, "  -f <tệp>       Đọc dòng truyền phát từ tệp thay vì stdin\n");
	fprintf(stderr, "  -v, --verbose  Hiển thị chi tiết từng tệp được phục hồi\n");
	fprintf(stderr, "  -h, --help     Hiển thị trợ giúp này\n");
}

int main(int argc, char **argv)
{
	struct vtx_receive_options opts;
	struct vtx_stream_stats stats;
	const char *target = NULL;
	const char *in_file = NULL;
	int verbose = 0;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argv[0]);
			return 0;
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
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
		print_usage(argv[0]);
		return 1;
	}

	if (!in_file && isatty(STDIN_FILENO)) {
		fprintf(stderr, "Lỗi: Đầu vào tiêu chuẩn (stdin) là terminal. Hãy chỉ định -f <tệp> hoặc pipe (|).\n");
		return 1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.target_path = target;
	opts.in_file = in_file;
	opts.verbose = verbose;

	rc = vtx_stream_receive(&opts, &stats);
	if (rc) {
		fprintf(stderr, "vtxfs-receive thất bại: %s (mã %d)\n", strerror(-rc), rc);
		return 1;
	}

	double mib = (double)stats.total_bytes / (1024.0 * 1024.0);
	double mbps = stats.elapsed_sec > 0 ? (mib / stats.elapsed_sec) : 0.0;

	fprintf(stderr, "Hoàn tất vtxfs-receive: %.2f MiB (%llu bytes), %llu files, %llu dirs, %.3fs (%.2f MiB/s), CRC32c: 0x%08X [OK]\n",
		mib, (unsigned long long)stats.total_bytes,
		(unsigned long long)stats.total_files,
		(unsigned long long)stats.total_dirs,
		stats.elapsed_sec, mbps, stats.stream_crc);

	return 0;
}
