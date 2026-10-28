// SPDX-License-Identifier: GPL-2.0-only
/*
 * ramdisk.c — block device RAM bio-based để self-test mount end-to-end.
 * Dữ liệu nằm trong buffer vzalloc; xử lý bio đọc/ghi trực tiếp.
 */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include "ctree.h"
#include "vtxfs_trace.h"

#define VTXRAM_SECT_SHIFT 9

struct vtxfs_ramdisk {
	struct gendisk *disk;
	u8 *data;
	sector_t capacity;	/* sectors */
	spinlock_t lock;
	int id;
};

static DEFINE_IDA(vtxram_ida);

static void vtxram_submit_bio(struct bio *bio)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_ramdisk *rd = bio->bi_bdev->bd_disk->private_data;
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned long flags;

	spin_lock_irqsave(&rd->lock, flags);
	bio_for_each_segment(bvec, bio, iter) {
		size_t len = bvec.bv_len;
		loff_t off = (loff_t)sector << VTXRAM_SECT_SHIFT;
		void *pg = kmap_local_page(bvec.bv_page);

		if (off + len > ((loff_t)rd->capacity << VTXRAM_SECT_SHIFT)) {
			kunmap_local(pg);
			goto ioerr;
		}
		if (bio_op(bio) == REQ_OP_READ)
			memcpy(pg + bvec.bv_offset, rd->data + off, len);
		else
			memcpy(rd->data + off, pg + bvec.bv_offset, len);
		kunmap_local(pg);
		sector += len >> VTXRAM_SECT_SHIFT;
	}
	spin_unlock_irqrestore(&rd->lock, flags);
	bio_endio(bio);
	return;
ioerr:
	spin_unlock_irqrestore(&rd->lock, flags);
	bio_io_error(bio);
}

static const struct block_device_operations vtxram_fops = {
	.owner = THIS_MODULE,
	.submit_bio = vtxram_submit_bio,
};

struct gendisk *vtxfs_ramdisk_create(u64 nr_fs_blocks)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_ramdisk *rd;
	u64 bytes = nr_fs_blocks << VTXFS_BLOCK_SHIFT;
	struct queue_limits lim = {
		.logical_block_size = VTXFS_BLOCK_SIZE,
		.physical_block_size = VTXFS_BLOCK_SIZE,
		.max_sectors = UINT_MAX,
	};

	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return ERR_PTR(-ENOMEM);
	rd->data = vzalloc(bytes);
	if (!rd->data) {
		kfree(rd);
		return ERR_PTR(-ENOMEM);
	}
	spin_lock_init(&rd->lock);
	rd->capacity = bytes >> VTXRAM_SECT_SHIFT;
	rd->id = ida_alloc(&vtxram_ida, GFP_KERNEL);
	if (rd->id < 0) {
		vfree(rd->data);
		kfree(rd);
		return ERR_PTR(rd->id);
	}

	rd->disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR_OR_NULL(rd->disk)) {
		ida_free(&vtxram_ida, rd->id);
		vfree(rd->data);
		kfree(rd);
		return rd->disk ? rd->disk : ERR_PTR(-ENOMEM);
	}
	rd->disk->fops = &vtxram_fops;
	rd->disk->private_data = rd;
	snprintf(rd->disk->disk_name, DISK_NAME_LEN, "vtxram%d", rd->id);
	set_capacity(rd->disk, rd->capacity);
	if (add_disk(rd->disk)) {
		put_disk(rd->disk);
		ida_free(&vtxram_ida, rd->id);
		vfree(rd->data);
		kfree(rd);
		return ERR_PTR(-ENXIO);
	}
	return rd->disk;
}

void vtxfs_ramdisk_destroy(struct gendisk *disk)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_ramdisk *rd;

	if (!disk)
		return;
	rd = disk->private_data;
	del_gendisk(disk);
	put_disk(disk);
	ida_free(&vtxram_ida, rd->id);
	vfree(rd->data);
	kfree(rd);
}
EXPORT_SYMBOL_GPL(vtxfs_ramdisk_create);
EXPORT_SYMBOL_GPL(vtxfs_ramdisk_destroy);
