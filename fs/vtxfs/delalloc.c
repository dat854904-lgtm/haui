// SPDX-License-Identifier: GPL-2.0-only
/*
 * delalloc.c — delayed allocation thật: ghi chỉ đánh dấu DELALLOC + reserve;
 * allocate extent + tính CRC32C + chèn extent item chỉ diễn ra ở flush
 * (run_delalloc_range), dữ liệu gom từ page cache.
 */
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/bio.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"

#define VTXFS_DELALLOC_CHUNK (128U * 1024U)

/* Bước 1 khi write: đánh dấu delalloc + reserve (chưa allocate). */
int vtxfs_delalloc_reserve(struct inode *inode, u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_inode *vinode = VTXFS_I(inode);
	u64 len = end - start + 1;
	int ret;

	ret = vtxfs_set_extent_bit(&vinode->io_tree, start, end,
				   VTXFS_EXT_DELALLOC | VTXFS_EXT_DIRTY);
	if (ret)
		return ret;
	spin_lock(&vinode->lock);
	vinode->reserved_bytes += len;
	vinode->outstanding_extents +=
		DIV_ROUND_UP(len, VTXFS_MAX_EXTENT_BYTES);
	spin_unlock(&vinode->lock);
	return 0;
}

/* Gom dữ liệu [start,end] từ page cache vào buffer, trả số byte thực. */
static size_t gather_pages(struct inode *inode, u64 start, u64 end, u8 *buf)
{
	VTXFS_TRACE_ENTER();

	struct address_space *mapping = inode->i_mapping;
	u64 pos;
	size_t got = 0;

	for (pos = start; pos <= end; pos += PAGE_SIZE) {
		struct page *page = find_get_page(mapping,
						  pos >> PAGE_SHIFT);
		size_t off_in_pg = pos & (PAGE_SIZE - 1);
		size_t n = min_t(size_t, PAGE_SIZE - off_in_pg,
				 end - pos + 1);
		void *k;

		if (!page) {
			memset(buf + got, 0, n);
			got += n;
			continue;
		}
		k = kmap_local_page(page);
		memcpy(buf + got, k + off_in_pg, n);
		kunmap_local(k);
		put_page(page);
		got += n;
	}
	return got;
}

/* Ghi dữ liệu đã allocate xuống block device (data write). */
static int submit_data(struct vtxfs_fs_info *fs, u64 pblk, u32 blocks,
		       const u8 *data)
{
	VTXFS_TRACE_ENTER();

	u32 i;
	int ret = 0;

	for (i = 0; i < blocks; i++) {
		const void *ptr = data + (size_t)i * VTXFS_BLOCK_SIZE;
		struct page *pg = vtx_buf_to_page(ptr);
		struct bio *bio = bio_alloc(fs->bdev, 1,
				REQ_OP_WRITE | REQ_SYNC, GFP_NOFS);

		bio->bi_iter.bi_sector =
			(pblk + i) << (VTXFS_BLOCK_SHIFT - 9);
		if (bio_add_page(bio, pg, VTXFS_BLOCK_SIZE,
				 offset_in_page(ptr))
		    != VTXFS_BLOCK_SIZE) {
			bio_put(bio);
			return -EIO;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Bước 2 khi flush: với mỗi range DELALLOC trong [start,end], allocate extent,
 * tính CRC32C từng block, chèn extent item, ghi dữ liệu, clear DELALLOC.
 */
int vtxfs_run_delalloc_range(struct inode *inode, u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_inode *vinode = VTXFS_I(inode);
	struct vtxfs_root *root = vinode->root ? vinode->root :
				  (inode->i_sb ? VTXFS_FSI(inode->i_sb)->root_tree : NULL);
	struct vtxfs_fs_info *fs;
	u64 cur = start;
	u8 *buf;
	int ret = 0;

	if (!root || !root->fs)
		return -EINVAL;
	fs = root->fs;

	buf = kvmalloc(VTXFS_DELALLOC_CHUNK, GFP_NOFS);
	if (!buf)
		return -ENOMEM;

	while (cur <= end) {
		u64 chunk_end = min_t(u64, end, cur + VTXFS_DELALLOC_CHUNK - 1);
		u64 al_start = cur & ~(u64)(VTXFS_BLOCK_SIZE - 1);
		u64 al_end = ALIGN(chunk_end + 1, VTXFS_BLOCK_SIZE);
		u32 nblocks = (al_end - al_start) >> VTXFS_BLOCK_SHIFT;
		size_t got;
		u64 vblk = 0, pblk = 0, alloc_id;
		__le32 csums[VTXFS_MAX_EXTENT_BLOCKS];
		u32 b;

		nblocks = min_t(u32, nblocks, VTXFS_MAX_EXTENT_BLOCKS);
		got = gather_pages(inode, al_start, al_start +
				   ((u64)nblocks << VTXFS_BLOCK_SHIFT) - 1, buf);

		/* ALLOCATE tại flush (không phải lúc write). */
		if (!fs->alloc_ops || !fs->alloc_ops->alloc_blocks) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = fs->alloc_ops->alloc_blocks(fs, nblocks, &vblk);
		if (ret)
			break;
		ret = vtxfs_vag_vblk_to_pblk(fs->vag, vblk, &pblk);
		if (ret)
			break;
		alloc_id = atomic64_inc_return(&fs->next_object_id);

		/* CRC32C từng block 4 KiB. */
		for (b = 0; b < nblocks; b++)
			csums[b] = cpu_to_le32(vtx_crc32c(
					buf + (size_t)b * VTXFS_BLOCK_SIZE,
					VTXFS_BLOCK_SIZE));

		struct vtxfs_ordered_extent *oe = NULL;

		/* chèn extent item (đánh dấu cây bẩn) TRƯỚC, rồi ghi dữ liệu */
		ret = insert_extent_item_crc(root, vinode->ino, al_start,
					     vblk, alloc_id, nblocks, csums);
		if (ret)
			break;
		vtxfs_ctree_delayed_ref(fs, alloc_id, vblk, nblocks,
					VTX_REF_FLAG_DATA, 1);

		if (inode->i_sb)
			oe = vtxfs_alloc_ordered(inode, al_start,
						 (u64)nblocks << VTXFS_BLOCK_SHIFT,
						 vblk, alloc_id, nblocks);

		/* Ghi dữ liệu */
		ret = submit_data(fs, pblk, nblocks, buf);
		if (ret) {
			if (oe && !IS_ERR(oe)) {
				vtxfs_ordered_finish(oe);
				vtxfs_put_ordered(oe);
			}
			break;
		}
		if (oe && !IS_ERR(oe)) {
			vtxfs_ordered_set_io_done(oe);
			vtxfs_put_ordered(oe);
		}

		/* clear DELALLOC, giải phóng reservation. */
		vtxfs_clear_extent_bit(&vinode->io_tree, cur, chunk_end,
				       VTXFS_EXT_DELALLOC | VTXFS_EXT_DIRTY);
		spin_lock(&vinode->lock);
		vinode->reserved_bytes -= (chunk_end - cur + 1);
		spin_unlock(&vinode->lock);
		cur = chunk_end + 1;
	}
	kvfree(buf);
	return ret;
}
