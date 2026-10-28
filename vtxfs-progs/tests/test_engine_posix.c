// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * VTXFS v2.1 — Comprehensive Engine, POSIX, Snapshot, Reaper, SFQ & Fsck Tests
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
			"FSCK FAILED: rc=%d errors=%u xid=%llu vags=%u subvols=%u nodes=%llu extents=%llu refs=%llu sfq=%llu\n",
			rc, rep.errors_found,
			(unsigned long long)rep.committed_xid,
			rep.total_vags, rep.subvolumes_checked,
			(unsigned long long)rep.btree_nodes_verified,
			(unsigned long long)rep.data_extents_verified,
			(unsigned long long)rep.ref_items_verified,
			(unsigned long long)rep.sfq_entries_verified);
	}
	assert(rc == 0);
	assert(rep.errors_found == 0);
}

int main(void)
{
	struct vtx_bdev bdev;
	struct vtx_fs *fs = NULL;
	__u8 uuid[16] = {
		0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
		0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x01
	};
	int rc;

	/* 65536 blocks = 256 MiB sparse RAM disk (1 full VAG + 128 MiB bootstrap) */
	rc = vtx_ram_bdev_init(&bdev, 65536);
	assert(rc == 0);

	/* 1. Format & Mount */
	rc = vtx_mkfs(&bdev, "vtxfs-test", uuid);
	assert(rc == 0);
	assert_fsck_clean(&bdev);

	fs = vtx_fs_alloc();
	assert(fs != NULL);

	rc = vtx_mount(fs, &bdev, 0);
	assert(rc == 0);
	assert(fs->mounted == 1);
	assert(fs->committed_xid == 1);
	assert(vtx_cpu64(fs->super_blk.hdr.mkfs_time_ns) != 0);
	assert(vtx_cpu64(fs->cp_blk.hdr.commit_time_ns) != 0);
	assert(vtx_cpu64(fs->cp_blk.hdr.mirror_tree_vblk) == 0);
	assert(vtx_cpu64(fs->cp_blk.hdr.evac_tree_vblk) == 0);
	assert(vtx_cpu64(fs->cp_blk.hdr.scrub_tree_vblk) == 0);
	assert(vtx_cpu64(fs->cp_blk.hdr.keybag_tree_vblk) == 0);
	assert(vtx_cpu64(fs->cp_blk.hdr.chain_tree_vblk) == 0);

	/* Check UAPI info queries */
	{
		struct vtx_ioctl_abi abi;
		struct vtx_ioc_capabilities caps;

		memset(&abi, 0, sizeof(abi));
		abi.struct_size = sizeof(abi);
		rc = vtx_ioctl_get_abi(fs, &abi);
		assert(rc == 0);
		assert(abi.major == VTXFS_VERSION_MAJOR);

		memset(&caps, 0, sizeof(caps));
		caps.struct_size = sizeof(caps);
		rc = vtx_ioctl_get_capabilities(fs, &caps);
		assert(rc == 0);
		assert((caps.capabilities & VTXFS_CAP_P0_SINGLEDEV_RW) != 0);
	}

	/* 2. POSIX Directories, Files, Symlinks, Xattrs */
	__u64 dir_etc = 0, file_conf = 0, sym_conf = 0;

	rc = vtx_mkdir(fs, VTXFS_OID_FIRST_USER_SUBVOL, VTXFS_INO_ROOT_DIR,
		       "etc", 0755, &dir_etc);
	assert(rc == 0 && dir_etc > VTXFS_INO_ROOT_DIR);

	rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_etc,
			"app.conf", 0644, &file_conf);
	assert(rc == 0 && file_conf > dir_etc);

	rc = vtx_symlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, VTXFS_INO_ROOT_DIR,
			 "app.conf.lnk", "/etc/app.conf", &sym_conf);
	assert(rc == 0 && sym_conf > file_conf);

	rc = vtx_sync_checkpoint(fs);
	assert(rc == 0);
	assert_fsck_clean(&bdev);

	/* Lookup path & readlink */
	{
		__u64 found_ino = 0;
		__u8 ftype = 0;
		char target[64];
		size_t tlen = 0;

		rc = vtx_lookup_path(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				     "/etc/app.conf", &found_ino, &ftype);
		assert(rc == 0 && found_ino == file_conf &&
		       ftype == VTX_FT_REG);

		rc = vtx_readlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, sym_conf,
				  target, sizeof(target), &tlen);
		assert(rc == 0 && tlen == strlen("/etc/app.conf"));
		assert(strcmp(target, "/etc/app.conf") == 0);
	}

	/* Readdir on root */
	{
		struct vtx_dirent ents[16];
		__u64 cookie = 0;
		size_t count = 0;

		rc = vtx_readdir(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				 VTXFS_INO_ROOT_DIR, &cookie,
				 ents, 16, &count);
		assert(rc == 0);
		assert(count == 2);
	}

	/* Stat, chmod, chown, utimes */
	{
		struct vtx_inode_item st;

		rc = vtx_chmod(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0600);
		assert(rc == 0);
		rc = vtx_chown(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf,
			       1000, 1000);
		assert(rc == 0);
		rc = vtx_utimes(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf,
				123456789ULL, 987654321ULL);
		assert(rc == 0);

		rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, &st);
		assert(rc == 0);
		assert((vtx_cpu32(st.mode) & 0777U) == 0600U);
		assert(vtx_cpu32(st.uid) == 1000U);
		assert(vtx_cpu32(st.gid) == 1000U);
		assert(vtx_cpu64(st.atime_ns) == 123456789ULL);
		assert(vtx_cpu64(st.mtime_ns) == 987654321ULL);
	}

	/* Extended attributes & mknod */
	{
		char xbuf[64], lbuf[128];
		size_t xlen = 0, llen = 0;
		__u64 fifo_ino = 0;
		struct vtx_inode_item st;

		rc = vtx_setxattr(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf,
				  "user.author", "antigravity", 11);
		assert(rc == 0);
		rc = vtx_getxattr(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf,
				  "user.author", xbuf, sizeof(xbuf), &xlen);
		assert(rc == 0 && xlen == 11);
		assert(memcmp(xbuf, "antigravity", 11) == 0);

		rc = vtx_listxattr(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf,
				   lbuf, sizeof(lbuf), &llen);
		assert(rc == 0 && llen == 12);
		assert(strcmp(lbuf, "user.author") == 0);

		rc = vtx_removexattr(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf,
				     "user.author");
		assert(rc == 0);
		rc = vtx_getxattr(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf,
				  "user.author", xbuf, sizeof(xbuf), &xlen);
		assert(rc == -ENOENT);

		rc = vtx_mknod(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_etc,
			       "test.fifo", 0010644U, 0, &fifo_ino);
		assert(rc == 0 && fifo_ino > 0);
		rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, fifo_ino, &st);
		assert(rc == 0 && (vtx_cpu32(st.mode) & 0170000U) == 0010000U);
		rc = vtx_unlink(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_etc,
				"test.fifo");
		assert(rc == 0);
	}

	/* 3. Inline Write -> Extent Promotion -> Partial Overwrite -> CRC32C Verification */
	{
		const char *inline_str = "key=value\nmode=production\n";
		char rbuf[64];
		size_t bytes_read = 0;
		struct vtx_inode_item st;

		rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			       inline_str, strlen(inline_str));
		assert(rc == 0);

		rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, &st);
		assert(rc == 0);
		assert(vtx_cpu64(st.size_bytes) == strlen(inline_str));
		assert(vtx_cpu64(st.alloc_bytes) == strlen(inline_str));

		memset(rbuf, 0, sizeof(rbuf));
		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			      rbuf, sizeof(rbuf), &bytes_read);
		assert(rc == 0 && bytes_read == strlen(inline_str));
		assert(memcmp(rbuf, inline_str, bytes_read) == 0);

		/* Promote to multi-block extent by writing 16 KiB (4 blocks) */
		__u8 *big_w = malloc(16384);
		__u8 *big_r = malloc(16384);
		size_t i;

		assert(big_w && big_r);
		for (i = 0; i < 16384; i++)
			big_w[i] = (__u8)((i * 37 + 13) & 0xFF);

		rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			       big_w, 16384);
		assert(rc == 0);

		rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, &st);
		assert(rc == 0);
		assert(vtx_cpu64(st.size_bytes) == 16384);
		assert(vtx_cpu64(st.alloc_bytes) == 16384);

		/* Overwrite middle block (offset 4096..8192) to test extent splitting */
		for (i = 4096; i < 8192; i++)
			big_w[i] = (__u8)((i * 91 + 7) & 0xFF);
		rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 4096,
			       big_w + 4096, 4096);
		assert(rc == 0);

		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		memset(big_r, 0, 16384);
		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			      big_r, 16384, &bytes_read);
		assert(rc == 0 && bytes_read == 16384);
		assert(memcmp(big_w, big_r, 16384) == 0);

		/* Test per-block data CRC32C verification by flipping 1 bit on disk */
		{
			struct vtx_key ek;
			__u8 ev[256];
			size_t ev_len = sizeof(ev);
			struct vtx_extent_item *ei;
			__u64 data_vblk, data_pblk = 0;
			__u8 blk_buf[VTXFS_BLOCK_SIZE];
			struct vtx_fsck_report rep;

			rc = vtx_subvol_switch(fs, VTXFS_OID_FIRST_USER_SUBVOL);
			assert(rc == 0);
			ek.oid_and_type = vtx_le64(
				VTX_MAKE_OID_TYPE(file_conf, VTX_TYPE_EXTENT));
			ek.offset = vtx_le64(0);
			rc = vtx_rw_lookup(&fs->active_fs_tree, &ek,
					   ev, &ev_len);
			assert(rc == 0 && ev_len >= sizeof(*ei));
			ei = (struct vtx_extent_item *)ev;
			data_vblk = vtx_cpu64(ei->vblk);
			rc = vtx_vag_vblk_to_pblk(&fs->vag, data_vblk,
						  &data_pblk);
			assert(rc == 0 && data_pblk >= VTXFS_FIRST_VAG_PBLK);

			/* Read, flip a bit, and write back to physical block data_pblk */
			rc = bdev.ops->read_blocks(&bdev, data_pblk, 1, blk_buf);
			assert(rc == 0);
			blk_buf[100] ^= 0x40;
			rc = bdev.ops->write_blocks(&bdev, data_pblk, 1, blk_buf);
			assert(rc == 0);

			rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
				      big_r, 4096, &bytes_read);
			assert(rc == -EUCLEAN);

			rc = vtx_fsck(&bdev, &rep);
			assert(rc == -EUCLEAN);
			assert(rep.errors_found > 0);

			/* Restore the bit */
			blk_buf[100] ^= 0x40;
			rc = bdev.ops->write_blocks(&bdev, data_pblk, 1, blk_buf);
			assert(rc == 0);

			rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
				      big_r, 4096, &bytes_read);
			assert(rc == 0);
			assert_fsck_clean(&bdev);
		}

		/* Truncate down to 8192 bytes (2 blocks) */
		rc = vtx_truncate(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 8192);
		assert(rc == 0);
		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			      big_r, 16384, &bytes_read);
		assert(rc == 0 && bytes_read == 8192);
		assert(memcmp(big_w, big_r, 8192) == 0);

		free(big_w);
		free(big_r);
	}

	/* 4. Reflink, Rename, Unlink */
	{
		__u64 file_cloned = 0;
		__u8 buf1[8192], buf2[8192];
		size_t r1 = 0, r2 = 0;

		rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_etc,
				"cloned.conf", 0644, &file_cloned);
		assert(rc == 0);

		rc = vtx_reflink(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				 file_conf, 0, file_cloned, 0, 8192);
		assert(rc == 0);
		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			      buf1, sizeof(buf1), &r1);
		assert(rc == 0 && r1 == 8192);
		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_cloned, 0,
			      buf2, sizeof(buf2), &r2);
		assert(rc == 0 && r2 == 8192);
		assert(memcmp(buf1, buf2, 8192) == 0);

		/* Rename cloned.conf -> backup.conf */
		rc = vtx_rename(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				dir_etc, "cloned.conf",
				VTXFS_INO_ROOT_DIR, "backup.conf");
		assert(rc == 0);

		/* Unlink backup.conf */
		rc = vtx_unlink(fs, VTXFS_OID_FIRST_USER_SUBVOL,
				VTXFS_INO_ROOT_DIR, "backup.conf");
		assert(rc == 0);

		/* RENAME_EXCHANGE across two parents and unequal name lengths. */
		{
			__u64 left_ino = 0, right_ino = 0, found = 0;
			__u8 type = 0;
			struct vtx_inode_item st;

			rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_etc,
					"left", 0644, &left_ino);
			assert(rc == 0);
			rc = vtx_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					VTXFS_INO_ROOT_DIR, "right-long", 0644,
					&right_ino);
			assert(rc == 0);
			rc = vtx_rename_exchange(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					dir_etc, "left", VTXFS_INO_ROOT_DIR,
					"right-long");
			assert(rc == 0);
			rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL, dir_etc,
					"left", &found, &type);
			assert(rc == 0 && found == right_ino && type == VTX_FT_REG);
			rc = vtx_lookup(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					VTXFS_INO_ROOT_DIR, "right-long", &found,
					&type);
			assert(rc == 0 && found == left_ino && type == VTX_FT_REG);
			rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, left_ino, &st);
			assert(rc == 0 &&
			       vtx_cpu64(st.parent_ino) == VTXFS_INO_ROOT_DIR);
			rc = vtx_stat(fs, VTXFS_OID_FIRST_USER_SUBVOL, right_ino,
				      &st);
			assert(rc == 0 && vtx_cpu64(st.parent_ino) == dir_etc);
			rc = vtx_rename_exchange(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					dir_etc, "missing", VTXFS_INO_ROOT_DIR,
					"right-long");
			assert(rc == -ENOENT);

			/* Exchanging a directory with its child must not create a cycle. */
			{
				__u64 parent = 0, child = 0;

				rc = vtx_mkdir(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					       VTXFS_INO_ROOT_DIR, "cycle-a", 0755,
					       &parent);
				assert(rc == 0);
				rc = vtx_mkdir(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					       parent, "cycle-b", 0755, &child);
				assert(rc == 0);
				rc = vtx_rename_exchange(
					fs, VTXFS_OID_FIRST_USER_SUBVOL,
					VTXFS_INO_ROOT_DIR, "cycle-a", parent,
					"cycle-b");
				assert(rc == -EINVAL);
				rc = vtx_rmdir(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					       parent, "cycle-b");
				assert(rc == 0);
				rc = vtx_rmdir(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					       VTXFS_INO_ROOT_DIR, "cycle-a");
				assert(rc == 0);
			}
		}
		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);
	}

	/* 5. Snapshots (RO & RW), Branch Isolation, Reaper, SFQ Promotion */
	{
		__u64 snap_ro = 0, snap_rw = 0;
		__u32 steps_done = 0;
		int all_done = 0;
		char mod_buf[64];
		size_t rlen = 0;

		/* Create RO snapshot */
		rc = vtx_snapshot_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					 "snap-ro", 1, &snap_ro);
		assert(rc == 0 && snap_ro > VTXFS_OID_FIRST_USER_SUBVOL);
		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		/* Verify write to RO snapshot fails with -EROFS */
		rc = vtx_write(fs, snap_ro, file_conf, 0, "fail", 4);
		assert(rc == -EROFS);

		/* Create RW snapshot */
		rc = vtx_snapshot_create(fs, VTXFS_OID_FIRST_USER_SUBVOL,
					 "snap-rw", 0, &snap_rw);
		assert(rc == 0 && snap_rw > snap_ro);
		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		/* Modify file_conf in RW snapshot */
		rc = vtx_write(fs, snap_rw, file_conf, 0, "SNAP_RW_HEADER!", 15);
		assert(rc == 0);

		/* Modify file_conf in default subvolume differently */
		rc = vtx_write(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			       "MAIN_SV_HEADER!", 15);
		assert(rc == 0);

		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		/* Verify branch isolation across default, snap_rw, and snap_ro */
		rc = vtx_read(fs, VTXFS_OID_FIRST_USER_SUBVOL, file_conf, 0,
			      mod_buf, 15, &rlen);
		assert(rc == 0 && memcmp(mod_buf, "MAIN_SV_HEADER!", 15) == 0);

		rc = vtx_read(fs, snap_rw, file_conf, 0, mod_buf, 15, &rlen);
		assert(rc == 0 && memcmp(mod_buf, "SNAP_RW_HEADER!", 15) == 0);

		rc = vtx_read(fs, snap_ro, file_conf, 0, mod_buf, 15, &rlen);
		assert(rc == 0 && memcmp(mod_buf, "MAIN_SV_HEADER!", 15) != 0 &&
		       memcmp(mod_buf, "SNAP_RW_HEADER!", 15) != 0);

		/* Destroy both snapshots and run crash-resumable Reaper across remount */
		rc = vtx_subvol_destroy(fs, snap_ro);
		assert(rc == 0);
		rc = vtx_subvol_destroy(fs, snap_rw);
		assert(rc == 0);
		rc = vtx_sync_checkpoint(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		/* Run 1 step of reaper before unmount */
		rc = vtx_reaper_run_steps(fs, 1, &steps_done, &all_done);
		assert(rc == 0 && steps_done == 1);
		assert_fsck_clean(&bdev);

		/* Unmount & Remount to test persistence and reaper continuation */
		rc = vtx_unmount(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		rc = vtx_mount(fs, &bdev, 0);
		assert(rc == 0);

		rc = vtx_reaper_run_all(fs);
		assert(rc == 0);
		assert_fsck_clean(&bdev);

		/* Promote SFQ entries and verify bitmap rebuild */
		{
			__u32 promoted = 0;

			rc = vtx_sfq_promote(fs, fs->committed_xid, &promoted);
			assert(rc == 0 && promoted > 0);
			rc = vtx_sync_checkpoint(fs);
			assert(rc == 0);
			assert_fsck_clean(&bdev);

			/* Rebuild VAG bitmaps from authoritative REF + SFQ */
			rc = vtx_vag_rebuild_bitmaps(fs);
			assert(rc == 0);
			rc = vtx_sync_checkpoint(fs);
			assert(rc == 0);
			assert_fsck_clean(&bdev);
		}

		/* 6. Live OMAP Lookup & Scrub Ioctl Handlers */
		{
			struct vtx_ioc_omap_lookup omap_arg;
			struct vtx_ioc_scrub_args scrub_arg;

			memset(&omap_arg, 0, sizeof(omap_arg));
			omap_arg.struct_size = sizeof(omap_arg);
			omap_arg.subvol_id = 0;
			omap_arg.oid = VTXFS_OID_ROOT_TREE;
			omap_arg.hint_vblk = fs->root_tree.root_vblk;
			rc = vtx_ioctl_omap_lookup(fs, &omap_arg);
			assert(rc == 0);
			assert(omap_arg.resolved_vblk == fs->root_tree.root_vblk);
			assert(omap_arg.hint_valid == 1U);

			memset(&scrub_arg, 0, sizeof(scrub_arg));
			scrub_arg.struct_size = sizeof(scrub_arg);
			scrub_arg.flags = VTX_SCRUB_VERIFY_ONLY;
			rc = vtx_ioctl_scrub(fs, &scrub_arg);
			assert(rc == 0);
			assert(scrub_arg.meta_blocks_scrubbed >= 8ULL);
			assert(scrub_arg.data_blocks_scrubbed >= 1ULL);
			assert(scrub_arg.checksum_errors == 0ULL);
			assert(scrub_arg.uncorrectable_errors == 0ULL);
		}
	}

	rc = vtx_unmount(fs);
	assert(rc == 0);
	vtx_fs_free(fs);
	assert_fsck_clean(&bdev);
	vtx_ram_bdev_destroy(&bdev);

	printf("PASS: Engine, POSIX, extents, CRC32C, reflink, snapshots, Reaper, SFQ & fsck\n");
	return 0;
}
