// SPDX-License-Identifier: GPL-2.0-only
/*
 * vag-alloc.c — kernel-side VAG directory load + physical block allocator.
 * Implements the vtxfs_alloc_ops hook used by extent-tree.c / ctree.c so
 * tree-block and data allocation go through the authoritative VAG bitmaps.
 */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"

int vtxfs_vag_vblk_to_pblk(struct vtxfs_vag_dir *vag, u64 vblk, u64 *pblk)
{
	VTXFS_TRACE_ENTER();

	u32 vag_id, ag_off;
	u64 start;

	if (!vag || !pblk || vtx_vblk_decode(vblk, &vag_id, &ag_off) != 0 ||
	    vag_id >= vag->total_vags)
		return -ERANGE;
	start = vtx_cpu64(vag->descs[vag_id].start_pblk);
	if (start < VTXFS_FIRST_VAG_PBLK)
		return -EUCLEAN;
	*pblk = start + ag_off;
	return 0;
}

int vtxfs_vag_run_to_pblk(struct vtxfs_vag_dir *vag, u64 vblk,
                          u32 blocks, u64 *pblk)
{
	VTXFS_TRACE_ENTER();

    u32 vag_id, offset;
    int ret;

    if (!blocks || !vag || !pblk)
        return -EINVAL;
    if (vtx_vblk_decode(vblk, &vag_id, &offset) || vag_id >= vag->total_vags)
        return -ERANGE;
    if (blocks > VTXFS_VAG_BLOCKS - offset)
        return -ERANGE;
    ret = vtxfs_vag_vblk_to_pblk(vag, vblk, pblk);
    return ret;
}

int vtxfs_vag_test_bit(struct vtxfs_vag_dir *vag, u64 vblk)
{
	VTXFS_TRACE_ENTER();

	u32 vag_id, ag_off;

	if (!vag || vtx_vblk_decode(vblk, &vag_id, &ag_off) != 0 ||
	    vag_id >= vag->total_vags)
		return -ERANGE;
	return (vag->bitmaps[vag_id][ag_off >> 3] >> (ag_off & 7U)) & 1;
}

int vtxfs_vag_alloc_blocks(struct vtxfs_vag_dir *vag, u32 count,
			   u64 *start_vblk)
{
	VTXFS_TRACE_ENTER();

	u32 v, off, run;

	VTXFS_TRACE_EVENT("alloc-vag blocks=%u\n", count);
	if (!vag || !count || count > VTXFS_VAG_BLOCKS || !start_vblk)
		return -EINVAL;
	mutex_lock(&vag->lock);
	for (v = 0; v < vag->total_vags; v++) {
		u8 *bm = vag->bitmaps[v];
		u32 start_off = (v == 0) ? 1U : 0U; /* VBLK 0 reserved */

		if (vtx_cpu32(vag->descs[v].free_blocks) < count)
			continue;
		run = 0;
		for (off = start_off; off < VTXFS_VAG_BLOCKS; ) {
			/* Fast-forward over fully allocated 64-block words when no run started */
			if (run == 0 && (off & 63U) == 0 && off + 64U <= VTXFS_VAG_BLOCKS) {
				u64 wval;
				memcpy(&wval, bm + (off >> 3), sizeof(wval));
				if (wval == ~0ULL) {
					off += 64U;
					continue;
				}
			}
			if (!((bm[off >> 3] >> (off & 7U)) & 1)) {
				if (++run == count) {
					u32 first = off + 1U - count;
					u64 vblk = VTXFS_MAKE_VBLK(v, first);
					u32 i;

					for (i = 0; i < count; i++)
						bm[(first + i) >> 3] |=
							1U << ((first + i) & 7U);
					vag->descs[v].free_blocks = vtx_le32(
						vtx_cpu32(vag->descs[v].free_blocks)
						- count);
					mutex_unlock(&vag->lock);
					*start_vblk = vblk;
					return 0;
				}
			} else {
				run = 0;
			}
			off++;
		}
	}
	mutex_unlock(&vag->lock);
	VTXFS_TRACE_EVENT("alloc-vag ENOSPC blocks=%u\n", count);
	return -ENOSPC;
}

int vtxfs_vag_free_blocks(struct vtxfs_vag_dir *vag, u64 start_vblk,
			  u32 count)
{
	VTXFS_TRACE_ENTER();

	u32 vag_id, ag_off, i, free_count;

	if (!vag || !count ||
	    vtx_vblk_decode(start_vblk, &vag_id, &ag_off) != 0 ||
	    vag_id >= vag->total_vags ||
	    count > VTXFS_VAG_BLOCKS - ag_off ||
	    (vag_id == 0 && ag_off == 0))
		return -EINVAL;

	mutex_lock(&vag->lock);
	if (!vag->bitmaps || !vag->bitmaps[vag_id] || !vag->descs) {
		mutex_unlock(&vag->lock);
		return -EUCLEAN;
	}
	free_count = vtx_cpu32(vag->descs[vag_id].free_blocks);
	if (free_count > VTXFS_VAG_BLOCKS - count) {
		mutex_unlock(&vag->lock);
		return -EUCLEAN;
	}
	/* Preflight the ENTIRE range before changing any bit. A double-free
	 * must not be silently accepted or partially alter the bitmap.
	 * Allocation/reuse remains governed by the authoritative REF/SFQ. */
	for (i = 0; i < count; i++) {
		u32 bit = ag_off + i;

		if (!(vag->bitmaps[vag_id][bit >> 3] &
		      (1U << (bit & 7U)))) {
			mutex_unlock(&vag->lock);
			return -EUCLEAN;
		}
	}
	for (i = 0; i < count; i++) {
		u32 bit = ag_off + i;

		vag->bitmaps[vag_id][bit >> 3] &=
			~(1U << (bit & 7U));
	}
	vag->descs[vag_id].free_blocks =
		vtx_le32(free_count + count);
	mutex_unlock(&vag->lock);
	return 0;
}

/* ---- alloc_ops hook (fs_info->alloc_ops) ---- */
static int vag_op_alloc(struct vtxfs_fs_info *fs, u32 blocks, u64 *out)
{
	VTXFS_TRACE_ENTER();

	if (fs->shadow_only)
		return -EROFS;
	if (!fs->vag)
		return -EOPNOTSUPP;
	return vtxfs_vag_alloc_blocks(fs->vag, blocks, out);
}

static int vag_op_free(struct vtxfs_fs_info *fs, u64 start_vblk, u32 blocks)
{
	VTXFS_TRACE_ENTER();

	if (fs->shadow_only)
		return -EROFS;
	if (!fs->vag)
		return -EOPNOTSUPP;
	return vtxfs_vag_free_blocks(fs->vag, start_vblk, blocks);
}

const struct vtxfs_alloc_ops vtxfs_vag_alloc_ops = {
	.alloc_blocks = vag_op_alloc,
	.free_blocks  = vag_op_free,
};

/* ---- load from a checkpoint phys_ref ---- */
int vtxfs_vag_load(struct block_device *bdev, const struct vtx_phys_ref *ref,
		   u64 total_blocks, const u8 metadata_uuid[VTXFS_UUID_SIZE],
		   struct vtxfs_vag_dir **out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_vag_dir *vag;
	u64 root_pblk, leaf_pblk, total_vags;
	u32 i;
	int ret;

	if (!bdev || !ref || !out || !vtx_phys_ref_valid(ref, total_blocks, 0))
		return -EUCLEAN;
	root_pblk = vtx_cpu64(ref->pblk);

	vag = kzalloc(sizeof(*vag), GFP_KERNEL);
	if (!vag)
		return -ENOMEM;
	mutex_init(&vag->lock);
	vag->base_pblk = root_pblk;

	ret = vtxfs_bdev_read_blocks(bdev, root_pblk, 1, &vag->root);
	if (ret)
		goto err;
	if (vtx_cpu32(vag->root.hdr.magic) != VTXFS_VAG_DIR_MAGIC ||
	    !vtx_vag_root_checksum_verify(&vag->root, metadata_uuid)) {
		ret = -EUCLEAN;
		goto err;
	}
	total_vags = vtx_cpu64(vag->root.hdr.total_vags);
	if (total_vags == 0 || total_vags > VTX_MAX_SUPPORTED_VAGS ||
	    vtx_cpu16(vag->root.entries[0].valid_count) != total_vags) {
		ret = -EUCLEAN;
		goto err;
	}
	vag->total_vags = (u32)total_vags;

	vag->descs = vzalloc(sizeof(*vag->descs) * VTXFS_VAG_LEAF_ENTRIES);
	vag->bitmaps = kcalloc(vag->total_vags, sizeof(u8 *), GFP_KERNEL);
	if (!vag->descs || !vag->bitmaps) {
		ret = -ENOMEM;
		goto err;
	}

	leaf_pblk = vtx_cpu64(vag->root.entries[0].start_pblk);
	if (leaf_pblk >= total_blocks || 16ULL > total_blocks - leaf_pblk) {
		ret = -EUCLEAN;
		goto err;
	}
	ret = vtxfs_bdev_read_blocks(bdev, leaf_pblk, 16, vag->descs);
	if (ret)
		goto err;
	if (vtx_crc32c(vag->descs, sizeof(*vag->descs) * VTXFS_VAG_LEAF_ENTRIES)
	    != vtx_cpu32(vag->root.entries[0].leaf_crc32c)) {
		ret = -EUCLEAN;
		goto err;
	}

	for (i = 0; i < vag->total_vags; i++) {
		u64 bm_pblk = vtx_cpu64(vag->descs[i].bitmap_pblk);

		if (vtx_cpu64(vag->descs[i].start_pblk) !=
		    VTXFS_FIRST_VAG_PBLK + (u64)i * VTXFS_VAG_BLOCKS ||
		    bm_pblk >= total_blocks) {
			ret = -EUCLEAN;
			goto err;
		}
		vag->bitmaps[i] = vzalloc(VTXFS_BLOCK_SIZE);
		if (!vag->bitmaps[i]) {
			ret = -ENOMEM;
			goto err;
		}
		ret = vtxfs_bdev_read_blocks(bdev, bm_pblk, 1,
					     vag->bitmaps[i]);
		if (ret)
			goto err;
		if (vtx_crc32c(vag->bitmaps[i], VTXFS_BLOCK_SIZE) !=
		    vtx_cpu32(vag->descs[i].bitmap_crc32c)) {
			ret = -EUCLEAN;
			goto err;
		}
	}
	*out = vag;
	return 0;
err:
	vtxfs_vag_free(vag);
	return ret;
}

void vtxfs_vag_free(struct vtxfs_vag_dir *vag)
{
	VTXFS_TRACE_ENTER();

	u32 i;

	if (!vag)
		return;
	if (vag->bitmaps) {
		for (i = 0; i < vag->total_vags; i++)
			vfree(vag->bitmaps[i]);
		kfree(vag->bitmaps);
	}
	vfree(vag->descs);
	kfree(vag);
}
EXPORT_SYMBOL_GPL(vtxfs_vag_free);
