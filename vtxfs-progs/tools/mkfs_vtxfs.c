// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * mkfs.vtxfs — Format a VTXFS v2.1 P0 filesystem image or block device
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "vtxfs_engine.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-L label] [-b total_4k_blocks] <image_or_device>\n",
		prog);
}

static void generate_uuid(__u8 uuid[16])
{
	FILE *urand = fopen("/dev/urandom", "rb");
	size_t i;

	if (urand) {
		size_t got = fread(uuid, 1, 16, urand);

		fclose(urand);
		if (got == 16) {
			uuid[6] = (__u8)((uuid[6] & 0x0FU) | 0x40U);
			uuid[8] = (__u8)((uuid[8] & 0x3FU) | 0x80U);
			return;
		}
	}
	srand((unsigned int)time(NULL));
	for (i = 0; i < 16; i++)
		uuid[i] = (__u8)(rand() & 0xFF);
	uuid[6] = (__u8)((uuid[6] & 0x0FU) | 0x40U);
	uuid[8] = (__u8)((uuid[8] & 0x3FU) | 0x80U);
}

int main(int argc, char **argv)
{
	const char *label = "vtxfs";
	const char *path = NULL;
	__u64 blocks = 0;
	int user_specified_blocks = 0;
	__u8 uuid[16];
	struct vtx_bdev bdev;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
			label = argv[++i];
		} else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			char *end = NULL;
			unsigned long long val = strtoull(argv[++i], &end, 10);

			if (!end || *end != '\0' || val < 65536ULL) {
				fprintf(stderr,
					"mkfs.vtxfs: blocks must be >= 65536 (256 MiB)\n");
				return 1;
			}
			blocks = (__u64)val;
			user_specified_blocks = 1;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
			return 1;
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		usage(argv[0]);
		return 1;
	}

	if (!user_specified_blocks) {
		struct stat st;
		if (stat(path, &st) == 0 && S_ISBLK(st.st_mode)) {
			blocks = 0; /* Auto-query full block device size */
		} else if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
			blocks = (__u64)st.st_size / 4096ULL;
		} else {
			blocks = 65536ULL; /* Default 256 MiB for new image files */
		}
	}

	generate_uuid(uuid);

	rc = vtx_file_bdev_open(&bdev, path, 2, blocks);
	if (rc != 0) {
		fprintf(stderr, "mkfs.vtxfs: failed to open '%s': %s\n",
			path, strerror(-rc));
		return 1;
	}

	rc = vtx_mkfs(&bdev, label, uuid);
	if (rc != 0) {
		fprintf(stderr, "mkfs.vtxfs: vtx_mkfs failed: %s\n",
			strerror(-rc));
		vtx_file_bdev_close(&bdev);
		return 1;
	}

	blocks = bdev.total_blocks;
	vtx_file_bdev_close(&bdev);
	printf("mkfs.vtxfs v2.1: formatted '%s' (label='%s', blocks=%llu, size=%llu MiB)\n",
	       path, label, (unsigned long long)blocks,
	       (unsigned long long)((blocks * VTXFS_BLOCK_SIZE) >> 20));
	return 0;
}
