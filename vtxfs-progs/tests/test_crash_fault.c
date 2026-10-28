// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * VTXFS v2.1 — Crash Recovery, Ordered Commit Fault Injection & Mirror Tests
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtxfs_engine.h"

static void assert_fsck_clean(struct vtx_bdev *bdev)
{
	struct vtx_fsck_report rep;
	int rc = vtx_fsck(bdev, &rep);

	if (rc != 0 || rep.errors_found != 0) {
		fprintf(stderr,
			"FSCK FAILED: rc=%d errors=%u xid=%llu\n",
			rc, rep.errors_found,
			(unsigned long long)rep.committed_xid);
	}
	assert(rc == 0);
	assert(rep.errors_found == 0);
}

int main(void)
{
	struct vtx_bdev bdev;
	struct vtx_fs *fs = NULL;
	__u8 uuid[16] = {
		0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
		0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90
	};
	__u64 base_ino = 0;
	__u8 expected_data[8192];
	__u8 read_buf[8192];
	size_t bytes_read = 0;
	size_t i;
	int rc;

	rc = vtx_ram_bdev_init(&bdev, 65536);
	assert(rc == 0);

	rc = vtx_mkfs(&bdev, "vtxfs-crash", uuid);
	assert(rc == 0);
	assert_fsck_clean(&bdev);

	/* Mount and create baseline committed state (xid = 2) */
	fs = vtx_fs_alloc();
	assert(fs != NULL);
	rc = vtx_mount(fs, &bdev, 0);
	assert(rc == 0);
	assert(fs->committed_xid == 1);

	for (i = 0; i < sizeof(expected_data); i++)
		expected_data[i] = (__u8)((i * 53 + 19) & 0xFF);

	rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL, VTXFS_INO_ROOT_DIR,
			"committed.bin", 0644, &base_ino);
	assert(rc == 0);
	rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, base_ino, 0,
		       expected_data, sizeof(expected_data));
	assert(rc == 0);
	rc = vtx_sync_checkpoint(fs);
	assert(rc == 0);
	assert(fs->committed_xid == 2);
	assert_fsck_clean(&bdev);

	/*
	 * 1. Test pre-CPD fault stages (VTX_FAULT_DATA_WRITE .. VTX_FAULT_CPD_PRIMARY):
	 *    Any failure before primary CPD FUA must leave the volume cleanly
	 *    mountable at the previous committed_xid (2) with zero fsck errors.
	 */
	{
		enum vtx_io_fault_stage stages[] = {
			VTX_FAULT_DATA_WRITE,
			VTX_FAULT_DATA_FLUSH,
			VTX_FAULT_META_WRITE,
			VTX_FAULT_VAG_WRITE,
			VTX_FAULT_META_FLUSH,
			VTX_FAULT_CPD_PRIMARY
		};
		size_t s;

		for (s = 0; s < sizeof(stages) / sizeof(stages[0]); s++) {
			__u64 temp_ino = 0;
			__u8 dirty_buf[4096];

			memset(dirty_buf, (__u8)(0xA0 + s), sizeof(dirty_buf));

			rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					VTXFS_INO_ROOT_DIR, "uncommitted.tmp",
					0644, &temp_ino);
			assert(rc == 0);
			rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				       temp_ino, 0, dirty_buf,
				       sizeof(dirty_buf));
			assert(rc == 0);

			/* Inject fault during checkpoint commit */
			bdev.fault_stage = stages[s];
			bdev.fault_triggered = 0;
			rc = vtx_sync_checkpoint(fs);
			assert(rc == -EIO);
			assert(bdev.fault_triggered == 1);

			/* Clear fault & simulate abrupt crash (destroy fs without unmount) */
			bdev.fault_stage = VTX_FAULT_NONE;
			bdev.fault_triggered = 0;
			vtx_vag_destroy(&fs->vag);
			vtx_fs_free(fs);

			/* Verify disk is still valid at xid=2 and remounts cleanly */
			assert_fsck_clean(&bdev);

			fs = vtx_fs_alloc();
			assert(fs != NULL);
			rc = vtx_mount(fs, &bdev, 0);
			assert(rc == 0);
			assert(fs->committed_xid == 2);

			/* Verify uncommitted file does not exist */
			rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					VTXFS_INO_ROOT_DIR, "uncommitted.tmp",
					&temp_ino, NULL);
			assert(rc == -ENOENT);

			/* Verify committed.bin is 100% intact */
			memset(read_buf, 0, sizeof(read_buf));
			rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				      base_ino, 0, read_buf,
				      sizeof(read_buf), &bytes_read);
			assert(rc == 0 && bytes_read == sizeof(expected_data));
			assert(memcmp(read_buf, expected_data,
				      sizeof(expected_data)) == 0);
		}
	}

	/*
	 * 2. Test VTX_FAULT_CPD_MIRROR:
	 *    Primary CPD FUA succeeds (xid=3 is durable in primary ring),
	 *    mirror CPD write fails. Remount must recover xid=3 from primary!
	 */
	{
		__u64 surv_ino = 0;

		rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				VTXFS_INO_ROOT_DIR, "survived.txt",
				0644, &surv_ino);
		assert(rc == 0);
		rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, surv_ino,
			       0, "durable-primary", 15);
		assert(rc == 0);

		bdev.fault_stage = VTX_FAULT_CPD_MIRROR;
		bdev.fault_triggered = 0;
		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert(bdev.fault_triggered == 1);

		bdev.fault_stage = VTX_FAULT_NONE;
		bdev.fault_triggered = 0;
		vtx_vag_destroy(&fs->vag);
		vtx_fs_free(fs);

		assert_fsck_clean(&bdev);

		fs = vtx_fs_alloc();
		assert(fs != NULL);
		rc = vtx_mount(fs, &bdev, 0);
		assert(rc == 0);
		assert(fs->committed_xid == 3);

		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, surv_ino,
			      0, read_buf, 15, &bytes_read);
		assert(rc == 0 && bytes_read == 15);
		assert(memcmp(read_buf, "durable-primary", 15) == 0);
	}

	/* Commit one more clean transaction (xid = 4) */
	rc = vtx_utimes(fs, VTXFS_OID_FIRST_USER_SUBVOL, base_ino, 777, 888);
	assert(rc == 0);
	rc = vtx_unmount(fs); /* commits xid = 4 */
	assert(rc == 0);
	vtx_fs_free(fs);
	assert_fsck_clean(&bdev);

	/*
	 * 3. Corrupt primary Superblock (block 16) -> verify mount & fsck
	 *    fall back to 64 MiB mirror Superblock (block 16384)!
	 */
	{
		__u8 zero_blk[VTXFS_BLOCK_SIZE];
		__u8 saved_sb[VTXFS_BLOCK_SIZE];

		memset(zero_blk, 0xFF, sizeof(zero_blk));
		rc = bdev.ops->read_blocks(&bdev, VTXFS_IP_BOOT_PBLK,
					   1, saved_sb);
		assert(rc == 0);
		rc = bdev.ops->write_blocks(&bdev, VTXFS_IP_BOOT_PBLK,
					    1, zero_blk);
		assert(rc == 0);

		/* Both fsck and mount should succeed via VTXFS_BOOT_MIRROR0_PBLK */
		assert_fsck_clean(&bdev);

		fs = vtx_fs_alloc();
		assert(fs != NULL);
		rc = vtx_mount(fs, &bdev, 1);
		assert(rc == 0);
		assert(fs->committed_xid == 4);
		rc = vtx_unmount(fs);
		assert(rc == 0);
		vtx_fs_free(fs);

		/* Restore primary SB */
		rc = bdev.ops->write_blocks(&bdev, VTXFS_IP_BOOT_PBLK,
					    1, saved_sb);
		assert(rc == 0);
	}

	/*
	 * 4. Corrupt latest primary CPD slot (xid = 4 -> slot = 4 % 8 = 4) ->
	 *    verify mount & fsck recover xid = 4 from 64 MiB mirror CPD slot!
	 *    Then also corrupt mirror CPD slot 4 -> verify mount rolls back
	 *    to previous valid checkpoint xid = 3 (slot 3)!
	 */
	{
		__u64 slot4 = VTXFS_CPD_SLOT_FOR_XID(4ULL);
		__u64 prim_cpd4 = VTXFS_CPD_RING_PBLK + slot4;
		__u64 mirr_cpd4 = VTXFS_BOOT_MIRROR0_PBLK + prim_cpd4;
		__u8 corrupt_blk[VTXFS_BLOCK_SIZE];

		memset(corrupt_blk, 0x55, sizeof(corrupt_blk));

		/* Corrupt primary CPD slot 4 only */
		rc = bdev.ops->write_blocks(&bdev, prim_cpd4, 1, corrupt_blk);
		assert(rc == 0);

		assert_fsck_clean(&bdev);
		fs = vtx_fs_alloc();
		assert(fs != NULL);
		rc = vtx_mount(fs, &bdev, 1);
		assert(rc == 0);
		assert(fs->committed_xid == 4);
		rc = vtx_unmount(fs);
		assert(rc == 0);
		vtx_fs_free(fs);

		/* Now corrupt mirror CPD slot 4 as well -> rollback to xid = 3! */
		rc = bdev.ops->write_blocks(&bdev, mirr_cpd4, 1, corrupt_blk);
		assert(rc == 0);

		assert_fsck_clean(&bdev);
		fs = vtx_fs_alloc();
		assert(fs != NULL);
		rc = vtx_mount(fs, &bdev, 0);
		assert(rc == 0);
		assert(fs->committed_xid == 3);
		rc = vtx_unmount(fs);
		assert(rc == 0);
		vtx_fs_free(fs);
		assert_fsck_clean(&bdev);
	}

	vtx_ram_bdev_destroy(&bdev);
	printf("PASS: Crash recovery, fault injection (all 7 stages), SB/CPD mirror fallback & ring rollback\n");
	return 0;
}
