// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs-send — Công cụ xuất stream nhị phân độc lập của VTXFS (VTXSEND1).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vtxfs_stream.h"

static void print_usage(const char *prog)
{
	fprintf(stderr, "Cách dùng: %s <đường_dẫn_subvolume> [-f <tệp_xuất>] [-s <subvol_id>] [-v]\n", prog);
	fprintf(stderr, "Tùy chọn:\n");
	fprintf(stderr, "  -f <tệp>       Ghi dòng truyền phát ra tệp thay vì stdout\n");
	fprintf(stderr, "  -s <id>        Chỉ định ID subvolume nếu là ảnh/thiết bị\n");
	fprintf(stderr, "  -v, --verbose  Hiển thị chi tiết từng tệp được tuần tự hóa\n");
	fprintf(stderr, "  -h, --help     Hiển thị trợ giúp này\n");
}

int main(int argc, char **argv)
{
	struct vtx_send_options opts;
	struct vtx_stream_stats stats;
	const char *src = NULL;
	const char *out_file = NULL;
	__u64 subvol_id = 0;
	int verbose = 0;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argv[0]);
			return 0;
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			out_file = argv[++i];
		} else if (!strncmp(argv[i], "-f=", 3)) {
			out_file = argv[i] + 3;
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			subvol_id = strtoull(argv[++i], NULL, 10);
		} else if (argv[i][0] != '-') {
			if (!src)
				src = argv[i];
		}
	}

	if (!src) {
		print_usage(argv[0]);
		return 1;
	}

	if (!out_file && isatty(STDOUT_FILENO)) {
		fprintf(stderr, "Lỗi: Đầu ra tiêu chuẩn (stdout) là terminal. Hãy chỉ định -f <tệp> hoặc pipe (|).\n");
		return 1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.source_path = src;
	opts.out_file = out_file;
	opts.subvol_id = subvol_id;
	opts.verbose = verbose;

	rc = vtx_stream_send(&opts, &stats);
	if (rc) {
		fprintf(stderr, "vtxfs-send thất bại: %s (mã %d)\n", strerror(-rc), rc);
		return 1;
	}

	double mib = (double)stats.total_bytes / (1024.0 * 1024.0);
	double mbps = stats.elapsed_sec > 0 ? (mib / stats.elapsed_sec) : 0.0;

	fprintf(stderr, "Hoàn tất vtxfs-send: %.2f MiB (%llu bytes), %llu files, %llu dirs, %.3fs (%.2f MiB/s), CRC32c: 0x%08X\n",
		mib, (unsigned long long)stats.total_bytes,
		(unsigned long long)stats.total_files,
		(unsigned long long)stats.total_dirs,
		stats.elapsed_sec, mbps, stats.stream_crc);

	return 0;
}
