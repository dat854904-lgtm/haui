// SPDX-License-Identifier: GPL-2.0-only
/*
 * VTXFS v2.1 P0.6 — file_operations, address_space_operations, readdir,
 * fallocate, copy_file_range, DIO dispatch and fsync bindings.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/highmem.h>
#include <linux/writeback.h>
#include <linux/falloc.h>
#include <linux/splice.h>
#include <linux/fs.h>

#include "vtxfs_drv.h"
#include "vtxfs_trace.h"
#include "vtxfs_readahead.h"

#define VTXFS_READDIR_BATCH 16U

static unsigned char vtxfs_dtype(u8 ft)
{
	VTXFS_TRACE_ENTER();

	switch (ft) {
	case VTX_FT_REG:     return DT_REG;
	case VTX_FT_DIR:     return DT_DIR;
	case VTX_FT_CHRDEV:  return DT_CHR;
	case VTX_FT_BLKDEV:  return DT_BLK;
	case VTX_FT_FIFO:    return DT_FIFO;
	case VTX_FT_SOCK:    return DT_SOCK;
	case VTX_FT_SYMLINK: return DT_LNK;
	default:             return DT_UNKNOWN;
	}
}

/* ---------------- address_space_operations ---------------- */

static int vtxfs_read_folio(struct file *file, struct folio *folio)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = folio->mapping->host;
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	char *kaddr;
	size_t got = 0;
	loff_t pos = folio_pos(folio);
	int ret;

	VTXFS_TRACE_EVENT("folio-read ino=%llu pos=%lld len=%zu\n",
			  (unsigned long long)inode->i_ino, (long long)pos, (size_t)folio_size(folio));
	(void)file;
	kaddr = kmap_local_folio(folio, 0);
	vtxfs_op_begin(sbi);
	ret = vtx_read(sbi->fs, vtxfs_active_subvol(inode->i_sb),
		       VTXFS_I(inode)->vtx_ino, (u64)pos,
		       kaddr, folio_size(folio), &got);
	vtxfs_op_end(sbi);
	if (ret) {
		kunmap_local(kaddr);
		folio_unlock(folio);
		return ret;
	}
	if (got < folio_size(folio))
		memset(kaddr + got, 0, folio_size(folio) - got);
	kunmap_local(kaddr);
	flush_dcache_folio(folio);
	folio_mark_uptodate(folio);
	folio_unlock(folio);
	return 0;
}

static int vtxfs_write_begin(const struct kiocb *iocb,
			     struct address_space *mapping,
			     loff_t pos, unsigned int len,
			     struct folio **foliop, void **fsdata)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = mapping->host;
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	struct folio *folio;
	int ret = 0;

	(void)iocb;
	(void)len;
	(void)fsdata;
	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT,
				    FGP_WRITEBEGIN, mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	if (!folio_test_uptodate(folio)) {
		loff_t base = folio_pos(folio);
		size_t fsize = folio_size(folio);
		char *kaddr = kmap_local_folio(folio, 0);
		size_t got = 0;

		vtxfs_op_begin(sbi);
		ret = vtx_read(sbi->fs, vtxfs_active_subvol(inode->i_sb),
			       VTXFS_I(inode)->vtx_ino, (u64)base,
			       kaddr, fsize, &got);
		vtxfs_op_end(sbi);
		if (ret == 0) {
			if (got < fsize)
				memset(kaddr + got, 0, fsize - got);
			folio_mark_uptodate(folio);
		}
		kunmap_local(kaddr);
		if (ret) {
			folio_unlock(folio);
			folio_put(folio);
			return ret;
		}
	}

	*foliop = folio;
	return 0;
}

static int vtxfs_write_end(const struct kiocb *iocb,
			   struct address_space *mapping,
			   loff_t pos, unsigned int len, unsigned int copied,
			   struct folio *folio, void *fsdata)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = mapping->host;

	(void)iocb;
	(void)len;
	(void)fsdata;

	if (copied) {
		loff_t end = pos + copied;

		if (end > i_size_read(inode))
			i_size_write(inode, end);
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
		mark_inode_dirty(inode);
		folio_mark_uptodate(folio);
		folio_mark_dirty(folio);
	}
	folio_unlock(folio);
	folio_put(folio);
	return (int)copied;
}

const struct address_space_operations vtxfs_aops = {
	.read_folio	= vtxfs_read_folio,
	.readahead	= vtxfs_readahead,
	.writepages	= vtxfs_writepages,
	.write_begin	= vtxfs_write_begin,
	.write_end	= vtxfs_write_end,
	.dirty_folio	= filemap_dirty_folio,
};

/* ---------------- fsync ---------------- */

static int vtxfs_file_fsync(struct file *file, loff_t start, loff_t end,
			    int datasync)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = file->f_mapping->host;
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	int ret;

	u64 want;

	(void)datasync;
	ret = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (ret)
		return ret;
	if (sb_rdonly(inode->i_sb))
		return 0;

	/* No engine operation since the last successful checkpoint?  Do not
	 * rotate CPD or flush again. Every engine op_end advances op_seq,
	 * including reads, so this skip is deliberately conservative. */
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

/* ---------------- readdir ---------------- */

static int vtxfs_readdir(struct file *file, struct dir_context *ctx)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = file_inode(file);
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	struct vtx_dirent *ents;
	u64 cookie;
	int ret = 0;

	VTXFS_TRACE_EVENT("readdir ino=%llu pos=%lld\n",
			  (unsigned long long)inode->i_ino, (long long)ctx->pos);
	if (!dir_emit_dots(file, ctx))
		return 0;

	ents = kmalloc_array(VTXFS_READDIR_BATCH, sizeof(*ents), GFP_KERNEL);
	if (!ents)
		return -ENOMEM;

	/* ctx->pos 0/1/2 are reserved for "." and ".."; engine dir_seq
	 * cookies are always >= 256, so they never collide. */
	cookie = (ctx->pos <= 2) ? 0 : (u64)ctx->pos;

	for (;;) {
		size_t count = 0, i;

		vtxfs_op_begin(sbi);
		ret = vtx_readdir(sbi->fs, vtxfs_active_subvol(inode->i_sb),
				  VTXFS_I(inode)->vtx_ino, &cookie,
				  ents, VTXFS_READDIR_BATCH, &count);
		vtxfs_op_end(sbi);
		if (ret || count == 0)
			break;

		for (i = 0; i < count; i++) {
			if (!dir_emit(ctx, ents[i].name,
				      (int)strnlen(ents[i].name, VTXFS_NAME_MAX),
				      ents[i].ino,
				      vtxfs_dtype(ents[i].file_type)))
				goto out;
			ctx->pos = (loff_t)ents[i].dir_seq;
			cookie = ents[i].dir_seq;
		}
	}
out:
	kfree(ents);
	return ret;
}

/* ---------------- DIO dispatch, fallocate, copy_file_range ---------------- */

static ssize_t vtxfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	VTXFS_TRACE_ENTER();

	if (iocb->ki_flags & IOCB_DIRECT) {
		struct inode *inode = file_inode(iocb->ki_filp);
		ssize_t ret;

		inode_lock_shared(inode);
		ret = vtxfs_direct_read(iocb, to);
		inode_unlock_shared(inode);
		return ret;
	}
	return generic_file_read_iter(iocb, to);
}

static ssize_t vtxfs_file_write_iter(struct kiocb *iocb,
				     struct iov_iter *from)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (!(iocb->ki_flags & IOCB_DIRECT))
		return generic_file_write_iter(iocb, from);

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;
	ret = file_modified(iocb->ki_filp);
	if (ret)
		goto out_unlock;
	ret = vtxfs_direct_write(iocb, from);
out_unlock:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

/*
 * fallocate — P0 EMULATION: engine has no PREALLOC yet, so space is
 * reserved by zero-filling through the engine (checksummed CoW write).
 */
static long vtxfs_fallocate(struct file *file, int mode,
			    loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	loff_t end, chunk_off, chunk_len, orig_size;
	int ret = 0;

	if (mode)
		return -EOPNOTSUPP; /* no KEEP_SIZE or HOLE_PUNCH yet */
	if (offset < 0 || len <= 0 || offset > MAX_LFS_FILESIZE - len)
		return -EINVAL;
	if (!IS_ALIGNED((u64)offset | (u64)len, VTXFS_BLOCK_SIZE))
		return -EOPNOTSUPP;
	end = offset + len;
	inode_lock(inode);
	ret = filemap_write_and_wait_range(inode->i_mapping, offset, end - 1);
	if (ret)
		goto out;

	orig_size = i_size_read(inode);

	for (chunk_off = offset; chunk_off < end; chunk_off += chunk_len) {
		chunk_len = min_t(loff_t, end - chunk_off, 1024 * 1024);
		vtxfs_op_begin(sbi);
		ret = vtx_fallocate_eager(sbi->fs, sbi->subvol_id,
					  VTXFS_I(inode)->vtx_ino, chunk_off, chunk_len);
		if (ret == 0 &&
		    (sbi->fs->num_pending_data >= (VTX_MAX_PENDING_DATA * 3U / 4U) ||
		     sbi->fs->num_delayed_refs >= (VTX_MAX_DELAYED_REFS * 3U / 4U))) {
			ret = vtx_sync_checkpoint(sbi->fs);
		}
		vtxfs_op_end(sbi);
		if (ret)
			break;
	}
	if (!ret) {
		if (end > orig_size)
			i_size_write(inode, end);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
	} else if (chunk_off > offset) {
		/* Partial allocation failed: rollback newly allocated extents
		 * to preserve filesystem space and avoid leaked trailing extents. */
		vtxfs_op_begin(sbi);
		(void)vtx_truncate(sbi->fs, sbi->subvol_id,
				   VTXFS_I(inode)->vtx_ino, (u64)orig_size);
		vtxfs_op_end(sbi);
	}
out:
	inode_unlock(inode);
	return ret;
}

/*
 * remap_file_range — Native CoW reflink (FICLONE / FICLONERANGE).
 * Shares block-aligned extents instantaneously without copying data.
 */
static loff_t vtxfs_remap_file_range(struct file *file_in, loff_t pos_in,
				     struct file *file_out, loff_t pos_out,
				     loff_t len, unsigned int remap_flags)
{
	struct inode *inode_in = file_inode(file_in);
	struct inode *inode_out = file_inode(file_out);
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode_in->i_sb);
	int ret;

	if (remap_flags & ~(REMAP_FILE_DEDUP))
		return -EINVAL;
	if (inode_in->i_sb != inode_out->i_sb)
		return -EXDEV;
	if (sb_rdonly(inode_out->i_sb))
		return -EROFS;

	ret = generic_remap_file_range_prep(file_in, pos_in, file_out, pos_out,
					    &len, remap_flags);
	if (ret < 0 || len == 0)
		return ret;

	/* Flush dirty pagecache on source and destination */
	ret = filemap_write_and_wait_range(inode_in->i_mapping, pos_in,
					   pos_in + len - 1);
	if (ret)
		return ret;
	ret = filemap_write_and_wait_range(inode_out->i_mapping, pos_out,
					   pos_out + len - 1);
	if (ret)
		return ret;
	truncate_inode_pages_range(inode_out->i_mapping, pos_out,
				   pos_out + len - 1);

	vtxfs_op_begin(sbi);
	ret = vtx_reflink(sbi->fs, sbi->subvol_id,
			  VTXFS_I(inode_in)->vtx_ino, (u64)pos_in,
			  VTXFS_I(inode_out)->vtx_ino, (u64)pos_out,
			  (u64)len);
	vtxfs_op_end(sbi);
	if (ret)
		return ret;

	if (pos_out + len > i_size_read(inode_out))
		i_size_write(inode_out, pos_out + len);
	inode_set_mtime_to_ts(inode_out, inode_set_ctime_current(inode_out));
	mark_inode_dirty(inode_out);
	return len;
}

static ssize_t vtxfs_copy_file_range(struct file *file_in, loff_t pos_in,
				     struct file *file_out, loff_t pos_out,
				     size_t len, unsigned int flags)
{
	struct inode *inode_in = file_inode(file_in);
	struct inode *inode_out = file_inode(file_out);
	ssize_t ret;

	if (flags)
		return -EINVAL;

	/* Try fast zero-copy CoW reflink first if source and dest are on same fs */
	if (inode_in->i_sb == inode_out->i_sb &&
	    (pos_in & (VTXFS_BLOCK_SIZE - 1)) == 0 &&
	    (pos_out & (VTXFS_BLOCK_SIZE - 1)) == 0) {
		ret = vtxfs_remap_file_range(file_in, pos_in, file_out, pos_out,
					     len, 0);
		if (ret > 0)
			return ret;
	}

	return splice_copy_file_range(file_in, pos_in, file_out, pos_out, len);
}

/* ---------------- file_operations ---------------- */

static int vtxfs_file_open(struct inode *inode, struct file *file)
{
	int ret = generic_file_open(inode, file);

	/* The VFS refuses O_DIRECT at open() without FMODE_CAN_ODIRECT.
	 * vtxfs_file_read_iter/write_iter already implement checked DIO. */
	if (!ret)
		file->f_mode |= FMODE_CAN_ODIRECT;
	return ret;
}

const struct file_operations vtxfs_file_fops = {
	.owner		= THIS_MODULE,
	.open		= vtxfs_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= vtxfs_file_read_iter,
	.write_iter	= vtxfs_file_write_iter,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.mmap		= generic_file_mmap,
	.fsync		= vtxfs_file_fsync,
	.fallocate	= vtxfs_fallocate,
	.copy_file_range = vtxfs_copy_file_range,
	.remap_file_range = vtxfs_remap_file_range,
	.unlocked_ioctl	= vtxfs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

const struct file_operations vtxfs_dir_fops = {
	.owner		= THIS_MODULE,
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= vtxfs_readdir,
	.fsync		= vtxfs_file_fsync,
	.unlocked_ioctl	= vtxfs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};
