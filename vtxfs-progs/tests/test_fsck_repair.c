// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * VTXFS v2.1 — Offline Fsck Repair & Recovery Test
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vtxfs_engine.h"

static void corrupt_vag_bitmap_on_disk(struct vtx_bdev *bdev,
				       const struct vtx_super_block *sb,
				       __u64 xid)
{
	__u64 slot = VTXFS_CPD_SLOT_FOR_XID(xid);
	__u64 prim_base = VTXFS_IP_BOOT_PBLK + slot * VTX_IP_SLOT_STRIDE;
	__u64 mirr_base = VTXFS_BOOT_MIRROR0_PBLK + prim_base;
	struct vtx_vag_dir_root root;
	struct vtx_vag_desc *descs = (struct vtx_vag_desc *)calloc(16, VTXFS_BLOCK_SIZE);
	__u8 bm[VTXFS_BLOCK_SIZE];
	int rc;

	assert(descs != NULL);

	/* Read root, descs, and bitmap 0 */
	rc = bdev->ops->read_blocks(bdev, prim_base, 1, &root);
	assert(rc == 0);
	rc = bdev->ops->read_blocks(bdev, prim_base + 1ULL, 16, descs);
	assert(rc == 0);
	rc = bdev->ops->read_blocks(bdev, prim_base + 17ULL, 1, bm);
	assert(rc == 0);

	/* Flip free bits to allocated (bit 1000 & 1001 = byte 125, bits 0 & 1) */
	bm[125] ^= 0x03;

	/* Recalculate CRC32C and root checksum */
	descs[0].bitmap_crc32c = vtx_le32(vtx_crc32c(bm, VTXFS_BLOCK_SIZE));
	root.entries[0].leaf_crc32c = vtx_le32(vtx_crc32c(descs, 16 * VTXFS_BLOCK_SIZE));
	vtx_vag_root_checksum_compute(&root, sb->hdr.metadata_uuid);

	/* Write back to mirror, then primary */
	rc = bdev->ops->write_blocks(bdev, mirr_base + 17ULL, 1, bm);
	assert(rc == 0);
	rc = bdev->ops->write_blocks(bdev, mirr_base + 1ULL, 16, descs);
	assert(rc == 0);
	rc = bdev->ops->write_blocks(bdev, mirr_base, 1, &root);
	assert(rc == 0);

	rc = bdev->ops->write_blocks(bdev, prim_base + 17ULL, 1, bm);
	assert(rc == 0);
	rc = bdev->ops->write_blocks(bdev, prim_base + 1ULL, 16, descs);
	assert(rc == 0);
	rc = bdev->ops->write_blocks(bdev, prim_base, 1, &root);
	assert(rc == 0);
	rc = bdev->ops->flush(bdev);
	assert(rc == 0);
	free(descs);
}

int main(int argc, char **argv)
{
	const char *fsck_bin = (argc > 1) ? argv[1] : "./tools/fsck.vtxfs";
	const char *tmpdir = getenv("TMPDIR");
	char img_path[512];
	struct vtx_bdev bdev;
	struct vtx_fs *fs;
	const __u8 uuid[16] = {
		0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
	};
	struct vtx_super_block saved_sb;
	__u64 subvol_id = 0, dir_ino = 0, file_ino = 0;
	__u8 payload[16384];
	char cmd[1024];
	int rc, status, exit_code;
	__u64 latest_xid;
	size_t i;

	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	snprintf(img_path, sizeof(img_path), "%s/vtxfs_repair_test_%d.img",
		 tmpdir, (int)getpid());

	printf("--- Running VTXFS FSCK Repair Test Suite ---\n");

	/* 1. Format image (65536 blocks = 256 MiB) */
	rc = vtx_file_bdev_open(&bdev, img_path, 2, 65536);
	assert(rc == 0);
	rc = vtx_mkfs(&bdev, "vtxfs-repair", uuid);
	assert(rc == 0);

	/* 2. Mount and populate subvolumes, directories, files, and extents */
	fs = vtx_fs_alloc();
	assert(fs != NULL);
	rc = vtx_mount(fs, &bdev, 0);
	assert(rc == 0);

	rc = vtx_subvol_create(fs, VTXFS_OID_FIRST_USER_SUBVOL, "subvol_repair", &subvol_id);
	assert(rc == 0);

	rc = vtx_mkdir(fs, subvol_id, VTXFS_INO_ROOT_DIR, "repair_dir", 0755, &dir_ino);
	assert(rc == 0);

	rc = vtx_create(fs, subvol_id, dir_ino, "extents.dat", 0644, &file_ino);
	assert(rc == 0);

	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (__u8)((i * 37 + 11) & 0xFF);

	rc = vtx_write(fs, subvol_id, file_ino, 0, payload, sizeof(payload));
	assert(rc == 0);

	rc = vtx_sync_checkpoint(fs);
	assert(rc == 0);

	/* 3. Intentionally corrupt:
	 *    a) A REF item refcount (sets refcount != observed references).
	 */
	{
		struct vtx_key cur_k = {0}, found_k;
		struct vtx_ref_item r_it;
		int first = 1;
		__u64 target_alloc_id = 0;
		__u64 cur_refcount;

		for (;;) {
			__u8 vbuf[128];
			size_t vb = sizeof(vbuf);
			rc = vtx_rw_next(&fs->ref_tree, &cur_k, first, &found_k, vbuf, &vb);
			if (rc != 0)
				break;
			first = 0;
			cur_k = found_k;
			if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) == VTX_TYPE_REF) {
				memcpy(&r_it, vbuf, sizeof(r_it));
				if (vtx_cpu32(r_it.flags) & VTX_REF_FLAG_DATA) {
					target_alloc_id = vtx_cpu64(r_it.allocation_id);
					break;
				}
			}
		}
		assert(target_alloc_id != 0);

		cur_refcount = vtx_cpu64(r_it.refcount);
		r_it.refcount = vtx_le64(cur_refcount + 10ULL);
		rc = vtx_rw_replace(&fs->ref_tree, &found_k, &r_it, sizeof(r_it));
		assert(rc == 0);
	}

	/* Commit checkpoint with REF corruption and unmount */
	saved_sb = fs->super_blk;
	rc = vtx_unmount(fs);
	assert(rc == 0);
	latest_xid = fs->committed_xid;
	vtx_fs_free(fs);

	/* 3b. Intentionally corrupt:
	 *     b) A VAG bitmap (flips allocated bit to free and vice versa).
	 */
	corrupt_vag_bitmap_on_disk(&bdev, &saved_sb, latest_xid);
	vtx_file_bdev_close(&bdev);

	/* 4. Verify fsck.vtxfs (verify-only) detects errors and returns exit code 4 */
	snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", fsck_bin, img_path);
	status = system(cmd);
	exit_code = WEXITSTATUS(status);
	assert(exit_code == 4);
	printf("PASS: fsck.vtxfs detected corruptions and returned exit code 4\n");

	/* 5. Run fsck.vtxfs --dry-run and verify it reports errors, proposed repairs, without modifying disk (exit code 4) */
	snprintf(cmd, sizeof(cmd), "%s --dry-run %s >/dev/null 2>&1", fsck_bin, img_path);
	status = system(cmd);
	exit_code = WEXITSTATUS(status);
	assert(exit_code == 4);
	printf("PASS: fsck.vtxfs --dry-run returned exit code 4\n");

	/* Verify disk is STILL corrupt via verify-only */
	snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", fsck_bin, img_path);
	status = system(cmd);
	exit_code = WEXITSTATUS(status);
	assert(exit_code == 4);
	printf("PASS: verify confirmed disk was unmodified by dry-run\n");

	/* 6. Run fsck.vtxfs --repair -y and verify exit code 1 (corrected) */
	snprintf(cmd, sizeof(cmd), "%s --repair -y %s", fsck_bin, img_path);
	status = system(cmd);
	exit_code = WEXITSTATUS(status);
	if (exit_code != 1) {
		fprintf(stderr, "FAIL: expected exit code 1, got %d\n", exit_code);
	}
	assert(exit_code == 1);
	printf("PASS: fsck.vtxfs --repair -y corrected all errors and returned exit code 1\n");

	/* 7. Run fsck.vtxfs again and verify exit code 0 (CLEAN, 0 errors) */
	snprintf(cmd, sizeof(cmd), "%s %s", fsck_bin, img_path);
	status = system(cmd);
	exit_code = WEXITSTATUS(status);
	if (exit_code != 0) {
		fprintf(stderr, "FAIL: post-repair verification expected 0, got %d\n", exit_code);
	}
	assert(exit_code == 0);
	printf("PASS: post-repair verification returned exit code 0 (CLEAN)\n");

	/* 8. Synchronize CPD mirrors test:
	 *    Corrupt primary CPD slot -> fsck.vtxfs --repair -y restores from mirror!
	 */
	{
		__u32 slot;
		__u64 prim_cpd;
		__u8 corrupt_blk[VTXFS_BLOCK_SIZE];

		rc = vtx_file_bdev_open(&bdev, img_path, 1, 0);
		assert(rc == 0);
		fs = vtx_fs_alloc();
		assert(fs != NULL);
		rc = vtx_mount(fs, &bdev, 1);
		assert(rc == 0);
		latest_xid = fs->committed_xid;
		vtx_fs_free(fs);

		slot = VTXFS_CPD_SLOT_FOR_XID(latest_xid);
		prim_cpd = VTXFS_CPD_RING_PBLK + slot;
		memset(corrupt_blk, 0xCC, sizeof(corrupt_blk));

		rc = bdev.ops->write_blocks(&bdev, prim_cpd, 1, corrupt_blk);
		assert(rc == 0);
		vtx_file_bdev_close(&bdev);

		/* Repair restores primary CPD from mirror */
		snprintf(cmd, sizeof(cmd), "%s --repair -y %s >/dev/null 2>&1", fsck_bin, img_path);
		status = system(cmd);
		exit_code = WEXITSTATUS(status);
		assert(exit_code == 1);
		printf("PASS: fsck.vtxfs --repair -y synchronized CPD mirror and returned exit code 1\n");

		/* Verify clean */
		snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", fsck_bin, img_path);
		status = system(cmd);
		exit_code = WEXITSTATUS(status);
		assert(exit_code == 0);
		printf("PASS: post-CPD-sync verification returned exit code 0 (CLEAN)\n");
	}

	/* 9. Corrupt VTX_TYPE_ALLOC_BY_VBLK reverse-index item AND primary Superblock slot 0:
	 *    Verify fsck detects error (exit 4), --repair -y repairs both (exit 1), and re-verify is clean (exit 0).
	 */
	{
		struct vtx_key cur_k = {0}, found_k;
		int first = 1, deleted_rev = 0;
		__u8 corrupt_blk[VTXFS_BLOCK_SIZE];

		rc = vtx_file_bdev_open(&bdev, img_path, 1, 0);
		assert(rc == 0);
		fs = vtx_fs_alloc();
		assert(fs != NULL);
		rc = vtx_mount(fs, &bdev, 0);
		assert(rc == 0);

		for (;;) {
			__u8 vbuf[128];
			size_t vb = sizeof(vbuf);
			rc = vtx_rw_next(&fs->ref_tree, &cur_k, first, &found_k, vbuf, &vb);
			if (rc != 0)
				break;
			first = 0;
			cur_k = found_k;
			if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) == VTX_TYPE_ALLOC_BY_VBLK) {
				rc = vtx_rw_delete(&fs->ref_tree, &found_k);
				assert(rc == 0);
				deleted_rev = 1;
				break;
			}
		}
		assert(deleted_rev == 1);
		rc = vtx_unmount(fs);
		assert(rc == 0);
		vtx_fs_free(fs);

		memset(corrupt_blk, 0xEE, sizeof(corrupt_blk));
		rc = bdev.ops->write_blocks(&bdev, VTXFS_SUPER_RING_PBLK, 1, corrupt_blk);
		assert(rc == 0);
		vtx_file_bdev_close(&bdev);

		snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", fsck_bin, img_path);
		status = system(cmd);
		assert(WEXITSTATUS(status) == 4);

		snprintf(cmd, sizeof(cmd), "%s --repair -y %s >/dev/null 2>&1", fsck_bin, img_path);
		status = system(cmd);
		assert(WEXITSTATUS(status) == 1);

		snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", fsck_bin, img_path);
		status = system(cmd);
		assert(WEXITSTATUS(status) == 0);
		printf("PASS: VTX_TYPE_ALLOC_BY_VBLK reverse-index + Superblock mirror repaired and verified clean\n");
	}

	unlink(img_path);
	printf("ALL VTXFS FSCK REPAIR TESTS PASSED SUCCESSFULLY!\n");
	return 0;
}
