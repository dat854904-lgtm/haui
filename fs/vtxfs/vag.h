/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VTXFS_VAG_H
#define _VTXFS_VAG_H

#include <linux/mutex.h>
#include <linux/blkdev.h>
#include "vtxfs_mem.h"

/* Kernel-side two-level VAG directory (heap-backed; never on stack). */
struct vtxfs_vag_dir {
	struct vtx_vag_dir_root root;	/* 4 KiB root block */
	struct vtx_vag_desc *descs;	/* 2048 * 32 B = 64 KiB leaf */
	u8 **bitmaps;			/* total_vags * 4 KiB */
	u32 total_vags;
	u64 base_pblk;			/* pblk of this copy (for persist) */
	struct mutex lock;
};

/* Shared block read used by mount-native & vag-alloc (bio-based). */
int vtxfs_bdev_read_blocks(struct block_device *bdev, u64 pblk,
			   u32 count, void *buf);

int vtxfs_vag_load(struct block_device *bdev,
		   const struct vtx_phys_ref *ref, u64 total_blocks,
		   const u8 metadata_uuid[VTXFS_UUID_SIZE],
		   struct vtxfs_vag_dir **out);
void vtxfs_vag_free(struct vtxfs_vag_dir *vag);
int vtxfs_vag_vblk_to_pblk(struct vtxfs_vag_dir *vag, u64 vblk, u64 *pblk);
/* Reject runs crossing a VAG boundary rather than reading unrelated blocks. */
int vtxfs_vag_run_to_pblk(struct vtxfs_vag_dir *vag, u64 vblk,
                          u32 blocks, u64 *pblk);
int vtxfs_vag_test_bit(struct vtxfs_vag_dir *vag, u64 vblk);
int vtxfs_vag_alloc_blocks(struct vtxfs_vag_dir *vag, u32 count,
			   u64 *start_vblk);
int vtxfs_vag_free_blocks(struct vtxfs_vag_dir *vag, u64 start_vblk,
			  u32 count);

/* Wired into fs_info->alloc_ops (see subsys.h). */
extern const struct vtxfs_alloc_ops vtxfs_vag_alloc_ops;

#endif /* _VTXFS_VAG_H */
