/* SPDX-License-Identifier: GPL-2.0-only */
/* Synchronous multi-page BIO with per-block page-aligned bounce buffers.
 * Preserve current REQ_FUA and flush ordering; do not return before I/O.
 */
#ifndef VTXFS_BIO_BATCH_H
#define VTXFS_BIO_BATCH_H

#include <linux/blkdev.h>
#include <linux/slab.h>

#define VTXFS_BIO_BATCH_BLOCKS 256U

static int vtx_linux_submit_batched(struct block_device *bd, u64 pblk,
                                   u32 count, void *buf, blk_opf_t opf)
{
	u8 *p = buf;
	u32 done = 0;
	bool writing = (opf & REQ_OP_MASK) == REQ_OP_WRITE;
	struct page **pages;
	struct blk_plug plug;

	pages = kmalloc_array(VTXFS_BIO_BATCH_BLOCKS, sizeof(*pages), GFP_NOFS);
	if (!pages)
		return -ENOMEM;

	blk_start_plug(&plug);
	while (done < count) {
		u32 cap = min_t(u32, count - done, VTXFS_BIO_BATCH_BLOCKS);
		struct bio *bio;
		u32 i, added = 0;
		int ret = 0;

		bio = bio_alloc(bd, cap, opf, GFP_NOFS);
		if (!bio) {
			blk_finish_plug(&plug);
			kfree(pages);
			return -ENOMEM;
		}
		bio->bi_iter.bi_sector =
			(sector_t)(pblk + done) << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
		for (i = 0; i < cap; i++) {
			struct page *page = alloc_page(GFP_NOFS);
			if (!page) { ret = -ENOMEM; break; }
			if (writing)
				memcpy(page_address(page),
				       p + (size_t)(done + i) * VTXFS_BLOCK_SIZE,
				       VTXFS_BLOCK_SIZE);
			if (bio_add_page(bio, page, VTXFS_BLOCK_SIZE, 0) !=
			    VTXFS_BLOCK_SIZE) {
				__free_page(page);
				if (!added) ret = -EIO;
				break; /* Queue's BIO size cap: submit prefix. */
			}
			pages[added++] = page;
		}
		if (!ret) {
			ret = submit_bio_wait(bio);
			if (!ret && !writing)
				for (i = 0; i < added; i++)
					memcpy(p + (size_t)(done + i) * VTXFS_BLOCK_SIZE,
					       page_address(pages[i]), VTXFS_BLOCK_SIZE);
		}
		bio_put(bio);
		for (i = 0; i < added; i++) __free_page(pages[i]);
		if (ret) {
			blk_finish_plug(&plug);
			kfree(pages);
			return ret;
		}
		done += added;
	}
	blk_finish_plug(&plug);
	kfree(pages);
	return 0;
}
#endif
