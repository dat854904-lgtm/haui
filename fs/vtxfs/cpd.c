// SPDX-License-Identifier: GPL-2.0-only
/*
 * cpd.c — bước PUBLISHING của commit: dựng vtx_checkpoint_desc từ fs_info,
 * ký BLAKE3-256, ghi FUA vào slot CPD chính (xid % 8) rồi ghi mirror 64MiB.
 * Khớp giao thức ordered-commit Step 6 trong docs/P0_CONTRACTS.md.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/timekeeping.h>
#include <linux/mm.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"

static u64 root_vblk_of(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;
	u64 vblk = 0;

	if (!root)
		return 0;
	rcu_read_lock();
	eb = rcu_dereference(root->node);
	if (eb)
		vblk = eb->vblk;
	rcu_read_unlock();
	return vblk;
}

static int write_fua_block(struct block_device *bdev, sector_t sector,
			   const void *buf)
{
	VTXFS_TRACE_ENTER();

	struct page *page = is_vmalloc_addr(buf) ? vmalloc_to_page(buf) : virt_to_page(buf);
	struct bio *bio;
	int ret;

	bio = bio_alloc(bdev, 1, REQ_OP_WRITE | REQ_SYNC | REQ_FUA, GFP_NOFS);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = sector;
	if (bio_add_page(bio, page, sizeof(struct vtx_checkpoint_desc),
			 offset_in_page(buf))
	    != sizeof(struct vtx_checkpoint_desc)) {
		bio_put(bio);
		return -EIO;
	}
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

int vtxfs_publish_cpd(struct vtxfs_fs_info *fs, u64 xid)
{
	VTXFS_TRACE_ENTER();

	struct vtx_checkpoint_desc *cpd;
	sector_t prim, mirr;
	u32 slot;
	int ret;

	if (fs->shadow_only)
		return -EROFS;
	cpd = kzalloc(sizeof(*cpd), GFP_NOFS);
	if (!cpd)
		return -ENOMEM;

	cpd->hdr = fs->cp;
	memcpy(cpd->hdr.magic, VTXFS_CPD_MAGIC, VTXFS_MAGIC_LEN);
	cpd->hdr.xid = cpu_to_le64(xid);
	cpd->hdr.prev_xid = cpu_to_le64(fs->committed_xid);
	cpd->hdr.commit_time_ns = cpu_to_le64(ktime_get_real_ns());
	cpd->hdr.super_generation = fs->super.super_generation;
	memcpy(cpd->hdr.fsid_uuid, fs->super.fsid_uuid, VTXFS_UUID_SIZE);
	cpd->hdr.total_blocks = fs->super.total_blocks;
	cpd->hdr.next_oid =
		cpu_to_le64(atomic64_read(&fs->next_object_id));

	cpd->hdr.root_tree_vblk   = cpu_to_le64(root_vblk_of(fs->root_tree));
	cpd->hdr.ref_tree_vblk    = cpu_to_le64(root_vblk_of(fs->ref_root));
	cpd->hdr.sfq_tree_vblk    = cpu_to_le64(root_vblk_of(fs->sfq_root));
	cpd->hdr.reaper_tree_vblk = cpu_to_le64(root_vblk_of(fs->reaper_root));
	cpd->hdr.uuid_tree_vblk   = cpu_to_le64(root_vblk_of(fs->uuid_root));
	cpd->hdr.omap_meta_tree_vblk =
			cpu_to_le64(root_vblk_of(fs->omap_meta_root));

	cpd->hdr.tree_levels[VTX_TREE_ROOT] =
			(u8)(fs->root_tree ? root_level_pub(fs->root_tree) : 0);
	cpd->hdr.tree_levels[VTX_TREE_REF] =
			(u8)(fs->ref_root ? root_level_pub(fs->ref_root) : 0);
	cpd->hdr.tree_levels[VTX_TREE_SFQ] =
			(u8)(fs->sfq_root ? root_level_pub(fs->sfq_root) : 0);
	cpd->hdr.tree_levels[VTX_TREE_REAPER] =
			(u8)(fs->reaper_root ? root_level_pub(fs->reaper_root) : 0);
	cpd->hdr.tree_levels[VTX_TREE_UUID] =
			(u8)(fs->uuid_root ? root_level_pub(fs->uuid_root) : 0);
	cpd->hdr.tree_levels[VTX_TREE_OMAP_META] =
			(u8)(fs->omap_meta_root ? root_level_pub(fs->omap_meta_root) : 0);
	cpd->hdr.root_tree_level = cpd->hdr.tree_levels[VTX_TREE_ROOT];

	vtx_cpd_checksum_compute(cpd);

	slot = VTXFS_CPD_SLOT_FOR_XID(xid);
	prim = (sector_t)(VTXFS_CPD_RING_PBLK + slot)
	       << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
	mirr = (sector_t)(VTXFS_BOOT_MIRROR0_PBLK + VTXFS_CPD_RING_PBLK + slot)
	       << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);

	ret = write_fua_block(fs->bdev, prim, cpd);
	if (ret)
		goto out;
	ret = write_fua_block(fs->bdev, mirr, cpd);
out:
	kfree(cpd);
	return ret;
}
