// SPDX-License-Identifier: GPL-2.0-only
/* fiemap.c — FS_IOC_FIEMAP over engine extent items + VAG pblk mapping. */
#include <linux/fiemap.h>
#include <linux/slab.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

#define VTXFS_FIEMAP_EBUF_CAP \
	(sizeof(struct vtx_extent_item) + 256U * sizeof(__le32) + VTXFS_MAX_INLINE_DATA)

int vtxfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		 u64 start, u64 len)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(inode->i_sb);
	struct vtx_fs *fs = sbi->fs;
	struct vtx_key cur = {0}, found;
	u8 *ebuf;
	size_t bytes;
	u64 ino = VTXFS_I(inode)->vtx_ino;
	loff_t i_size = i_size_read(inode);
	u64 end;
	int first = 1, ret;

	ret = fiemap_prep(inode, fieinfo, start, &len, FIEMAP_FLAG_SYNC);
	if (ret)
		return ret;
	if (len == 0)
		return 0;
	end = start + len;
	if (end < start)	/* overflow */
		end = ~0ULL;

	ebuf = kzalloc(VTXFS_FIEMAP_EBUF_CAP, GFP_KERNEL);
	if (!ebuf)
		return -ENOMEM;

	vtxfs_op_begin(sbi);
	ret = vtx_subvol_switch(fs, sbi->subvol_id);
	if (ret)
		goto out;

	cur.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
	cur.offset = 0;
	for (;;) {
		const struct vtx_extent_item *ext;
		u64 ext_start, ext_end, phys = 0, logical, length;
		u32 flags = 0, phys_blks;

		bytes = VTXFS_FIEMAP_EBUF_CAP;
		ret = vtx_rw_next(&fs->active_fs_tree, &cur, first,
				  &found, ebuf, &bytes);
		if (ret == -ENOENT) {
			ret = 0;
			break;
		}
		if (ret == -ENOBUFS) {
			if (VTX_KEY_GET_OID(vtx_cpu64(found.oid_and_type)) != ino ||
			    VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
			    VTX_TYPE_EXTENT) {
				ret = 0;
				break;
			}
		}
		if (ret)
			break;
		first = 0;
		cur = found;
		if (VTX_KEY_GET_OID(vtx_cpu64(found.oid_and_type)) != ino ||
		    VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_EXTENT)
			break;

		ext = (const struct vtx_extent_item *)ebuf;
		if (!vtx_extent_p0_value_valid(ext, bytes)) {
			ret = -EUCLEAN;
			break;
		}
		ext_start = vtx_cpu64(found.offset);
		/*
		 * The on-disk extent logical_bytes is authoritative for the
		 * mapped FILE range. Padded allocation bytes are not file data.
		 * Like Btrfs fiemap, intersect both requested range and EOF;
		 * never expose a physical prefix before the requested start.
		 */
		length = vtx_cpu32(ext->logical_bytes);
		if (!length || ext_start >= (u64)i_size) {
			ret = length ? 0 : -EUCLEAN;
			if (ret)
				break;
			continue;
		}
		ext_end = ext_start > ~0ULL - length ?
			  ~0ULL : ext_start + length;
		if (ext_end > (u64)i_size)
			ext_end = (u64)i_size;
		if (ext_end <= ext_start) {
			ret = -EUCLEAN;
			break;
		}
		if (!vtx_range_intersect(ext_start, ext_end - ext_start,
					 start, end, &logical, &length)) {
			if (ext_start >= end)
				break;
			continue;
		}
		if (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE) {
			flags = FIEMAP_EXTENT_DATA_INLINE |
				FIEMAP_EXTENT_NOT_ALIGNED;
		} else {
			__u64 pblk = 0;

			phys_blks = vtx_cpu32(ext->physical_blocks);
			/* A single extent may not cross a physical VAG boundary. */
			if (!VTXFS_VALID_VBLK(vtx_cpu64(ext->vblk)) ||
			    phys_blks > VTXFS_VAG_BLOCKS -
					VTXFS_VBLK_TO_AG_OFF(vtx_cpu64(ext->vblk)) ||
			    (u64)phys_blks * VTXFS_BLOCK_SIZE <
					ext_end - ext_start) {
				ret = -EUCLEAN;
				break;
			}
			if (vtx_vag_vblk_to_pblk(&fs->vag,
						vtx_cpu64(ext->vblk),
						&pblk) != 0) {
				ret = -EUCLEAN;
				break;
			}
			phys = (pblk << VTXFS_BLOCK_SHIFT) +
			       (logical - ext_start);
			if (vtx_cpu32(ext->flags) & VTX_EXTENT_SHARED)
				flags |= FIEMAP_EXTENT_SHARED;
		}
		if (ext_end >= (u64)i_size)
			flags |= FIEMAP_EXTENT_LAST;

		ret = fiemap_fill_next_extent(fieinfo, logical, phys,
					      length, flags);
		if (ret)	/* 1 = user array full */
			break;
	}
out:
	vtxfs_op_end(sbi);
	kfree(ebuf);
	return ret < 0 ? ret : 0;
}
