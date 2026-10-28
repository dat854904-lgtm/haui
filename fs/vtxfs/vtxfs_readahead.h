/* SPDX-License-Identifier: GPL-2.0-only */
/* Readahead under the existing op_lock, preserving engine CRC checks.
 * Failed reads never get marked uptodate; later read_folio may retry.
 */
#ifndef VTXFS_READAHEAD_H
#define VTXFS_READAHEAD_H
#define VTXFS_RA_BATCH  (128U * 1024U)
static void vtxfs_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->mapping->host;
	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	struct folio *f;
	char *buf = kvmalloc(VTXFS_RA_BATCH, GFP_NOFS);
	loff_t cached_pos = -1;
	size_t got = 0;
	int ret = -ENOMEM;

	while ((f = readahead_folio(rac))) {
		loff_t pos = folio_pos(f);
		size_t len = folio_size(f);
		if (buf && len <= VTXFS_RA_BATCH &&
		    (cached_pos < 0 || pos < cached_pos ||
		     pos - cached_pos > VTXFS_RA_BATCH - len)) {
			got = 0;
			vtxfs_op_begin(sbi);
			ret = vtx_read(sbi->fs, vtxfs_active_subvol(inode->i_sb),
				       VTXFS_I(inode)->vtx_ino, pos,
				       buf, VTXFS_RA_BATCH, &got);
			vtxfs_op_end(sbi);
			cached_pos = pos;
		}
		if (buf && len <= VTXFS_RA_BATCH && !ret) {
			size_t off = (size_t)(pos - cached_pos);
			size_t n = got > off ? min(len, got - off) : 0;
			if (n)
				memcpy_to_folio(f, 0, buf + off, n);
			if (n < len)
				folio_zero_segment(f, n, len);
			flush_dcache_folio(f);
			folio_mark_uptodate(f);
		}
		folio_unlock(f);
	}
	kvfree(buf);
}
#endif
