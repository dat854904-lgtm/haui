// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * vtxfs-ioctl-test — exercises GET_ABI / GET_CAPABILITIES / START_SYNC / WAIT_SYNC
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../uapi/vtxfs.h"

int main(int argc, char **argv)
{
	struct vtx_ioctl_abi abi;
	struct vtx_ioc_capabilities caps;
	struct vtx_ioc_sync sync_args;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <mounted-dir-or-file>\n", argv[0]);
		return 1;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	memset(&abi, 0, sizeof(abi));
	abi.struct_size = sizeof(abi);
	if (ioctl(fd, VTXFS_IOC_GET_ABI, &abi) != 0) {
		perror("VTXFS_IOC_GET_ABI");
		close(fd);
		return 1;
	}
	printf("ABI: major=%u minor=%u uapi=%u incompat=0x%llx/0x%llx\n",
	       abi.major, abi.minor, abi.uapi_version,
	       (unsigned long long)abi.supported_incompat,
	       (unsigned long long)abi.active_incompat);
	if (abi.major != VTXFS_VERSION_MAJOR ||
	    abi.minor != VTXFS_VERSION_MINOR) {
		close(fd);
		return 1;
	}

	memset(&caps, 0, sizeof(caps));
	caps.struct_size = sizeof(caps);
	if (ioctl(fd, VTXFS_IOC_GET_CAPABILITIES, &caps) != 0) {
		perror("VTXFS_IOC_GET_CAPABILITIES");
		close(fd);
		return 1;
	}
	printf("CAPS: 0x%llx\n", (unsigned long long)caps.capabilities);
	if (!(caps.capabilities & VTXFS_CAP_P0_SINGLEDEV_RW)) {
		close(fd);
		return 1;
	}

	memset(&sync_args, 0, sizeof(sync_args));
	sync_args.struct_size = sizeof(sync_args);
	if (ioctl(fd, VTX_IOC_START_SYNC, &sync_args) != 0) {
		perror("VTX_IOC_START_SYNC");
		close(fd);
		return 1;
	}
	printf("START_SYNC -> xid=%llu\n", (unsigned long long)sync_args.xid);

	memset(&sync_args, 0, sizeof(sync_args));
	sync_args.struct_size = sizeof(sync_args);
	if (ioctl(fd, VTX_IOC_WAIT_SYNC, &sync_args) != 0) {
		perror("VTX_IOC_WAIT_SYNC");
		close(fd);
		return 1;
	}
	printf("WAIT_SYNC  -> xid=%llu\n", (unsigned long long)sync_args.xid);

	{
		struct vtx_ioc_scrub_args scrub_args;

		memset(&scrub_args, 0, sizeof(scrub_args));
		scrub_args.struct_size = sizeof(scrub_args);
		scrub_args.flags = VTX_SCRUB_VERIFY_ONLY;
		if (ioctl(fd, VTX_IOC_SCRUB, &scrub_args) == 0) {
			printf("SCRUB      -> scrubbed=%llu errs=%llu\n",
			       (unsigned long long)scrub_args.data_blocks_scrubbed,
			       (unsigned long long)scrub_args.checksum_errors);
		}
	}

	close(fd);

	printf("PASS: VTXFS ioctl smoke test\n");
	return 0;
}
