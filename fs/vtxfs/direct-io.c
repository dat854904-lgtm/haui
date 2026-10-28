// SPDX-License-Identifier: GPL-2.0-only
/*
 * direct-io.c — O_DIRECT read path.
 *
 * Direct I/O uses engine-verified bounce buffers.  Writes bypass the page
 * cache but still go through vtx_write() and the ordered checkpoint path;
 * raw bios here would bypass CoW/refcount/checksum metadata.
 */
#include <linux/uio.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

#define VTXFS_DIO_CHUNK (1024U * 1024U)

ssize_t vtxfs_direct_read(struct kiocb *iocb, struct iov_iter *to)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = file_inode(iocb->ki_filp);
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	loff_t pos = iocb->ki_pos;
	loff_t size = i_size_read(inode);
	size_t total = 0;
	unsigned int alignment = bdev_logical_block_size(sbi->bdev);
	u8 *buf;
	int ret = 0;

	if (pos < 0 || iov_iter_count(to) > (size_t)(LLONG_MAX - pos))
		return -EINVAL;
	if (!iov_iter_count(to) || pos >= size)
		return 0;
	/* Match device block alignment, as write path and Btrfs DIO do. */
	if (!alignment)
		alignment = SECTOR_SIZE;
	if (!IS_ALIGNED(pos, alignment) ||
	    !IS_ALIGNED(iov_iter_count(to), alignment) ||
	    (iov_iter_alignment(to) & (alignment - 1U)))
		return -EINVAL;

	buf = kmalloc(VTXFS_DIO_CHUNK, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (iov_iter_count(to) && pos < size) {
		size_t want = min3(iov_iter_count(to),
				   (size_t)VTXFS_DIO_CHUNK,
				   (size_t)(size - pos));
		size_t got = 0;

		vtxfs_op_begin(sbi);
		ret = vtx_read(sbi->fs, sbi->subvol_id,
			       VTXFS_I(inode)->vtx_ino, pos, buf, want, &got);
		vtxfs_op_end(sbi);
		if (ret)
			break;
		if (!got)
			break;
		{
			size_t copied = copy_to_iter(buf, got, to);

			/* A partial user copy has consumed bytes. Return that exact
			 * count rather than silently losing progress (POSIX read). */
			pos += copied;
			total += copied;
			if (copied != got) {
				ret = -EFAULT;
				break;
			}
		}
		if (got < want)
			break;	/* EOF mid-chunk */
	}
	kfree(buf);

	if (ret && !total)
		return ret;
	iocb->ki_pos = pos;
	return (ssize_t)total;
}

ssize_t vtxfs_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = file_inode(iocb->ki_filp);
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	loff_t pos = iocb->ki_pos;
	size_t requested = iov_iter_count(from);
	size_t total = 0;
	unsigned int alignment = bdev_logical_block_size(sbi->bdev);
	pgoff_t first_page, last_page;
	u8 *buf;
	int ret = 0, sync_ret;

	if (!requested)
		return 0;
	if (iocb->ki_flags & IOCB_NOWAIT)
		return -EOPNOTSUPP;
	if (pos < 0 || requested > (size_t)(LLONG_MAX - pos))
		return -EINVAL;
	if (!alignment)
		alignment = SECTOR_SIZE;
	if (!IS_ALIGNED(pos, alignment) ||
	    !IS_ALIGNED(requested, alignment) ||
	    (iov_iter_alignment(from) & (alignment - 1U)))
		return -EINVAL;
	if (sb_rdonly(inode->i_sb))
		return -EROFS;

	first_page = (pgoff_t)((u64)pos >> PAGE_SHIFT);
	last_page = (pgoff_t)(((u64)pos + requested - 1U) >> PAGE_SHIFT);
	ret = filemap_write_and_wait_range(mapping, pos,
					   pos + (loff_t)requested - 1);
	if (ret)
		return ret;
	ret = invalidate_inode_pages2_range(mapping, first_page, last_page);
	if (ret)
		return ret;

	buf = kmalloc(VTXFS_DIO_CHUNK, GFP_NOFS);
	if (!buf)
		return -ENOMEM;

	while (iov_iter_count(from)) {
		size_t want = min_t(size_t, VTXFS_DIO_CHUNK,
				    iov_iter_count(from));
		size_t copied = copy_from_iter(buf, want, from);

		if (!copied) {
			ret = -EFAULT;
			break;
		}
		vtxfs_op_begin(sbi);
		ret = vtx_write(sbi->fs, sbi->subvol_id,
				VTXFS_I(inode)->vtx_ino, (u64)pos, buf, copied);
		if (ret == 0 &&
		    (sbi->fs->num_pending_data >= (VTX_MAX_PENDING_DATA * 3U / 4U) ||
		     sbi->fs->num_delayed_refs >= (VTX_MAX_DELAYED_REFS * 3U / 4U))) {
			ret = vtx_sync_checkpoint(sbi->fs);
			if (ret == 0 && sbi->fs->committed_xid > 1ULL)
				(void)vtx_sfq_promote(sbi->fs,
						      sbi->fs->committed_xid - 1ULL,
						      NULL);
		}
		vtxfs_op_end(sbi);
		if (ret) {
			iov_iter_revert(from, copied);
			break;
		}
		pos += (loff_t)copied;
		total += copied;
		if (copied != want) {
			ret = -EFAULT;
			break;
		}
	}

	/* Submit data and atomically publish the extent/checksum metadata. */
	if (total && ret == 0) {
		vtxfs_commit_begin(sbi);
		sync_ret = vtx_sync_checkpoint(sbi->fs);
		vtxfs_commit_end(sbi);
		if (sync_ret)
			ret = sync_ret;
	}
	if (total && (!ret || ret == -EFAULT)) {
		(void)invalidate_inode_pages2_range(mapping, first_page, last_page);
		if (pos > i_size_read(inode))
			i_size_write(inode, pos);
		iocb->ki_pos = pos;
	}
	kfree(buf);
	if (ret && (!total || ret != -EFAULT))
		return ret;
	return (ssize_t)total;
}
