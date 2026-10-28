// SPDX-License-Identifier: GPL-2.0-only
/*
 * selftest_e2e.c — mount end-to-end trong kernel:
 *   RAM disk -> engine mkfs -> kernel native mount -> ctree ops
 *            -> engine vtx_fsck xác nhận tương thích đĩa.
 */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include "ctree.h"
#include "vag.h"
#include "vtxfs_subsys.h"
#include "vtxfs_engine.h"
#include "vtxfs_trace.h"

/* --- adapter: kernel bdev -> engine vtx_bdev_ops --- */
static int kbdev_rw(struct block_device *bdev, u64 pblk, u32 count,
		    void *buf, int write)
{
	VTXFS_TRACE_ENTER();

	struct page *pg;
	u32 i;
	int ret = 0;

	pg = alloc_page(GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		void *src_dst = (u8 *)buf + (size_t)i * VTXFS_BLOCK_SIZE;
		void *kaddr;
		struct bio *bio;

		if (write) {
			kaddr = kmap_local_page(pg);
			memcpy(kaddr, src_dst, VTXFS_BLOCK_SIZE);
			kunmap_local(kaddr);
		}

		bio = bio_alloc(bdev, 1,
				write ? (REQ_OP_WRITE | REQ_SYNC) : REQ_OP_READ,
				GFP_KERNEL);

		bio->bi_iter.bi_sector =
			(pblk + i) << (VTXFS_BLOCK_SHIFT - 9);
		__bio_add_page(bio, pg, VTXFS_BLOCK_SIZE, 0);

		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			break;

		if (!write) {
			kaddr = kmap_local_page(pg);
			memcpy(src_dst, kaddr, VTXFS_BLOCK_SIZE);
			kunmap_local(kaddr);
		}
	}
	__free_page(pg);
	return ret;
}

static int kbdev_read(struct vtx_bdev *b, u64 p, u32 c, void *buf)
{
	VTXFS_TRACE_ENTER();

	return kbdev_rw(b->priv, p, c, buf, 0);
}

static int kbdev_write(struct vtx_bdev *b, u64 p, u32 c, const void *buf)
{
	VTXFS_TRACE_ENTER();

	return kbdev_rw(b->priv, p, c, (void *)buf, 1);
}

static int kbdev_fua(struct vtx_bdev *b, u64 p, u32 c, const void *buf)
{
	VTXFS_TRACE_ENTER();

	return kbdev_rw(b->priv, p, c, (void *)buf, 1);
}

static int kbdev_flush(struct vtx_bdev *b)
{
	VTXFS_TRACE_ENTER();

	return blkdev_issue_flush((struct block_device *)b->priv);
}

static const struct vtx_bdev_ops kbdev_ops = {
	.read_blocks = kbdev_read,
	.write_blocks = kbdev_write,
	.write_fua = kbdev_fua,
	.flush = kbdev_flush,
};

int vtxfs_run_e2e_test(void);

int vtxfs_run_e2e_test(void)
{
	VTXFS_TRACE_ENTER();

	struct gendisk *disk;
	struct block_device *bdev;
	struct vtx_bdev ebdev = { };
	struct vtx_fsck_report rep = { };
	struct super_block *sb;
	struct vtx_key k;
	struct vtxfs_path *path;
	u8 fsid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		       0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01};
	int ret, fails = 0;

	disk = vtxfs_ramdisk_create(65536);	/* 256 MiB */
	if (IS_ERR(disk))
		return PTR_ERR(disk);
	bdev = disk->part0;
	if (!bdev) {
		vtxfs_ramdisk_destroy(disk);
		return -ENXIO;
	}

	/* 1. Format bằng engine qua adapter (đã kiểm chứng userspace). */
	ebdev.ops = &kbdev_ops;
	ebdev.priv = bdev;
	ebdev.total_blocks = 65536;
	ret = vtx_mkfs(&ebdev, "e2e", fsid);
	if (ret) {
		pr_err("e2e: mkfs %d\n", ret);
		fails++;
		goto out;
	}

	/* 2. Mount native trong kernel. */
	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb) {
		fails++;
		goto out;
	}
	sb->s_bdev = bdev;
	sb->s_fs_info = kzalloc(sizeof(struct vtxfs_sb_info), GFP_KERNEL);
	if (!sb->s_fs_info) {
		kfree(sb);
		fails++;
		goto out;
	}
	ret = vtxfs_mount_native(sb);
	if (ret) {
		pr_err("e2e: native mount %d\n", ret);
		fails++;
		goto out_sb;
	}

	/* 3. Exercise ctree: chèn đủ lớn để tách tầng, rồi xóa để merge. */
	{
		struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
		struct vtxfs_fs_info *fs = sbi->fs_info;
		struct vtxfs_root *rt = fs->root_tree;
		int i;

		mutex_lock(&fs->trans_lock);
		fs->running = kzalloc(sizeof(struct vtxfs_trans), GFP_KERNEL);
		if (!fs->running) {
			mutex_unlock(&fs->trans_lock);
			fails++;
			goto out_umount;
		}
		fs->running->fs = fs;
		fs->running->delayed_refs = RB_ROOT;
		spin_lock_init(&fs->running->delayed_ref_lock);

		for (i = 0; i < 400; i++) {
			char val[64];

			memset(val, i & 0xff, sizeof(val));
			vtxfs_mk_key(&k, 9000 + i, VTX_TYPE_ROOT_ITEM, 0);
			path = vtxfs_alloc_path();
			if (!path) {
				fails++;
				break;
			}
			ret = vtxfs_search_slot(rt, path, &k, sizeof(val), 1);
			if (ret == 1)
				ret = vtxfs_insert_item(rt, path, &k,
							sizeof(val), NULL);
			vtxfs_free_path(path);
			if (ret) {
				fails++;
				break;
			}
		}

		/* xóa một nửa để kích hoạt merge/rebalance + co gốc */
		for (i = 0; i < 400; i += 2) {
			vtxfs_mk_key(&k, 9000 + i, VTX_TYPE_ROOT_ITEM, 0);
			path = vtxfs_alloc_path();
			if (!path) {
				fails++;
				break;
			}
			ret = vtxfs_search_slot(rt, path, &k, 0, 1);
			if (ret == 0) {
				ret = vtxfs_del_item(rt, path);
			}
			vtxfs_free_path(path);
			if (ret && ret != -ENOENT) {
				fails++;
				break;
			}
		}

		/* drain delayed-ref vào cây REF rồi commit */
		vtxfs_run_delayed_refs(fs->running);
		vtxfs_write_dirty_ebs(fs);
		kfree(fs->running);
		fs->running = NULL;
		mutex_unlock(&fs->trans_lock);
	}

	/* 4. Engine fsck xác nhận dữ liệu kernel ghi ra đọc được. */
	ret = vtx_fsck(&ebdev, &rep);
	if (ret != 0 || rep.errors_found != 0) {
		pr_err("e2e: fsck rc=%d errors=%u\n", ret, rep.errors_found);
		fails++;
	}

out_umount:
	vtxfs_mount_native_teardown(sb);
out_sb:
	kfree(sb->s_fs_info);
	kfree(sb);

out:
	vtxfs_ramdisk_destroy(disk);
	pr_info("vtxfs e2e selftest: %s (%d failures)\n",
		fails ? "FAIL" : "PASS", fails);
	return fails ? -EINVAL : 0;
}

#ifdef MODULE
static int __init vtxfs_e2e_init(void)
{
	VTXFS_TRACE_ENTER();

	return vtxfs_run_e2e_test();
}

static void __exit vtxfs_e2e_exit(void)
{
	VTXFS_TRACE_ENTER();

}

module_init(vtxfs_e2e_init);
module_exit(vtxfs_e2e_exit);
MODULE_DESCRIPTION("VTXFS end-to-end self-test");
MODULE_AUTHOR("VTXFS Team");
MODULE_LICENSE("GPL");
#endif
