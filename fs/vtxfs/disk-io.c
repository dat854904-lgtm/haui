// SPDX-License-Identifier: GPL-2.0-only
/*
 * disk-io.c — kernel block I/O with metadata checksum verification and an
 * LRU-cached metadata block cache (used by future scrub/inspect paths).
 */
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include "vtxfs_drv.h"
#include "vtxfs_subsys.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"


#define VTXFS_EB_CACHE_MAX 1024UL

struct vtxfs_eb {
	struct vtxfs_lru_entry lru;	/* must be first */
	u64 vblk, committed_xid;
	u8 data[VTXFS_BLOCK_SIZE];
};

struct vtxfs_eb_lru {
	struct vtxfs_lru_cache cache;
	struct vtxfs_fs_info *fsi;
};

static void vtxfs_eb_free_cb(struct vtxfs_lru_cache *c,
			     struct vtxfs_lru_entry *e)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_eb_lru *wrap =
		container_of(c, struct vtxfs_eb_lru, cache);
	struct vtxfs_eb *eb = container_of(e, struct vtxfs_eb, lru);

	xa_erase(&wrap->fsi->eb_index, eb->vblk);
	kfree(eb);
}

int vtxfs_disk_init_cache(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fsi = VTXFS_FSI(sb);
	struct vtxfs_eb_lru *wrap;

	wrap = kzalloc(sizeof(*wrap), GFP_KERNEL);
	if (!wrap)
		return -ENOMEM;
	wrap->fsi = fsi;
	vtxfs_lru_init(&wrap->cache, VTXFS_EB_CACHE_MAX, vtxfs_eb_free_cb);
	fsi->eb_lru = &wrap->cache;
	mutex_init(&fsi->eb_mutex);
	xa_init(&fsi->eb_index);
	return 0;
}

void vtxfs_disk_destroy_cache(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fsi = VTXFS_FSI(sb);
	struct vtxfs_eb_lru *wrap;

	if (!fsi || !fsi->eb_lru)
		return;
	wrap = container_of(fsi->eb_lru, struct vtxfs_eb_lru, cache);
	mutex_lock(&fsi->eb_mutex);
	vtxfs_lru_prune_all(fsi->eb_lru);
	mutex_unlock(&fsi->eb_mutex);
	xa_destroy(&fsi->eb_index);
	kfree(wrap);
	fsi->eb_lru = NULL;
}

/*
 * Read one metadata tree block by VBLK, verify BLAKE3-128 domain checksum.
 * Copies 4 KiB into buf_out; safe to call without holding engine locks.
 */
/* Check identity + slotted geometry in addition to checksum, as in
 * Btrfs tree-block validation and APFS node read checks. The checksum alone
 * does not establish that this is the requested VBLK or a valid tree node.
 */
static int vtxfs_disk_meta_valid(const struct vtxfs_sb_info *sbi,
				 u64 vblk, const void *data)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h = data;

	if (!vtx_node_checksum_verify(data, VTXFS_BLOCK_SIZE,
				      sbi->fs->super_blk.hdr.metadata_uuid))
		return -EUCLEAN;
	if (!vtx_node_slots_valid(h, VTXFS_BLOCK_SIZE) ||
	    vtx_cpu64(h->self_vblk) != vblk || !vtx_cpu64(h->oid) ||
	    !vtx_cpu64(h->xid))
		return -EUCLEAN;
	return 0;
}

int vtxfs_disk_read_meta(struct vtxfs_sb_info *sbi, u64 vblk, void *buf_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fsi = sbi->fs_info;
	struct vtxfs_eb *eb;
	sector_t sector;
	int ret;

	if (!buf_out || !VTXFS_VALID_VBLK(vblk))
		return -EINVAL;

	/* Respect op_lock -> eb_mutex ordering: the XID and the cached block
	 * must describe the same committed generation on cache hits. */
	mutex_lock(&sbi->op_lock);
	mutex_lock(&fsi->eb_mutex);
	eb = xa_load(&fsi->eb_index, vblk);
	if (eb && eb->committed_xid != sbi->fs->committed_xid) {
		/* The same VBLK can hold another CoW incarnation after commit.
		 * Never serve a metadata cache entry from an older checkpoint. */
		vtxfs_lru_remove(fsi->eb_lru, &eb->lru);
		xa_erase(&fsi->eb_index, vblk);
		kfree(eb);
		eb = NULL;
	}
	if (eb) {
		memcpy(buf_out, eb->data, VTXFS_BLOCK_SIZE);
		mutex_unlock(&fsi->eb_mutex);
		mutex_unlock(&sbi->op_lock);
		return 0;
	}
	mutex_unlock(&fsi->eb_mutex);
	mutex_unlock(&sbi->op_lock);

	eb = kzalloc(sizeof(*eb), GFP_KERNEL);
	if (!eb)
		return -ENOMEM;
	eb->vblk = vblk;

	/* Resolve VBLK -> PBLK with a matching committed checkpoint. */
	mutex_lock(&sbi->op_lock);
	eb->committed_xid = sbi->fs->committed_xid;
	ret = vtx_vag_vblk_to_pblk(&sbi->fs->vag, vblk, &eb->lru.key);
	mutex_unlock(&sbi->op_lock);
	if (ret) {
		kfree(eb);
		return ret;
	}

	sector = (sector_t)eb->lru.key << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
	{
		struct bio *bio;
		struct page *page = vtx_buf_to_page(eb->data);

		bio = bio_alloc(sbi->bdev, 1, REQ_OP_READ, GFP_NOFS);
		bio->bi_iter.bi_sector = sector;
		if (bio_add_page(bio, page, VTXFS_BLOCK_SIZE,
				 offset_in_page(eb->data)) != VTXFS_BLOCK_SIZE) {
			bio_put(bio);
			kfree(eb);
			return -EIO;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret) {
			kfree(eb);
			return ret;
		}
	}

	ret = vtxfs_disk_meta_valid(sbi, vblk, eb->data);
	if (ret) {
		kfree(eb);
		return ret;
	}
	/* The checkpoint could change while a bio was pending. Do not return
	 * data resolved from a different VAG/OMAP generation. */
	mutex_lock(&sbi->op_lock);
	if (eb->committed_xid != sbi->fs->committed_xid) {
		mutex_unlock(&sbi->op_lock);
		kfree(eb);
		return -EAGAIN;
	}
	mutex_lock(&fsi->eb_mutex);
	if (!xa_load(&fsi->eb_index, vblk)) {
		void *old = xa_store(&fsi->eb_index, vblk, eb, GFP_NOFS);

		if (xa_is_err(old)) {
			ret = xa_err(old);
			goto cache_out;
		}
		/* Copy BEFORE publishing to LRU: eviction callbacks own cache
		 * entries, so do not dereference 'eb' after LRU manipulation. */
		memcpy(buf_out, eb->data, VTXFS_BLOCK_SIZE);
		vtxfs_lru_add(fsi->eb_lru, &eb->lru);
		eb = NULL; /* transferred to cache; no dereference after add */
	} else {
		/* Another reader installed the same checkpoint's block. */
		struct vtxfs_eb *cached = xa_load(&fsi->eb_index, vblk);

		if (cached->committed_xid == eb->committed_xid) {
			kfree(eb);
			eb = cached;
		} /* Otherwise return fresh verified read, without caching. */
	}
	if (eb) {
		memcpy(buf_out, eb->data, VTXFS_BLOCK_SIZE);
		/* The fresh buffer was not inserted: free unless we selected the
		 * already cached copy (which remains owned by xarray/LRU). */
		if (xa_load(&fsi->eb_index, vblk) != eb)
			kfree(eb);
	}
	ret = 0;
cache_out:
	if (ret)
		kfree(eb);
	mutex_unlock(&fsi->eb_mutex);
	mutex_unlock(&sbi->op_lock);
	return ret;
}

int vtxfs_disk_flush(struct vtxfs_sb_info *sbi)
{
	VTXFS_TRACE_ENTER();

	return blkdev_issue_flush(sbi->bdev);
}

