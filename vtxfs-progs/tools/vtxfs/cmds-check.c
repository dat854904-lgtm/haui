// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-check.c — Unified CLI integration for VTXFS File System Consistency Check & Repair
 * (Equivalent to btrfs check / fsck.vtxfs)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "commands.h"
#include "vtxfs_engine.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int is_path_mounted(const char *dev_path)
{
	char real_target[PATH_MAX];
	if (!realpath(dev_path, real_target)) {
		strncpy(real_target, dev_path, sizeof(real_target) - 1);
		real_target[sizeof(real_target) - 1] = '\0';
	}

	FILE *f_mnt = fopen("/proc/mounts", "r");
	if (!f_mnt)
		return 0;

	char line[512];
	int mounted = 0;
	while (fgets(line, sizeof(line), f_mnt)) {
		char m_dev[256], m_pt[256];
		if (sscanf(line, "%255s %255s", m_dev, m_pt) >= 2) {
			char real_mdev[PATH_MAX];
			if (realpath(m_dev, real_mdev)) {
				if (strcmp(real_mdev, real_target) == 0) {
					mounted = 1;
					break;
				}
			} else if (strcmp(m_dev, real_target) == 0) {
				mounted = 1;
				break;
			}
		}
	}
	fclose(f_mnt);
	return mounted;
}

static int check_use_color(void)
{
	static int checked = -1;
	if (checked == -1)
		checked = isatty(STDOUT_FILENO);
	return checked == 1;
}

#define C_RESET   (check_use_color() ? "\033[0m" : "")
#define C_BOLD    (check_use_color() ? "\033[1m" : "")
#define C_RED     (check_use_color() ? "\033[31m" : "")
#define C_GREEN   (check_use_color() ? "\033[32m" : "")
#define C_YELLOW  (check_use_color() ? "\033[33m" : "")
#define C_CYAN    (check_use_color() ? "\033[36m" : "")

static void check_usage(void)
{
	fprintf(stderr,
		"Cách dùng: vtxfs check [tùy_chọn] <thiết_bị_hoặc_image>\n\n"
		"Kiểm tra tính toàn vẹn và sửa chữa hệ thống tệp VTXFS v2.1\n\n"
		"Tùy chọn:\n"
		"  -y, --repair  Sửa chữa các lỗi phát hiện được (bitmaps, refcounts, CPD mirror)\n"
		"  --dry-run     Báo cáo kế hoạch sửa chữa mà không ghi đè lên đĩa\n"
		"  -f, --force   Bắt buộc kiểm tra ngay cả khi filesystem có cờ sạch\n"
		"  -v, --verbose Hiển thị chi tiết quá trình quét\n"
		"  -q, --quiet   Chế độ im lặng, chỉ trả về mã lỗi\n"
		"  -h, --help    Hiển thị trợ giúp này\n");
}

int cmd_check(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	const char *path = NULL;
	int quiet = 0;
	int verbose = 0;
	int repair = 0;
	int dry_run = 0;
	int force = 0;
	struct vtx_bdev bdev;
	struct vtx_fsck_report rep;
	int i, rc, rw_mode;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
			quiet = 1;
		} else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			verbose = 1;
		} else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0) {
			force = 1;
		} else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--repair") == 0) {
			repair = 1;
		} else if (strcmp(argv[i], "--dry-run") == 0) {
			dry_run = 1;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			check_usage();
			return 0;
		} else if (argv[i][0] == '-') {
			check_usage();
			return 8;
		} else {
			if (path) {
				check_usage();
				return 8;
			}
			path = argv[i];
		}
	}

	(void)force;
	(void)verbose;

	if (!path) {
		check_usage();
		return 8;
	}

	rw_mode = (repair && !dry_run) ? 1 : 0;
	if (rw_mode && is_path_mounted(path)) {
		fprintf(stderr, "%svtxfs check: LỖI: '%s' hiện đang được mount! "
		                "Không thể sửa chữa hệ thống tệp đang hoạt động (vui lòng unmount trước).%s\n",
		        C_RED, path, C_RESET);
		return 8;
	}
	rc = vtx_file_bdev_open(&bdev, path, rw_mode, 0);
	if (rc != 0) {
		fprintf(stderr, "%svtxfs check: Không thể mở '%s': %s%s\n",
			C_RED, path, strerror(-rc), C_RESET);
		return 8;
	}

	memset(&rep, 0, sizeof(rep));
	if (repair || dry_run) {
		rc = vtx_fsck_repair(&bdev, dry_run ? 1 : 0, &rep);
	} else {
		rc = vtx_fsck(&bdev, &rep);
	}
	vtx_file_bdev_close(&bdev);

	if (!quiet) {
		printf("==================================================================================\n");
		printf("  %sVTXFS v2.1 — BÁO CÁO KIỂM TRA TÍNH TOÀN VẸN (FSCK REPORT)%s\n", C_BOLD, C_RESET);
		printf("  Đối tượng: %s%s%s | Chế độ: %s%s%s\n",
		       C_CYAN, path, C_RESET,
		       repair ? (dry_run ? "DRY-RUN (Mô phỏng sửa)" : "REPAIR (Sửa chữa)") : "VERIFY-ONLY (Chỉ kiểm tra)",
		       C_RESET, "");
		printf("==================================================================================\n");
		printf("  Thế hệ Checkpoint XID   : %llu\n", (unsigned long long)rep.committed_xid);
		printf("  Super Generation        : %llu\n", (unsigned long long)rep.super_generation);
		printf("  Tổng số VAG trên đĩa    : %u VAGs\n", rep.total_vags);
		printf("  Số subvolumes kiểm tra  : %u subvolume(s)\n", rep.subvolumes_checked);
		printf("  Số node B-Tree xác minh : %llu nodes\n", (unsigned long long)rep.btree_nodes_verified);
		printf("  Số extents dữ liệu      : %llu extents\n", (unsigned long long)rep.data_extents_verified);
		printf("  Số bản ghi Ref Tree     : %llu items\n", (unsigned long long)rep.ref_items_verified);
		printf("  Số mục hàng đợi SFQ     : %llu entries\n", (unsigned long long)rep.sfq_entries_verified);
		printf("  Số tác vụ Reaper        : %llu jobs\n", (unsigned long long)rep.reaper_jobs_checked);
		printf("  Tổng số block cấp phát  : %llu blocks 4KB\n", (unsigned long long)rep.allocated_blocks_counted);
		printf("  ------------------------------------------------------------------------------\n");
		printf("  Số lỗi phát hiện        : %s%u lỗi%s\n",
		       rep.errors_found ? C_RED : C_GREEN, rep.errors_found, C_RESET);
		printf("  Số lỗi dự kiến sửa      : %u\n", rep.repairs_planned);
		printf("  Số lỗi đã khắc phục     : %s%u%s\n",
		       rep.repairs_performed ? C_GREEN : "", rep.repairs_performed, C_RESET);

		const char *status_str = (rc == 0 && rep.errors_found == 0) ? "CLEAN (HOÀN TOÀN SẠCH)" :
					 (rc == 0 && rep.repairs_performed > 0) ? "REPAIRED (ĐÃ KHẮC PHỤC THÀNH CÔNG)" :
					 "CORRUPT (PHÁT HIỆN HƯ HỎNG)";
		const char *color_str = (rc == 0 && rep.errors_found == 0) ? C_GREEN :
					(rc == 0 && rep.repairs_performed > 0) ? C_GREEN : C_RED;
		printf("  TRẠNG THÁI CUỐI CÙNG    : %s%s%s%s\n", C_BOLD, color_str, status_str, C_RESET);
		printf("==================================================================================\n");
	}

	/* Fsck exit code contract:
	 * 0: Clean
	 * 1: Repaired successfully
	 * 4: Uncorrected errors remain
	 * 8: Usage/operational error
	 */
	if (repair && !dry_run && rep.repairs_performed > 0 && rc == 0)
		return 1;
	if (rep.errors_found || rc == -EUCLEAN || rc == -EBADMSG || rc == -EINVAL)
		return 4;
	return rc == 0 ? 0 : 8;
}
