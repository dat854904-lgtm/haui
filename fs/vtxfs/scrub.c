// SPDX-License-Identifier: GPL-2.0-only
/*
 * scrub.c — verify data extents by walking subvolume B+trees in fs->roots
 * and re-checking per-block Castagnoli CRC32C against the authoritative
 * vtx_extent_item. Verifies B-tree metadata nodes during traversal.
 * Progress is tracked in a durable vtx_scrub_state.
 */
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/ratelimit.h>
#include <linux/minmax.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_trace.h"
#include "vtxfs_csum.h"
#include "vtxfs_backref.h"

int scrub_verify_extent(struct vtxfs_scrub_ctx *sc,
			const struct vtx_extent_item *ext)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = sc ? sc->fs : NULL;
	u64 vblk;
	u32 blocks, flags;
	u16 csum_count;
	const __le32 *tail;
	u32 b;
	u8 *blk;
	int ret = 0;

	if (!sc || !fs || !ext)
		return -EINVAL;

	flags = vtx_cpu32(ext->flags);
	if (flags & VTX_EXTENT_INLINE)
		return 0;

	vblk = vtx_cpu64(ext->vblk);
	blocks = vtx_cpu32(ext->physical_blocks);
	csum_count = vtx_cpu16(ext->csum_count);
	tail = (const __le32 *)ext->tail;

	if (!blocks || blocks > VTXFS_MAX_EXTENT_BLOCKS)
		return 0;

	blk = kvmalloc(VTXFS_BLOCK_SIZE, GFP_KERNEL);
	if (!blk)
		return -ENOMEM;

	for (b = 0; b < blocks; b++) {
		u64 cur_vblk = vblk + b;
		u64 pblk = 0;
		u32 actual_crc, expected_crc;

		if (sc->task && kthread_should_stop()) {
			ret = -EINTR;
			break;
		}

		sc->state.last_checked_vblk = cpu_to_le64(cur_vblk);

		if (fs->vag) {
			ret = vtxfs_vag_vblk_to_pblk(fs->vag, cur_vblk, &pblk);
			if (ret) {
				sc->state.error_count =
					cpu_to_le32(vtx_cpu32(sc->state.error_count) + 1);
				pr_warn("VTXFS: scrub VAG translation failed at vblk=%llu (ret=%d)\n",
					(unsigned long long)cur_vblk, ret);
				break;
			}
		} else {
			pblk = cur_vblk;
		}

		if (fs->read_block_fn) {
			ret = fs->read_block_fn(fs, pblk, blk);
		} else if (fs->bdev) {
			ret = vtxfs_bdev_read_blocks(fs->bdev, pblk, 1, blk);
		} else {
			ret = -ENODEV;
		}

		if (ret) {
			sc->state.error_count =
				cpu_to_le32(vtx_cpu32(sc->state.error_count) + 1);
			pr_warn("VTXFS: scrub read error at vblk=%llu pblk=%llu (ret=%d)\n",
				(unsigned long long)cur_vblk, (unsigned long long)pblk, ret);
			break;
		}

		actual_crc = vtx_crc32c(blk, VTXFS_BLOCK_SIZE);
		expected_crc = (b < csum_count) ? vtx_cpu32(tail[b]) : 0;

		if (actual_crc == expected_crc) {
			u64 verified = vtx_cpu64(sc->state.verified_blocks);
			sc->state.verified_blocks = cpu_to_le64(verified + 1);
		} else {
			sc->state.error_count =
				cpu_to_le32(vtx_cpu32(sc->state.error_count) + 1);
			pr_warn("VTXFS: scrub CRC mismatch at vblk=%llu pblk=%llu: expected 0x%08x, got 0x%08x\n",
				(unsigned long long)cur_vblk, (unsigned long long)pblk,
				expected_crc, actual_crc);
		}
	}

	kvfree(blk);
	return ret;
}

int vtxfs_scrub_verify_extent(struct vtxfs_scrub_ctx *sc,
			      const struct vtx_extent_item *ext)
{
	VTXFS_TRACE_ENTER();
	return scrub_verify_extent(sc, ext);
}

static void scrub_verify_eb_checksum(struct vtxfs_scrub_ctx *sc,
				     const struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();
	if (!eb || !eb->hdr)
		return;
	if (!vtx_node_slots_valid(eb->hdr, eb->node_size) ||
	    !vtx_node_checksum_verify(eb->hdr, eb->node_size, sc->fs->metadata_uuid)) {
		u32 errs = vtx_cpu32(sc->state.error_count);
		sc->state.error_count = cpu_to_le32(errs + 1);
		pr_warn("VTXFS: scrub metadata node checksum mismatch at vblk=%llu\n",
			(unsigned long long)eb->vblk);
	}
}

static int scrub_thread(void *data)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_scrub_ctx *sc = data;
	struct vtxfs_fs_info *fs = sc->fs;
	struct vtxfs_root *subvol;
	unsigned long idx;
	void *ext_buf;

	ext_buf = kvmalloc(VTXFS_BLOCK_SIZE, GFP_KERNEL);
	if (!ext_buf)
		return -ENOMEM;

	/* 1. Verify metadata of system roots if present */
	if (fs->omap_meta_root) {
		struct vtxfs_extent_buffer *eb = root_node_get_pub(fs->omap_meta_root);
		if (eb) {
			scrub_verify_eb_checksum(sc, eb);
			vtxfs_eb_put(eb);
		}
	}
	if (fs->root_tree) {
		struct vtxfs_extent_buffer *eb = root_node_get_pub(fs->root_tree);
		if (eb) {
			scrub_verify_eb_checksum(sc, eb);
			vtxfs_eb_put(eb);
		}
	}
	if (fs->ref_root) {
		struct vtxfs_extent_buffer *eb = root_node_get_pub(fs->ref_root);
		if (eb) {
			scrub_verify_eb_checksum(sc, eb);
			vtxfs_eb_put(eb);
		}
	}

	/* 2. Iterate across all registered subvolume roots in fs->roots */
	xa_for_each(&fs->roots, idx, subvol) {
		struct vtxfs_path *path;
		struct vtx_key cur;
		int ret;
		u64 last_verified_vblk[VTXFS_MAX_LEVEL] = { 0 };

		if (kthread_should_stop())
			break;
		if (!subvol || subvol->tree_type != VTX_TREE_FS)
			continue;

		path = vtxfs_alloc_path();
		if (!path)
			continue;

		ret = vtxfs_mk_key(&cur, 1, VTX_TYPE_ANY, 0);
		if (ret) {
			vtxfs_free_path(path);
			continue;
		}

		mutex_lock(&fs->trans_lock);
		ret = vtxfs_search_slot_for_read(subvol, path, &cur, 1);
		while (ret == 0 && !kthread_should_stop()) {
			struct vtxfs_extent_buffer *leaf = path->nodes[0];
			int slot = path->slots[0];
			const struct vtx_node_entry *entry;
			int lvl;
			bool is_extent = false;

			if (!leaf || slot < 0 || slot >= eb_nritems(leaf))
				break;

			/* Verify metadata nodes encountered along the path */
			for (lvl = 0; lvl < VTXFS_MAX_LEVEL; lvl++) {
				struct vtxfs_extent_buffer *node = path->nodes[lvl];
				if (!node || !node->hdr)
					continue;
				if (node->vblk == last_verified_vblk[lvl])
					continue;
				last_verified_vblk[lvl] = node->vblk;
				scrub_verify_eb_checksum(sc, node);
			}

			entry = &leaf->hdr->entries[slot];
			if (vtxfs_key_type(&entry->key) == VTX_TYPE_EXTENT) {
				u16 len = eb_val_len(leaf, slot);
				if (len >= sizeof(struct vtx_extent_item)) {
					const void *raw = eb_val(leaf, slot);
					memcpy(ext_buf, raw, min_t(size_t, len, VTXFS_BLOCK_SIZE));
					is_extent = true;
				}
			}

			if (is_extent) {
				cur = entry->key;
				cur.offset = cpu_to_le64(vtxfs_key_off(&entry->key) + 1);
				vtxfs_release_path(path);
				mutex_unlock(&fs->trans_lock);

				scrub_verify_extent(sc, ext_buf);

				cond_resched();
				mutex_lock(&fs->trans_lock);
				ret = vtxfs_search_slot_for_read(subvol, path, &cur, 1);
			} else {
				ret = vtxfs_next_key(subvol, path);
				cond_resched();
			}
		}
		mutex_unlock(&fs->trans_lock);
		vtxfs_free_path(path);
	}

	kvfree(ext_buf);
	sc->state.checkpoint_xid = cpu_to_le64(fs->committed_xid);
	return 0;
}

int vtxfs_scrub_start(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_scrub_ctx *sc;

	if (!fs)
		return -EINVAL;
	if (fs->scrub_priv)
		return -EALREADY;

	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;
	sc->fs = fs;
	sc->task = kthread_run(scrub_thread, sc, "vtxfs-scrub");
	if (IS_ERR(sc->task)) {
		int err = PTR_ERR(sc->task);
		kfree(sc);
		return err;
	}
	fs->scrub_priv = sc;
	return 0;
}

void vtxfs_scrub_stop(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_scrub_ctx *sc;

	if (!fs || !fs->scrub_priv)
		return;
	sc = fs->scrub_priv;
	if (sc->task && !IS_ERR(sc->task))
		kthread_stop(sc->task);
	kfree(sc);
	fs->scrub_priv = NULL;
}

int vtxfs_scrub_status(struct vtxfs_fs_info *fs, struct vtx_scrub_state *out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_scrub_ctx *sc;

	if (!fs || !fs->scrub_priv || !out)
		return -ENODATA;
	sc = fs->scrub_priv;
	*out = sc->state;
	return 0;
}
