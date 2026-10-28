// SPDX-License-Identifier: GPL-2.0-only
/*
 * VTXFS v2.1 P0.6 — superblock, mount glue and module registration.
 * Target: mainline Linux >= 6.8 (fs_context / get_tree_bdev APIs).
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/backing-dev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>

#include "vtxfs_drv.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_trace.h"
#include "vtxfs_bio_batch.h"

/* ------------------------------------------------------------------
 * Block device adapter: engine vtx_bdev_ops -> Linux block_device.
 * The engine commit protocol already orders data -> flush -> meta ->
 * flush -> CPD FUA, so plain write_blocks may be synchronous bio writes.
 * ------------------------------------------------------------------ */

static int vtx_linux_submit_blocks(struct block_device *bd, u64 pblk,
				   u32 count, void *buf, blk_opf_t opf)
{
	VTXFS_TRACE_ENTER();

	struct page *bounce_page;
	u8 *bounce, *p = buf;
	u32 i;
	int ret = 0;
	bool write_op = (opf & REQ_OP_MASK) == REQ_OP_WRITE;

	if (!bd || !p || !count)
		return -EINVAL;
	if (pblk >= (bdev_nr_bytes(bd) >> VTXFS_BLOCK_SHIFT) ||
	    count > (bdev_nr_bytes(bd) >> VTXFS_BLOCK_SHIFT) - pblk)
		return -EIO;
	if (count > 1)
		return vtx_linux_submit_batched(bd, pblk, count, buf, opf);

	/*
	 * Portable engine buffers can start at arbitrary offsets inside a
	 * vzalloc'd vtx_fs (e.g. fs->dev_tbl), and can straddle two pages.
	 * Mapping a single page plus offset with len=4096 violates bio_add_page
	 * capacity. Always DMA a full page-aligned bounce and copy by bytes.
	 * This fixes a kernel-only mount failure on valid mkfs images.
	 */
	bounce_page = alloc_page(GFP_NOFS);
	if (!bounce_page)
		return -ENOMEM;
	bounce = page_address(bounce_page);
	for (i = 0; i < count; i++) {
		u8 *blk = p + (size_t)i * VTXFS_BLOCK_SIZE;
		struct bio *bio;

		if (write_op)
			memcpy(bounce, blk, VTXFS_BLOCK_SIZE);
		bio = bio_alloc(bd, 1, opf, GFP_NOFS);
		if (!bio) {
			ret = -ENOMEM;
			break;
		}
		bio->bi_iter.bi_sector =
			(sector_t)(pblk + i) << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
		if (bio_add_page(bio, bounce_page, VTXFS_BLOCK_SIZE, 0) !=
		    VTXFS_BLOCK_SIZE) {
			ret = -EIO;
			bio_put(bio);
			break;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			break;
		if (!write_op)
			memcpy(blk, bounce, VTXFS_BLOCK_SIZE);
	}
	__free_page(bounce_page);
	return ret;
}

static int vtx_linux_read_blocks(struct vtx_bdev *bdev, u64 pblk, u32 count,
				 void *buf)
{
	VTXFS_TRACE_ENTER();

	struct block_device *bd = bdev->priv;

	bdev->reads_issued++;
	return vtx_linux_submit_blocks(bd, pblk, count, buf, REQ_OP_READ);
}

static int vtx_linux_write_blocks(struct vtx_bdev *bdev, u64 pblk, u32 count,
				  const void *buf)
{
	VTXFS_TRACE_ENTER();

	struct block_device *bd = bdev->priv;

	bdev->writes_issued++;
	return vtx_linux_submit_blocks(bd, pblk, count, (void *)buf,
				       REQ_OP_WRITE | REQ_SYNC);
}

/* FUA: synchronous per-block bios with REQ_FUA. */
static int vtx_linux_write_fua(struct vtx_bdev *bdev, u64 pblk, u32 count,
			       const void *buf)
{
	VTXFS_TRACE_ENTER();

	struct block_device *bd = bdev->priv;

	bdev->fuas_issued++;
	return vtx_linux_submit_blocks(bd, pblk, count, (void *)buf,
				       REQ_OP_WRITE | REQ_SYNC | REQ_FUA);
}

static int vtx_linux_flush(struct vtx_bdev *bdev)
{
	VTXFS_TRACE_ENTER();

	bdev->flushes_issued++;
	return blkdev_issue_flush(bdev->priv);
}

static const struct vtx_bdev_ops vtx_linux_bdev_ops = {
	.read_blocks  = vtx_linux_read_blocks,
	.write_blocks = vtx_linux_write_blocks,
	.write_fua    = vtx_linux_write_fua,
	.flush        = vtx_linux_flush,
};

/* ------------------------------------------------------------------ */

static void vtxfs_put_super(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);

	if (!sbi)
		return;
	if (sbi->fs_info) {
		vtxfs_scrub_stop(sbi->fs_info);
		vtxfs_reaper_stop(sbi->fs_info);
		vtxfs_sysfs_exit(sbi->fs_info);
		vtxfs_async_stop(sbi->fs_info);
		vtxfs_mount_native_teardown(sb);
		sbi->fs_info = NULL;
	}
	if (sbi->fs) {
		vtx_unmount(sbi->fs);	/* commits final clean checkpoint */
		vtx_fs_free(sbi->fs);
		sbi->fs = NULL;
	}
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static int vtxfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	VTXFS_TRACE_ENTER();

	struct super_block *sb = dentry->d_sb;
	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	u64 total, free_blks, num_inodes;

	vtxfs_op_begin(sbi);
	vtx_subvol_switch(sbi->fs, sbi->subvol_id);
	total      = vtx_cpu64(sbi->fs->cp_blk.hdr.total_blocks);
	free_blks  = vtx_cpu64(sbi->fs->cp_blk.hdr.free_blocks);
	num_inodes = vtx_cpu64(sbi->fs->active_subvol.num_inodes);
	vtxfs_op_end(sbi);

	buf->f_type    = VTXFS_SB_MAGIC;
	buf->f_bsize   = VTXFS_BLOCK_SIZE;
	buf->f_blocks  = total;
	buf->f_bfree   = free_blks;
	buf->f_bavail  = free_blks;
	/* Inodes are dynamic B+Tree objects, not a fixed inode bitmap.
	 * Report an upper-bound of available inode slots based on free
	 * allocation blocks; 0 would incorrectly make df -i show 100% used. */
	buf->f_files   = num_inodes + free_blks;
	buf->f_ffree   = free_blks;
	buf->f_namelen = VTXFS_NAME_MAX;
	return 0;
}

static int vtxfs_sync_fs(struct super_block *sb, int wait)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	int ret = 0;
	u64 want;

	if (!wait || sb_rdonly(sb))
		return 0;

	want = atomic64_read(&sbi->op_seq);
	if (READ_ONCE(sbi->durable_seq) >= want)
		return 0;

	vtxfs_commit_begin(sbi);
	want = atomic64_read(&sbi->op_seq);
	if (sbi->durable_seq < want) {
		ret = vtx_sync_checkpoint(sbi->fs);
		if (!ret)
			WRITE_ONCE(sbi->durable_seq, want);
	}
	vtxfs_commit_end(sbi);
	return ret;
}

static int vtxfs_show_options(struct seq_file *seq, struct dentry *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(root->d_sb);

	seq_printf(seq, ",label=%.*s", VTXFS_LABEL_SIZE,
		   sbi->fs->super_blk.hdr.label);
	seq_printf(seq, ",subvol=%llu", sbi->subvol_id);
	return 0;
}

static int vtxfs_freeze_fs(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	int ret = 0;

	if (!sb_rdonly(sb)) {
		vtxfs_commit_begin(sbi);
		ret = vtx_sync_checkpoint(sbi->fs);
		vtxfs_commit_end(sbi);
	}
	return ret;
}

static int vtxfs_unfreeze_fs(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	(void)sb;
	return 0;
}

static void vtxfs_evict_inode(struct inode *inode)
{
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	int ret = 0;

	/* Dirty mmap pages are reconciled by the VFS before the last
	 * inode reference reaches this callback.  The engine retains
	 * the orphan's extents until this final eviction. */
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (inode->i_nlink || !sbi || !sbi->fs || !sbi->fs->mounted)
		return;
	vtxfs_op_begin(sbi);
	if (!sbi->fs->read_only)
		ret = vtx_orphan_purge(sbi->fs, sbi->subvol_id,
				       VTXFS_I(inode)->vtx_ino);
	vtxfs_op_end(sbi);
	if (ret)
		pr_err("vtxfs: orphan purge failed ino=%llu error=%d\n",
		       (unsigned long long)inode->i_ino, ret);
}

static const struct super_operations vtxfs_super_ops = {
	.alloc_inode	= vtxfs_alloc_inode,
	.free_inode	= vtxfs_free_inode,
	.evict_inode	= vtxfs_evict_inode,
	.put_super	= vtxfs_put_super,
	.statfs		= vtxfs_statfs,
	.sync_fs	= vtxfs_sync_fs,
	.show_options	= vtxfs_show_options,
	.freeze_fs	= vtxfs_freeze_fs,
	.unfreeze_fs	= vtxfs_unfreeze_fs,
};

struct vtxfs_parse_opts {
	u64 subvol_id;	/* 0 = default subvolume */
};

static int vtxfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_parse_opts *opts = fc->fs_private;
	unsigned long long v = 0;

	/* fs_context invokes ->parse_param for the mount source as well.
	 * Without this, mount -t vtxfs /dev/vda /mnt fails with EINVAL
	 * before fill_super is reached (verified in isolated QEMU). */
	if (strcmp(param->key, "source") == 0)
		return vfs_parse_fs_param_source(fc, param);

	if (strcmp(param->key, "subvol") == 0) {
		if (param->type != fs_value_is_string || !param->string)
			return -EINVAL;
		if (kstrtoull(param->string, 10, &v) != 0)
			return -EINVAL;
		if (v < VTXFS_OID_FIRST_USER_SUBVOL ||
		    v > VTX_KEY_OID_MASK)
			return -EINVAL;
		opts->subvol_id = (u64)v;
		return 0;
	}
	return -EINVAL;	/* no other P0 options */
}

static int vtxfs_reconfigure(struct fs_context *fc)
{
	VTXFS_TRACE_ENTER();

	struct super_block *sb = fc->root->d_sb;
	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	bool new_ro = (fc->sb_flags & SB_RDONLY) != 0;
	int ret = 0;

	sync_filesystem(sb);
	if (new_ro == sb_rdonly(sb))
		return 0;

	if (new_ro) {
		/* RW -> RO: commit one clean checkpoint before downgrading. */
		vtxfs_commit_begin(sbi);
		sbi->fs->cp_blk.hdr.cp_flags |=
			vtx_le32(VTXFS_CP_FLAG_CLEAN_UNMOUNT);
		ret = vtx_sync_checkpoint(sbi->fs);
		if (ret == 0)
			sbi->fs->read_only = 1;
		vtxfs_commit_end(sbi);
		if (ret)
			return ret;
	} else {
		/* RO -> RW: only if the feature policy allowed RW. */
		if (sbi->policy_ro)
			return -EROFS;
		vtxfs_commit_begin(sbi);
		sbi->fs->read_only = 0;
		vtxfs_commit_end(sbi);
	}
	return 0;
}

static int vtxfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_parse_opts *opts = fc->fs_private;
	struct vtxfs_sb_info *sbi;
	struct inode *root;
	u64 total_blocks;
	int ret;

	(void)opts;
	total_blocks = bdev_nr_bytes(sb->s_bdev) >> VTXFS_BLOCK_SHIFT;
	VTXFS_TRACE_EVENT("mount size_blocks=%llu ro=%d\n",
			  (unsigned long long)total_blocks, sb_rdonly(sb));
	if (total_blocks < VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS) {
		pr_err("vtxfs: device too small (%llu blocks)\n",
		       (unsigned long long)total_blocks);
		return -EINVAL;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	sbi->sb   = sb;
	sbi->bdev = sb->s_bdev;
	sbi->subvol_id = (opts && opts->subvol_id) ? opts->subvol_id
						   : VTXFS_DEFAULT_SUBVOL;
	mutex_init(&sbi->op_lock);
	atomic64_set(&sbi->op_seq, 1);
	sbi->durable_seq = 0;

	/* (1) Engine adapter + mount — đường dẫn RW đã kiểm chứng. */
	sbi->engine_bdev.ops          = &vtx_linux_bdev_ops;
	sbi->engine_bdev.priv         = sb->s_bdev;
	sbi->engine_bdev.total_blocks = total_blocks;
	sbi->fs = vtx_fs_alloc();
	if (!sbi->fs) {
		ret = -ENOMEM;
		goto fail;
	}

	VTXFS_TRACE_EVENT("stage=engine-mount\n");
	ret = vtx_mount(sbi->fs, &sbi->engine_bdev, sb_rdonly(sb) ? 1 : 0);
	if (ret) {
		pr_err("vtxfs: engine mount failed: %d\n", ret);
		goto fail;
	}

	/* Select requested subvolume before VFS publishes mount writeability.
	 * A read-only snapshot must NEVER be exposed as a writable SB:
	 * mmap and page-cache writeback rely on this VFS-level contract. */
	ret = vtx_subvol_switch(sbi->fs, sbi->subvol_id);
	if (ret)
		goto fail_unmount;
	if (vtx_cpu64(sbi->fs->active_subvol.flags) & VTX_SUBVOL_RDONLY) {
		sbi->fs->read_only = 1;
		sb->s_flags |= SB_RDONLY;
	}

	/* VFS keeps unlinked inodes alive until the final evict_inode().
	 * Portable CLI retains immediate-delete semantics for its own use. */
	sbi->fs->defer_unlink = 1;
	if (sbi->fs->read_only == 0) {
		ret = vtx_orphan_recover(sbi->fs, sbi->subvol_id);
		if (ret)
			goto fail_unmount;
	}
	if (sbi->fs->read_only && !sb_rdonly(sb))
		sb->s_flags |= SB_RDONLY;
	sbi->policy_ro = sbi->fs->read_only ? true : false;

	/* (2) Dựng fs_info kernel-native (ctree/subsystem). */
	VTXFS_TRACE_EVENT("stage=native-mount\n");
	ret = vtxfs_mount_native(sb);	/* mount-native.c — lấp sbi->fs_info */
	if (ret)
		goto fail_unmount;
	ret = vtxfs_async_init(sbi->fs_info);		/* async-thread.c */
	if (ret)
		goto fail_native;

	/* (3) Gắn VFS ops + export + xattr + uuid. */
	sb->s_magic     = VTXFS_SB_MAGIC;
	sb->s_op        = &vtxfs_super_ops;
	sb->s_export_op = &vtxfs_export_ops;
	sb->s_xattr     = vtxfs_xattr_handlers;
	sb->s_maxbytes  = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;
	memcpy(&sb->s_uuid, sbi->fs->super_blk.hdr.fsid_uuid, 16);

	/* (4) Giao diện người dùng: sysfs + reaper worker nền. */
	ret = vtxfs_sysfs_init(sbi->fs_info);		/* sysfs.c */
	if (ret)
		goto fail_async;
	ret = vtxfs_reaper_start(sbi->fs_info);		/* reaper.c */
	if (ret)
		goto fail_sysfs;

	/* (5) Root dentry. */
	root = vtxfs_iget(sb, VTXFS_INO_ROOT_DIR);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto fail_reaper;
	}

	VTXFS_TRACE_EVENT("stage=mount-root-inode-ok\n");
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto fail_reaper;
	}
	return 0;

fail_reaper:
	vtxfs_reaper_stop(sbi->fs_info);
fail_sysfs:
	vtxfs_sysfs_exit(sbi->fs_info);
fail_async:
	vtxfs_async_stop(sbi->fs_info);
fail_native:
	vtxfs_mount_native_teardown(sb);
fail_unmount:
	/* A FAILED mount must not publish a clean checkpoint on disk. */
	sbi->fs->read_only = 1;
	vtx_unmount(sbi->fs);
	vtx_fs_free(sbi->fs);
	sbi->fs = NULL;
fail:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

static int vtxfs_get_tree(struct fs_context *fc)
{
	VTXFS_TRACE_ENTER();

	return get_tree_bdev(fc, vtxfs_fill_super);
}

static void vtxfs_free_fc(struct fs_context *fc)
{
	VTXFS_TRACE_ENTER();

	kfree(fc->fs_private);
}

static const struct fs_context_operations vtxfs_fc_ops = {
	.free		= vtxfs_free_fc,
	.parse_param	= vtxfs_parse_param,
	.get_tree	= vtxfs_get_tree,
	.reconfigure	= vtxfs_reconfigure,
};

static int vtxfs_init_fs_context(struct fs_context *fc)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_parse_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return -ENOMEM;
	fc->fs_private = opts;
	fc->ops = &vtxfs_fc_ops;
	return 0;
}

static struct file_system_type vtxfs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "vtxfs",
	.init_fs_context	= vtxfs_init_fs_context,
	.kill_sb		= kill_block_super,
	.fs_flags		= FS_REQUIRES_DEV,
};

static int __init init_vtxfs(void)
{
	VTXFS_TRACE_ENTER();

	int ret = vtxfs_init_inode_cache();

	if (ret)
		return ret;
#ifdef CONFIG_VTXFS_SELFTEST
	ret = vtxfs_run_selftest();
	if (ret) {
		vtxfs_destroy_inode_cache();
		return ret;
	}
#endif
	ret = register_filesystem(&vtxfs_fs_type);
	if (ret)
		vtxfs_destroy_inode_cache();
	return ret;
}


static void __exit exit_vtxfs(void)
{
	VTXFS_TRACE_ENTER();

	unregister_filesystem(&vtxfs_fs_type);
	vtxfs_destroy_inode_cache();
}

module_init(init_vtxfs);
module_exit(exit_vtxfs);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("VTXFS developers");
MODULE_DESCRIPTION("VTXFS v2.1 P0.6 single-device CoW filesystem");
MODULE_ALIAS_FS("vtxfs");
