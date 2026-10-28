// SPDX-License-Identifier: GPL-2.0-only
/*
 * ordered-extent.c — theo dõi I/O dữ liệu đang bay. Commit PHẢI chờ mọi
 * ordered extent hoàn tất TRƯỚC khi ghi khối cây bẩn (metadata tham chiếu
 * các block dữ liệu đó) và publish CPD.
 */
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

struct vtxfs_ordered_extent *vtxfs_alloc_ordered(struct inode *inode,
		u64 file_offset, u64 len, u64 vblk, u64 alloc_id, u32 blocks)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = VTXFS_FSI(inode->i_sb);
	struct vtxfs_ordered_extent *oe;
	unsigned long flags;

	oe = kzalloc(sizeof(*oe), GFP_NOFS);
	if (!oe)
		return ERR_PTR(-ENOMEM);
	refcount_set(&oe->refs, 2);		/* 1 ref do danh sách, 1 ref caller */
	oe->inode = inode;
	oe->file_offset = file_offset;
	oe->len = len;
	oe->vblk = vblk;
	oe->allocation_id = alloc_id;
	oe->blocks = blocks;
	init_completion(&oe->done);

	spin_lock_irqsave(&fs->ordered_lock, flags);
	list_add_tail(&oe->list, &fs->ordered_extents);
	atomic_inc(&fs->ordered_count);
	spin_unlock_irqrestore(&fs->ordered_lock, flags);
	return oe;
}

void vtxfs_ordered_set_io_done(struct vtxfs_ordered_extent *oe)
{
	VTXFS_TRACE_ENTER();

	set_bit(OE_IO_DONE, &oe->flags);
	complete(&oe->done);
}

static void ordered_bio_endio(struct bio *bio)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_ordered_extent *oe = bio->bi_private;

	if (bio->bi_status)
		mapping_set_error(oe->inode->i_mapping, -EIO);
	vtxfs_ordered_set_io_done(oe);
	bio_put(bio);
}

/* Tháo khỏi danh sách và thả ref của danh sách (không giải phóng). */
static void ordered_unlist(struct vtxfs_ordered_extent *oe)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = VTXFS_FSI(oe->inode->i_sb);
	unsigned long flags;

	spin_lock_irqsave(&fs->ordered_lock, flags);
	if (!list_empty(&oe->list)) {
		list_del_init(&oe->list);
		atomic_dec(&fs->ordered_count);
		refcount_dec(&oe->refs);	/* ref của danh sách */
	}
	spin_unlock_irqrestore(&fs->ordered_lock, flags);
}

void vtxfs_put_ordered(struct vtxfs_ordered_extent *oe)
{
	VTXFS_TRACE_ENTER();

	if (!oe)
		return;
	if (refcount_dec_and_test(&oe->refs))
		kfree(oe);
}

/* Hoàn tất: tháo danh sách + thả ref của người gọi. */
void vtxfs_ordered_finish(struct vtxfs_ordered_extent *oe)
{
	VTXFS_TRACE_ENTER();

	if (!oe)
		return;
	ordered_unlist(oe);
	if (refcount_dec_and_test(&oe->refs))
		kfree(oe);
}

void vtxfs_ordered_complete(struct vtxfs_ordered_extent *oe)
{
	VTXFS_TRACE_ENTER();

	if (!oe)
		return;
	vtxfs_ordered_set_io_done(oe);
	vtxfs_ordered_finish(oe);
}

/* Chờ đến khi I/O xong rồi hoàn tất. Dùng ở commit. */
int vtxfs_wait_ordered_all(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_ordered_extent *oe;
	unsigned long flags;

	while (atomic_read(&fs->ordered_count) > 0) {
		spin_lock_irqsave(&fs->ordered_lock, flags);
		oe = list_first_entry_or_null(&fs->ordered_extents,
				struct vtxfs_ordered_extent, list);
		if (oe)
			refcount_inc(&oe->refs);	/* ref của ta */
		spin_unlock_irqrestore(&fs->ordered_lock, flags);
		if (!oe)
			break;
		wait_for_completion(&oe->done);	/* chỉ xong khi bio endio */
		vtxfs_ordered_finish(oe);
		cond_resched();
	}
	return 0;
}

/* Đệ trình ghi dữ liệu đã allocate xuống đĩa, gắn với ordered extent. */
int vtxfs_submit_ordered_data(struct vtxfs_fs_info *fs, u64 pblk,
			      struct page **pages, unsigned int npages,
			      struct vtxfs_ordered_extent *oe)
{
	VTXFS_TRACE_ENTER();

	struct bio *bio;
	unsigned int i;

	bio = bio_alloc(fs->bdev, npages, REQ_OP_WRITE | REQ_SYNC, GFP_NOFS);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = pblk << (VTXFS_BLOCK_SHIFT - 9);
	bio->bi_private = oe;
	bio->bi_end_io = ordered_bio_endio;
	for (i = 0; i < npages; i++) {
		if (bio_add_page(bio, pages[i], VTXFS_BLOCK_SIZE, 0)
		    != VTXFS_BLOCK_SIZE) {
			bio_put(bio);
			return -EIO;
		}
	}
	submit_bio(bio);
	return 0;
}
