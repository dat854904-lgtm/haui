// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * fsck.vtxfs — Offline Integrity & Cross-Tree Reference/Bitmap Verifier
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-y|--repair] [--dry-run] [-f] [-v] [-q] [-a] [-p] [-n] <image_or_device>\n", prog);
	fprintf(stderr, "\nVTXFS Offline File System Check & Repair Utility\n\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -y, --repair  Repair detected corruptions (bitmaps, refcounts, CPD mirrors)\n");
	fprintf(stderr, "  --dry-run     Report proposed repairs without modifying disk\n");
	fprintf(stderr, "  -f            Force check even if filesystem appears clean\n");
	fprintf(stderr, "  -v            Verbose output\n");
	fprintf(stderr, "  -q            Quiet mode (suppress report output)\n");
	fprintf(stderr, "  -a, -p, -n    Fsck(8) compatibility flags\n");
	fprintf(stderr, "  -h, --help    Display this help message and exit\n");
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	int quiet = 0;
	int verbose = 0;
	int repair = 0;
	int dry_run = 0;
	int force = 0;
	struct vtx_bdev bdev;
	struct vtx_fsck_report rep;
	int i, rc, rw_mode;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-q") == 0) {
			quiet = 1;
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
		} else if (strcmp(argv[i], "-f") == 0) {
			force = 1;
		} else if (strcmp(argv[i], "-y") == 0 ||
			   strcmp(argv[i], "--repair") == 0) {
			repair = 1;
		} else if (strcmp(argv[i], "--dry-run") == 0) {
			dry_run = 1;
		} else if (strcmp(argv[i], "-a") == 0 ||
			   strcmp(argv[i], "-p") == 0 ||
			   strcmp(argv[i], "-n") == 0) {
			/* Systemd/fsck(8) compatibility. */
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
			return 8;
		} else {
			if (path) {
				usage(argv[0]);
				return 8;
			}
			path = argv[i];
		}
	}

	(void)force;
	(void)verbose;

	if (!path) {
		usage(argv[0]);
		return 8;
	}

	rw_mode = (repair && !dry_run) ? 1 : 0;
	if (rw_mode && is_path_mounted(path)) {
		fprintf(stderr, "fsck.vtxfs: LỖI: '%s' hiện đang được mount! "
		                "Không thể sửa chữa hệ thống tệp đang hoạt động (vui lòng unmount trước).\n",
		        path);
		return 8;
	}
	rc = vtx_file_bdev_open(&bdev, path, rw_mode, 0);
	if (rc != 0) {
		fprintf(stderr, "fsck.vtxfs: failed to open '%s': %s\n",
			path, strerror(-rc));
		return 8;
	}

	memset(&rep, 0, sizeof(rep));
	if (repair || dry_run) {
		rc = vtx_fsck_repair(&bdev, dry_run ? 1 : 0, &rep);
		if (rc != 0)
			fprintf(stderr, "fsck.vtxfs: vtx_fsck_repair rc=%d\n", rc);
	} else {
		rc = vtx_fsck(&bdev, &rep);
	}
	vtx_file_bdev_close(&bdev);

	if (!quiet) {
		printf("fsck.vtxfs v2.1 report for '%s':\n", path);
		printf("  committed_xid           : %llu\n",
		       (unsigned long long)rep.committed_xid);
		printf("  super_generation        : %llu\n",
		       (unsigned long long)rep.super_generation);
		printf("  total_vags              : %u\n", rep.total_vags);
		printf("  subvolumes_checked      : %u\n",
		       rep.subvolumes_checked);
		printf("  btree_nodes_verified    : %llu\n",
		       (unsigned long long)rep.btree_nodes_verified);
		printf("  data_extents_verified   : %llu\n",
		       (unsigned long long)rep.data_extents_verified);
		printf("  ref_items_verified      : %llu\n",
		       (unsigned long long)rep.ref_items_verified);
		printf("  sfq_entries_verified    : %llu\n",
		       (unsigned long long)rep.sfq_entries_verified);
		printf("  reaper_jobs_checked     : %llu\n",
		       (unsigned long long)rep.reaper_jobs_checked);
		printf("  allocated_blocks_counted: %llu\n",
		       (unsigned long long)rep.allocated_blocks_counted);
		printf("  errors_found            : %u\n", rep.errors_found);
		printf("  repairs_planned         : %u\n", rep.repairs_planned);
		printf("  repairs_performed       : %u\n", rep.repairs_performed);
		printf("  status                  : %s\n",
		       (rc == 0 && rep.errors_found == 0) ? "CLEAN" :
		       (rc == 0 && rep.repairs_performed > 0) ? "REPAIRED" : "CORRUPT");
	}

	/* fsck(8) exit codes:
	 * 0: Clean (no errors)
	 * 1: Errors were found and corrected successfully
	 * 4: Uncorrected errors remain
	 * 8: Operational / usage error
	 */
	if (repair && !dry_run && rep.repairs_performed > 0 && rc == 0) {
		return 1;
	}
	if (rep.errors_found || rc == -EUCLEAN || rc == -EBADMSG ||
	    rc == -EINVAL) {
		return 4;
	}
	return rc == 0 ? 0 : 8;
}
