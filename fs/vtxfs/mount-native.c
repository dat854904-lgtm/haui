// SPDX-License-Identifier: GPL-2.0-only
/*
 * mount-native.c — kernel-native bootstrap: superblock-ring scan, CPD-ring
 * scan with 64 MiB mirror fallback, VAG directory load, and system-tree
 * root verification. Builds a struct vtxfs_fs_info WITHOUT calling the
 * portable engine's vtx_mount.
 */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include "vtxfs_drv.h"
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"

int vtxfs_bdev_read_blocks(struct block_device *bdev, u64 pblk, u32 count,
			   void *buf)
{
	VTXFS_TRACE_ENTER();

	u32 i;
	int ret = 0;

	if (!bdev || !buf || !count)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		void *ptr = (u8 *)buf + (size_t)i * VTXFS_BLOCK_SIZE;
		struct page *page = vtx_buf_to_page(ptr);
		struct bio *bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_NOFS);

		bio->bi_iter.bi_sector =
			(pblk + i) << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
		if (offset_in_page(ptr) == 0 &&
		    bio_add_page(bio, page, VTXFS_BLOCK_SIZE, 0) == VTXFS_BLOCK_SIZE) {
			ret = submit_bio_wait(bio);
			bio_put(bio);
			if (ret)
				return ret;
		} else {
			struct page *bounce = alloc_page(GFP_NOFS);
			void *kaddr;

			bio_put(bio);
			if (!bounce)
				return -ENOMEM;
			bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_NOFS);
			bio->bi_iter.bi_sector =
				(pblk + i) << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
			__bio_add_page(bio, bounce, VTXFS_BLOCK_SIZE, 0);
			ret = submit_bio_wait(bio);
			bio_put(bio);
			if (!ret) {
				kaddr = kmap_local_page(bounce);
				memcpy(ptr, kaddr, VTXFS_BLOCK_SIZE);
				kunmap_local(kaddr);
			}
			__free_page(bounce);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static inline int vtx_is_zero_buf(const void *buf, size_t len)
{
	const u8 *p = (const u8 *)buf;
	size_t i;

	for (i = 0; i < len; i++) {
		if (p[i] != 0)
			return 0;
	}
	return 1;
}

static int super_valid(const struct vtx_super_block *sb, u64 bdev_blocks)
{
	VTXFS_TRACE_ENTER();

	u64 total;
	u64 incompat = vtx_cpu64(sb->hdr.incompat_flags);
	u32 dev_count = vtx_cpu32(sb->hdr.device_count);

	if (memcmp(sb->hdr.magic, VTXFS_MAGIC_BYTES, VTXFS_MAGIC_LEN) != 0 ||
	    !vtx_super_checksum_verify(sb) ||
	    vtx_cpu16(sb->hdr.major) != VTXFS_VERSION_MAJOR ||
	    vtx_cpu16(sb->hdr.minor) != VTXFS_VERSION_MINOR ||
	    vtx_cpu32(sb->hdr.block_size) != VTXFS_BLOCK_SIZE ||
	    vtx_cpu32(sb->hdr.node_size) != VTXFS_BLOCK_SIZE ||
	    ((incompat & VTXFS_FEAT_INCOMPAT_MULTIDEV) ?
		(dev_count < 1 || dev_count > VTXFS_MAX_DEVICES) :
		(dev_count != 1)))
		return 0;

	/* ABI v2.1 contract: compression feature-gate and reserved bytes zero check */
	if (!(incompat & VTXFS_FEAT_INCOMPAT_COMPRESS)) {
		if (sb->default_compression != 0)
			return 0;
	} else {
		if (sb->default_compression != VTX_COMPR_NONE &&
		    sb->default_compression != VTX_COMPR_ZSTD &&
		    sb->default_compression != VTX_COMPR_LZ4 &&
		    sb->default_compression != VTX_COMPR_ZLIB)
			return 0;
	}
	if (!vtx_is_zero_buf(sb->reserved, sizeof(sb->reserved)))
		return 0;

	total = vtx_cpu64(sb->hdr.total_blocks);
	if (incompat & VTXFS_FEAT_INCOMPAT_MULTIDEV)
		return total >= VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS;
	return total >= VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS &&
	       total <= bdev_blocks;
}

/* Read + verify one tree block through VAG translation. */
static int native_read_tree_block(struct vtxfs_fs_info *fs, u64 oid,
				  u64 vblk, u8 level, u8 tree_type, u64 owner,
				  struct vtxfs_extent_buffer **out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;
	void *tmp;
	u64 pblk;
	int ret;

	ret = vtxfs_vag_vblk_to_pblk(fs->vag, vblk, &pblk);
	if (ret)
		return ret;
	tmp = kvzalloc(fs->node_size, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	ret = vtxfs_bdev_read_blocks(fs->bdev, pblk,
				     fs->node_size >> VTXFS_BLOCK_SHIFT, tmp);
	if (ret)
		goto out;
	if (!vtx_node_slots_valid(tmp, fs->node_size) ||
	    !vtx_node_checksum_verify(tmp, fs->node_size, fs->metadata_uuid)) {
		ret = -EUCLEAN;
		goto out;
	}
	{
		const struct vtx_node_hdr *h = tmp;

		/* CPD identifies a SYSTEM root by VBLK, not a fixed node OID.
		 * After a B+Tree root split, the new root gets a fresh node OID.
		 * The portable engine validates this exact tree/level/root flag,
		 * but mount-native previously rejected all grown trees because
		 * it expected the initial mkfs OID (e.g. REF_TREE==3).
		 * oid==0 permits this CPD-anchored root identity, not arbitrary
		 * child nodes. Preserve strict checks on tree and checkpoint. */
		if (!vtx_cpu64(h->oid) ||
		    vtx_cpu64(h->oid) > VTX_KEY_OID_MASK ||
		    (oid && vtx_cpu64(h->oid) != oid) ||
		    vtx_cpu64(h->self_vblk) != vblk ||
		    !(vtx_cpu16(h->flags) & VTX_NODE_FLAG_ROOT) ||
		    h->level != level || h->tree_type != tree_type ||
		    vtx_cpu64(h->owner_id) != owner ||
		    vtx_cpu64(h->xid) == 0 ||
		    vtx_cpu64(h->xid) > fs->committed_xid) {
			pr_err("mount-native: node header mismatch: oid=%llu vs %llu, lvl=%u vs %u, tt=%u vs %u, owner=%llu vs %llu, xid=%llu (committed=%llu)\n",
			       vtx_cpu64(h->oid), oid, h->level, level,
			       h->tree_type, tree_type,
			       vtx_cpu64(h->owner_id), owner,
			       vtx_cpu64(h->xid), fs->committed_xid);
			ret = -EUCLEAN;
			goto out;
		}
	}
	eb = vtxfs_eb_alloc(fs, vblk, fs->node_size);
	if (IS_ERR(eb)) {
		ret = PTR_ERR(eb);
		goto out;
	}
	memcpy(eb->hdr, tmp, fs->node_size);
	/* Record the actual node OID: it changes when a CoW root splits. */
	eb->oid = vtx_cpu64(((struct vtx_node_hdr *)eb->hdr)->oid);
	eb->birth_xid = vtx_cpu64(((struct vtx_node_hdr *)eb->hdr)->xid);
	set_bit(VTXFS_EB_UPTODATE, &eb->flags);
	set_bit(VTXFS_EB_VERIFIED, &eb->flags);
	{
		void *old = xa_cmpxchg(&fs->eb_cache, vblk, NULL, eb,
				       GFP_KERNEL);

		if (xa_is_err(old)) {
			ret = xa_err(old);
			vtxfs_eb_put(eb);
			goto out;
		}
		if (old) {
			struct vtxfs_extent_buffer *exist = old;

			vtxfs_eb_put(eb);
			if (!refcount_inc_not_zero(&exist->refs)) {
				ret = -EAGAIN;
				goto out;
			}
			eb = exist;
		}
	}
	*out = eb;
	ret = 0;
out:
	kvfree(tmp);
	return ret;
}

static struct vtxfs_root *root_alloc(struct vtxfs_fs_info *fs,
				     enum vtx_tree_type tt, u64 oid, u64 subvol)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *r = kzalloc(sizeof(*r), GFP_KERNEL);

	if (!r)
		return ERR_PTR(-ENOMEM);
	r->fs = fs;
	r->fs_info = fs;
	r->tree_type = tt;
	r->root_oid = oid;
	r->subvol_id = subvol;
	refcount_set(&r->refs, 1);
	xa_init(&r->inodes);
	xa_init(&r->delayed_nodes);
	atomic64_set(&r->next_ino, VTXFS_INO_FIRST_USER);
	return r;
}

static void vtxfs_free_root(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	if (!root)
		return;
	if (root->node) {
		vtxfs_eb_put(root->node);
		root->node = NULL;
	}
	if (root->committed_node) {
		vtxfs_eb_put(root->committed_node);
		root->committed_node = NULL;
	}
	xa_destroy(&root->inodes);
	xa_destroy(&root->delayed_nodes);
	kfree(root);
}

static int load_system_root(struct vtxfs_fs_info *fs, enum vtx_tree_type tt,
			    u64 oid, u64 vblk, u8 level,
			    struct vtxfs_root **out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *root;
	struct vtxfs_extent_buffer *eb;
	int ret;

	root = root_alloc(fs, tt, oid, 0);
	if (IS_ERR(root))
		return PTR_ERR(root);
	/* CPD root VBLK is authoritative. The mkfs OID is NOT invariant
	 * after CoW tree growth (see portable engine verify_cpd_roots). */
	ret = native_read_tree_block(fs, 0, vblk, level, tt, 0, &eb);
	if (ret) {
		kfree(root);
		return ret;
	}
	root->root_oid = eb->oid;
	rcu_assign_pointer(root->node, eb);
	root->committed_node = eb;
	vtxfs_eb_get(eb);
	*out = root;
	return 0;
}

/* Scan superblock + CPD rings (primary and 64 MiB mirror), build fs_info. */
int vtxfs_mount_native(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	struct vtxfs_fs_info *fs;
	struct vtx_super_block *cand_sb;
	struct vtx_checkpoint_desc *cand_cpd;
	u64 bdev_blocks, best_sb_gen = 0, best_xid = 0;
	int found_sb = 0, found_cpd = 0, m, s, ret;

	bdev_blocks = bdev_nr_bytes(sb->s_bdev) >> VTXFS_BLOCK_SHIFT;
	VTXFS_TRACE_EVENT("native-mount size_blocks=%llu\n",
			  (unsigned long long)bdev_blocks);
	fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	cand_sb = vzalloc(sizeof(*cand_sb));
	cand_cpd = vzalloc(sizeof(*cand_cpd));
	if (!fs || !cand_sb || !cand_cpd) {
		ret = -ENOMEM;
		goto fail;
	}
	fs->sb = sb;
	fs->bdev = sb->s_bdev;
	fs->node_size = VTXFS_BLOCK_SIZE;
	mutex_init(&fs->trans_lock);
	init_rwsem(&fs->commit_sem);
	vtxfs_lock_init(fs);
	spin_lock_init(&fs->free_space_lock);
	fs->free_space_tree = RB_ROOT;
	spin_lock_init(&fs->eb_lock);
	INIT_LIST_HEAD(&fs->dirty_ebs);
	INIT_LIST_HEAD(&fs->ordered_extents);
	spin_lock_init(&fs->ordered_lock);
	atomic_set(&fs->ordered_count, 0);
	atomic64_set(&fs->next_object_id, 1024);
	xa_init(&fs->eb_cache);
	xa_init(&fs->roots);

	/* 1. Superblock ring (primary + mirror). */
	for (m = 0; m < 2; m++) {
		u64 base = (m ? VTXFS_BOOT_MIRROR0_PBLK : 0) +
			   VTXFS_SUPER_RING_PBLK;

		for (s = 0; s < (int)VTXFS_SUPER_RING_SLOTS; s++) {
			if (vtxfs_bdev_read_blocks(fs->bdev, base + s, 1,
						   cand_sb))
				continue;
			if (!super_valid(cand_sb, bdev_blocks))
				continue;
			if (!found_sb ||
			    vtx_cpu64(cand_sb->hdr.super_generation) >
			    best_sb_gen) {
				fs->super = cand_sb->hdr;
				best_sb_gen =
					vtx_cpu64(cand_sb->hdr.super_generation);
				found_sb = 1;
			}
		}
	}
	if (!found_sb) {
		pr_err("mount-native: no valid sb found\n");
		ret = -EUCLEAN;
		goto fail;
	}
	if (vtx_feature_policy(vtx_cpu64(fs->super.compat_flags),
			       vtx_cpu64(fs->super.ro_compat_flags),
			       vtx_cpu64(fs->super.incompat_flags))
	    == VTX_MOUNT_REFUSE) {
		ret = -EOPNOTSUPP;
		goto fail;
	}
	memcpy(fs->metadata_uuid, fs->super.metadata_uuid, VTXFS_UUID_SIZE);

	/* 2. CPD ring (primary + mirror) — highest valid xid wins. */
	for (m = 0; m < 2; m++) {
		u64 base = (m ? VTXFS_BOOT_MIRROR0_PBLK : 0) +
			   VTXFS_CPD_RING_PBLK;

		for (s = 0; s < (int)VTXFS_CPD_RING_SLOTS; s++) {
			u64 xid;

			if (vtxfs_bdev_read_blocks(fs->bdev, base + s, 1,
						   cand_cpd))
				continue;
			if (memcmp(cand_cpd->hdr.magic, VTXFS_CPD_MAGIC,
				   VTXFS_MAGIC_LEN) != 0 ||
			    !vtx_cpd_checksum_verify(cand_cpd))
				continue;
			xid = vtx_cpu64(cand_cpd->hdr.xid);
			if (!xid || VTXFS_CPD_SLOT_FOR_XID(xid) != (u32)s ||
			    memcmp(cand_cpd->hdr.fsid_uuid,
				   fs->super.fsid_uuid, VTXFS_UUID_SIZE) ||
			    vtx_cpu64(cand_cpd->hdr.super_generation) !=
			    best_sb_gen)
				continue;
			if (found_cpd && xid <= best_xid)
				continue;
			fs->cp = cand_cpd->hdr;
			best_xid = xid;
			found_cpd = 1;
		}
	}
	if (!found_cpd) {
		pr_err("mount-native: no valid cpd found\n");
		ret = -EUCLEAN;
		goto fail;
	}
	fs->committed_xid = best_xid;
	fs->current_xid = best_xid + 1;

	/* 3. VAG directory (phys_ref -> root -> leaf -> bitmaps). */
	ret = vtxfs_vag_load(fs->bdev, &fs->cp.vag_dir_root,
			     vtx_cpu64(fs->super.total_blocks),
			     fs->metadata_uuid, &fs->vag);
	if (ret)
		goto fail;
	fs->alloc_ops = &vtxfs_vag_alloc_ops;
	fs->alloc_priv = fs->vag;

	/* 4. System tree roots. */
	ret = load_system_root(fs, VTX_TREE_OMAP_META, VTXFS_OID_OMAP_META,
			       vtx_cpu64(fs->cp.omap_meta_tree_vblk),
			       fs->cp.tree_levels[VTX_TREE_OMAP_META],
			       &fs->omap_meta_root);
	if (ret)
		goto fail_vag;
	ret = load_system_root(fs, VTX_TREE_ROOT, VTXFS_OID_ROOT_TREE,
			       vtx_cpu64(fs->cp.root_tree_vblk),
			       fs->cp.tree_levels[VTX_TREE_ROOT],
			       &fs->root_tree);
	if (ret)
		goto fail_vag;
	ret = load_system_root(fs, VTX_TREE_REF, VTXFS_OID_REF_TREE,
			       vtx_cpu64(fs->cp.ref_tree_vblk),
			       fs->cp.tree_levels[VTX_TREE_REF],
			       &fs->ref_root);
	if (ret)
		goto fail_vag;
	ret = load_system_root(fs, VTX_TREE_SFQ, VTXFS_OID_SFQ_TREE,
			       vtx_cpu64(fs->cp.sfq_tree_vblk),
			       fs->cp.tree_levels[VTX_TREE_SFQ],
			       &fs->sfq_root);
	if (ret)
		goto fail_vag;
	ret = load_system_root(fs, VTX_TREE_REAPER, VTXFS_OID_REAPER_TREE,
			       vtx_cpu64(fs->cp.reaper_tree_vblk),
			       fs->cp.tree_levels[VTX_TREE_REAPER],
			       &fs->reaper_root);
	if (ret)
		goto fail_vag;
	ret = load_system_root(fs, VTX_TREE_UUID, VTXFS_OID_UUID_TREE,
			       vtx_cpu64(fs->cp.uuid_tree_vblk),
			       fs->cp.tree_levels[VTX_TREE_UUID],
			       &fs->uuid_root);
	if (ret)
		goto fail_vag;

	/* Cây hệ thống: phân giải con VIRTUAL qua omap_meta */
	fs->root_tree->resolve_root   = fs->omap_meta_root;
	fs->ref_root->resolve_root    = fs->omap_meta_root;
	fs->sfq_root->resolve_root    = fs->omap_meta_root;
	fs->reaper_root->resolve_root = fs->omap_meta_root;
	fs->uuid_root->resolve_root   = fs->omap_meta_root;

	/* Native trees are diagnostic shadows; portable engine owns writes. */
	fs->shadow_only = (sbi->fs != NULL);
	sbi->fs_info = fs;
	vfree(cand_sb);
	vfree(cand_cpd);
	return 0;

fail_vag:
	vtxfs_free_root(fs->uuid_root);
	vtxfs_free_root(fs->reaper_root);
	vtxfs_free_root(fs->sfq_root);
	vtxfs_free_root(fs->ref_root);
	vtxfs_free_root(fs->root_tree);
	vtxfs_free_root(fs->omap_meta_root);
	xa_destroy(&fs->roots);
	xa_destroy(&fs->eb_cache);
	vtxfs_vag_free(fs->vag);
	rcu_barrier();
fail:
	vfree(cand_sb);
	vfree(cand_cpd);
	kfree(fs);
	return ret;
}
EXPORT_SYMBOL_GPL(vtxfs_mount_native);

void vtxfs_mount_native_teardown(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	struct vtxfs_fs_info *fs;
	struct vtxfs_root *root;
	struct vtxfs_extent_buffer *deb, *dtmp;
	unsigned long vblk, flags;

	if (!sbi || !sbi->fs_info)
		return;
	fs = sbi->fs_info;

	/* Native writes are forbidden while the portable engine is owner. */
	if (fs->shadow_only && !list_empty(&fs->dirty_ebs))
		pr_err("vtxfs: unexpected dirty native tree; refusing to hide error\n");

	/* 1. Drain any remaining dirty extent buffers */
	spin_lock_irqsave(&fs->eb_lock, flags);
	list_for_each_entry_safe(deb, dtmp, &fs->dirty_ebs, dirty_link) {
		list_del_init(&deb->dirty_link);
		clear_bit(VTXFS_EB_DIRTY, &deb->flags);
		vtxfs_eb_put(deb);
	}
	spin_unlock_irqrestore(&fs->eb_lock, flags);

	/* 2. Free system roots */
	vtxfs_free_root(fs->omap_meta_root);
	fs->omap_meta_root = NULL;
	vtxfs_free_root(fs->root_tree);
	fs->root_tree = NULL;
	vtxfs_free_root(fs->ref_root);
	fs->ref_root = NULL;
	vtxfs_free_root(fs->sfq_root);
	fs->sfq_root = NULL;
	vtxfs_free_root(fs->reaper_root);
	fs->reaper_root = NULL;
	vtxfs_free_root(fs->uuid_root);
	fs->uuid_root = NULL;

	/* 3. Free loaded subvolume roots in fs->roots */
	xa_for_each(&fs->roots, vblk, root) {
		vtxfs_free_root(root);
	}
	xa_destroy(&fs->roots);

	/* 4. Destroy eb_cache lookup */
	xa_destroy(&fs->eb_cache);

	/* 5. Free VAG */
	if (fs->vag) {
		vtxfs_vag_free(fs->vag);
		fs->vag = NULL;
	}

	/* 6. The free-run cache owns heap nodes even when no roots remain. */
	vtxfs_free_space_invalidate(fs);

	/* 7. Wait for all pending RCU callbacks (vtxfs_eb_free_rcu) to finish */
	rcu_barrier();

	kfree(fs);
	sbi->fs_info = NULL;
}
EXPORT_SYMBOL_GPL(vtxfs_mount_native_teardown);
