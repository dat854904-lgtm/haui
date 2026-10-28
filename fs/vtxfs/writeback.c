// SPDX-License-Identifier: GPL-2.0-only
/*
 * writeback.c — batched writepages: coalesces up to 32 contiguous dirty
 * folios into a single engine write. Multiple writeback threads may run
 * this concurrently for different inodes; engine serialization remains
 * on op_lock (true multi-threaded engine = P2).
 */
#include <linux/writeback.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

#define VTXFS_WB_MAX_FOLIOS 256U

struct vtxfs_wb_ctx {
	struct super_block *sb;
	struct inode *inode;
	struct folio *folios[VTXFS_WB_MAX_FOLIOS];
	unsigned int nr;
	loff_t start_pos;
	size_t total_len;
	int err;
};

static int vtxfs_wb_flush(struct vtxfs_wb_ctx *ctx,
			  struct writeback_control *wbc)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(ctx->sb);
	loff_t i_size = i_size_read(ctx->inode);
	size_t write_len = ctx->total_len;
	char *buf = NULL;
	unsigned int i;
	int ret = 0;

	if (!ctx->nr)
		return 0;

	for (i = 0; i < ctx->nr; i++)
		folio_start_writeback(ctx->folios[i]);

	if (ctx->start_pos < i_size) {
		write_len = min_t(loff_t, write_len, i_size - ctx->start_pos);
		buf = kvmalloc(write_len, GFP_NOFS);
		if (!buf) {
			ret = -ENOMEM;
		} else {
			size_t copied = 0;

			for (i = 0; i < ctx->nr && copied < write_len; i++) {
				struct folio *f = ctx->folios[i];
				size_t flen = folio_size(f);
				size_t take = min(flen, write_len - copied);
				void *kaddr = kmap_local_folio(f, 0);

				memcpy(buf + copied, kaddr, take);
				kunmap_local(kaddr);
				copied += take;
			}
			vtxfs_op_begin(sbi);
			ret = vtx_write(sbi->fs, sbi->subvol_id,
					VTXFS_I(ctx->inode)->vtx_ino,
					ctx->start_pos, buf, write_len);
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
			kvfree(buf);
		}
	}

	for (i = 0; i < ctx->nr; i++) {
		struct folio *f = ctx->folios[i];

		if (ret) {
			/* Preserve mmap-dirty data even when the engine fails:
			 * clearing writeback without redirty silently loses it. */
			mapping_set_error(f->mapping, ret);
			folio_redirty_for_writepage(wbc, f);
		}
		folio_unlock(f);
		folio_end_writeback(f);
	}
	ctx->nr = 0;
	ctx->total_len = 0;
	if (ret && !ctx->err)
		ctx->err = ret;
	return ret;
}

int vtxfs_writepages(struct address_space *mapping,
		     struct writeback_control *wbc)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_wb_ctx *ctx;
	struct folio *folio = NULL;
	int err = 0, final_err;

	ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
	if (!ctx)
		return -ENOMEM;

	ctx->sb = mapping->host->i_sb;
	ctx->inode = mapping->host;

	while ((folio = writeback_iter(mapping, wbc, folio, &err))) {
		loff_t fpos = folio_pos(folio);
		size_t fsz = folio_size(folio);

		if (ctx->nr &&
		    (fpos != ctx->start_pos + (loff_t)ctx->total_len ||
		     ctx->nr == VTXFS_WB_MAX_FOLIOS ||
		     ctx->total_len + fsz > 1024U * 1024U)) {
			err = vtxfs_wb_flush(ctx, wbc);
			if (err) {
				folio_redirty_for_writepage(wbc, folio);
				folio_unlock(folio);
				/* Ensure writeback_iter releases all batch references. */
				continue;
			}
		}
		if (!ctx->nr)
			ctx->start_pos = fpos;
		ctx->folios[ctx->nr++] = folio;
		ctx->total_len += fsz;
	}
	vtxfs_wb_flush(ctx, wbc);
	final_err = err ? err : ctx->err;
	kfree(ctx);
	return final_err;
}
