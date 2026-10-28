/* SPDX-License-Identifier: GPL-2.0-only */
#include "vtxfs_engine.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
static inline void *vtx_zalloc(size_t size) { return vzalloc(size); }
static inline void vtx_free(void *ptr) { vfree(ptr); }
#else
#include <errno.h>
#include <string.h>
#include <stdlib.h>
static inline void *vtx_zalloc(size_t size) { return calloc(1, size); }
static inline void vtx_free(void *ptr) { free(ptr); }
#endif

/* Dynamic capacities: the old static limits silently made all >4096
 * allocation / >128 subvolume filesystems unverifiable. Bounded to avoid
 * adversarial metadata exhausting kernel/user memory during fsck. */
#define FSCK_REF_CAP_LIMIT     (1U << 20)
#define FSCK_SUBVOL_CAP_LIMIT  (1U << 16)

struct fsck_ref_tally {
    __u64 allocation_id;
    __u64 start_vblk;
    __u32 block_count;
    __u32 flags;
    __u64 expected_refs;
    __u64 observed_refs;
    int rev_corrupt;
};

struct fsck_ctx {
    struct vtx_fs *fs;
    struct vtx_fsck_report report;
    __u32 num_refs, refs_capacity;
    struct fsck_ref_tally *refs;
    __u8 *expected_bms[VTX_MAX_SUPPORTED_VAGS];
    __u64 *subvol_ids;
    __u32 subvols_capacity;
    __u8 val_buf[256];
};

static int fsck_grow_refs(struct fsck_ctx *fc)
{
    __u32 n = fc->refs_capacity ? fc->refs_capacity * 2U : 4096U;
    struct fsck_ref_tally *p;
    if (n > FSCK_REF_CAP_LIMIT)
        return -E2BIG;
    p = vtx_zalloc((size_t)n * sizeof(*p));
    if (!p)
        return -ENOMEM;
    if (fc->num_refs)
        memcpy(p, fc->refs, (size_t)fc->num_refs * sizeof(*p));
    vtx_free(fc->refs);
    fc->refs = p;
    fc->refs_capacity = n;
    return 0;
}
static int fsck_grow_subvols(struct fsck_ctx *fc, __u32 count)
{
    __u32 n = fc->subvols_capacity ? fc->subvols_capacity * 2U : 128U;
    __u64 *p;
    if (n > FSCK_SUBVOL_CAP_LIMIT)
        return -E2BIG;
    p = vtx_zalloc((size_t)n * sizeof(*p));
    if (!p)
        return -ENOMEM;
    if (count)
        memcpy(p, fc->subvol_ids, (size_t)count * sizeof(*p));
    vtx_free(fc->subvol_ids);
    fc->subvol_ids = p;
    fc->subvols_capacity = n;
    return 0;
}

static struct fsck_ref_tally *find_tally_by_id(struct fsck_ctx *fc, __u64 id)
{
    __u32 i;
    for (i = 0; i < fc->num_refs; i++) {
        if (fc->refs[i].allocation_id == id)
            return &fc->refs[i];
    }
    return NULL;
}

static struct fsck_ref_tally *find_tally_by_vblk(struct fsck_ctx *fc,
                                                 __u64 vblk)
{
    __u32 i;
    for (i = 0; i < fc->num_refs; i++) {
        if (fc->refs[i].start_vblk == vblk)
            return &fc->refs[i];
    }
    return NULL;
}

static int mark_expected_bm(struct fsck_ctx *fc, __u64 start_vblk, __u32 count)
{
    __u32 vag_id, ag_off, i;
    __u8 *bm;

    if (!count || vtx_vblk_decode(start_vblk, &vag_id, &ag_off) != 0 ||
        vag_id >= fc->fs->vag.total_vags ||
        ag_off + count > VTXFS_VAG_BLOCKS) {
        return -EUCLEAN;
    }
    bm = fc->expected_bms[vag_id];
    for (i = 0; i < count; i++) {
        __u32 bit = ag_off + i;
        __u8 mask = (__u8)(1U << (bit & 7U));
        if (bm[bit >> 3] & mask) {
            return -EUCLEAN; /* Double-claimed physical block! */
        }
        bm[bit >> 3] |= mask;
    }
    fc->report.allocated_blocks_counted += count;
    return 0;
}

static int walk_and_verify_tree(struct fsck_ctx *fc,
                                const struct vtx_rw_tree *t,
                                __u64 oid, __u64 vblk, int level,
                                __u64 exact_xid, int is_root,
                                const struct vtx_key *expected_min)
{
    const void *node = NULL;
    const struct vtx_node_hdr *h;
    size_t bytes = 0;
    int verified = 0, ret;
    __u16 flags;
    struct fsck_ref_tally *rt;

    ret = t->ops->read_node(t->ctx, vblk, &node, &bytes, &verified);
    if (ret)
        return ret;
    if (!verified || bytes != VTXFS_BLOCK_SIZE ||
        !vtx_node_slots_valid((const struct vtx_node_hdr *)node,
                              VTXFS_BLOCK_SIZE)) {
        t->ops->release_node(t->ctx, node);
        return -EUCLEAN;
    }
    h = (const struct vtx_node_hdr *)node;
    flags = vtx_cpu16(h->flags);
    if (vtx_cpu64(h->oid) != oid ||
        vtx_cpu64(h->self_vblk) != vblk ||
        vtx_cpu64(h->owner_id) != t->owner_id ||
        vtx_cpu64(h->xid) == 0 ||
        vtx_cpu64(h->xid) > fc->fs->committed_xid ||
        (exact_xid && vtx_cpu64(h->xid) != exact_xid) ||
        h->tree_type != t->tree_type ||
        h->level != level ||
        !!(flags & VTX_NODE_FLAG_ROOT) != !!is_root) {
        t->ops->release_node(t->ctx, node);
        return -EUCLEAN;
    }
    if (expected_min &&
        (!vtx_cpu16(h->nitems) ||
         vtx_key_cmp(expected_min, &h->entries[0].key) != 0)) {
        t->ops->release_node(t->ctx, node);
        return -EUCLEAN;
    }

    rt = find_tally_by_vblk(fc, vblk);
    if (!rt || !(rt->flags & VTX_REF_FLAG_META) || rt->block_count != 1) {
        t->ops->release_node(t->ctx, node);
        return -EUCLEAN;
    }
    rt->observed_refs++;
    fc->report.btree_nodes_verified++;

    if (level > 0) {
        __u32 n = vtx_cpu16(h->nitems), i;
        __u8 *copy = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
        if (!copy) {
            t->ops->release_node(t->ctx, node);
            return -ENOMEM;
        }
        memcpy(copy, node, VTXFS_BLOCK_SIZE);
        t->ops->release_node(t->ctx, node);
        h = (const struct vtx_node_hdr *)copy;

        for (i = 0; i < n; i++) {
            const struct vtx_node_entry *e = &h->entries[i];
            struct vtx_key_ptr_val ptr;
            __u64 c_oid, c_vblk = 0, c_xid = 0;

            if (vtx_cpu16(e->slot.val_len) != sizeof(ptr)) {
                vtx_free(copy);
                return -EUCLEAN;
            }
            memcpy(&ptr, copy + (__u32)vtx_cpu16(e->slot.val_off8) * 8U,
                   sizeof(ptr));
            c_oid = vtx_cpu64(ptr.child_oid);
            if (t->is_physical) {
                c_vblk = vtx_cpu64(ptr.hint_vblk);
                c_xid = vtx_cpu64(ptr.hint_xid);
            } else {
                ret = t->ops->omap_resolve(t->ctx, t->view_id, c_oid, &c_vblk);
                if (ret) {
                    vtx_free(copy);
                    return ret;
                }
            }
            ret = walk_and_verify_tree(fc, t, c_oid, c_vblk, level - 1,
                                       c_xid, 0, &e->key);
            if (ret) {
                vtx_free(copy);
                return ret;
            }
        }
        vtx_free(copy);
        return 0;
    }

    t->ops->release_node(t->ctx, node);
    return 0;
}

static int verify_subvolume_items(struct fsck_ctx *fc, __u64 subvol_id)
{
    struct vtx_fs *fs = fc->fs;
    struct vtx_key cur_k = {0}, found_k;
    __u8 *ibuf = NULL, *blk_buf = NULL;
    size_t bytes;
    int first = 1, ret;

    ret = vtx_subvol_switch_view(fs, subvol_id);
    if (ret)
        return ret;
    ret = walk_and_verify_tree(fc, &fs->active_omap_view,
                               fs->active_omap_view.root_oid,
                               fs->active_omap_view.root_vblk,
                               fs->active_omap_view.root_level, 0, 1, NULL);
    if (ret)
        return ret;
    ret = walk_and_verify_tree(fc, &fs->active_fs_tree,
                               fs->active_fs_tree.root_oid,
                               fs->active_fs_tree.root_vblk,
                               fs->active_fs_tree.root_level, 0, 1, NULL);
    if (ret)
        return ret;

    ibuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    blk_buf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!ibuf || !blk_buf) {
        vtx_free(ibuf);
        vtx_free(blk_buf);
        return -ENOMEM;
    }

    for (;;) {
        __u8 type;
        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first,
                          &found_k, ibuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur_k = found_k;
        type = VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type));

        if (type == VTX_TYPE_INODE) {
            if (bytes != sizeof(struct vtx_inode_item)) {
                ret = -EUCLEAN;
                break;
            }
        } else if (type == VTX_TYPE_DIR_ITEM) {
            if (!vtx_dir_item_valid((const struct vtx_dir_item *)ibuf, bytes)) {
                ret = -EUCLEAN;
                break;
            }
        } else if (type == VTX_TYPE_DIR_INDEX) {
            if (bytes != sizeof(struct vtx_dir_index_val)) {
                ret = -EUCLEAN;
                break;
            }
        } else if (type == VTX_TYPE_EXTENT) {
            const struct vtx_extent_item *ext =
                (const struct vtx_extent_item *)ibuf;
            if (!vtx_extent_p0_value_valid(ext, bytes)) {
                ret = -EUCLEAN;
                break;
            }
            fc->report.data_extents_verified++;
            if (!(vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)) {
                struct vtx_key k_ref;
                struct vtx_ref_item r_it;
                size_t rbytes = sizeof(r_it);
                struct fsck_ref_tally *rt;
                __u32 phys_blks = vtx_cpu32(ext->physical_blocks), b;

                k_ref.oid_and_type =
                    vtx_le64(VTX_MAKE_OID_TYPE(vtx_cpu64(ext->allocation_id),
                                               VTX_TYPE_REF));
                k_ref.offset = 0;
                ret = vtx_rw_lookup(&fs->ref_tree, &k_ref, &r_it, &rbytes);
                if (ret || rbytes != sizeof(r_it) ||
                    !vtx_extent_within_ref(ext, &r_it)) {
                    ret = -EUCLEAN;
                    break;
                }
                rt = find_tally_by_id(fc, vtx_cpu64(ext->allocation_id));
                if (!rt || !(rt->flags & VTX_REF_FLAG_DATA)) {
                    ret = -EUCLEAN;
                    break;
                }
                rt->observed_refs++;

                for (b = 0; b < phys_blks; b++) {
                    __u64 pblk = 0;
                    __le32 crc_le;
                    ret = vtx_vag_vblk_to_pblk(&fs->vag,
                                               vtx_cpu64(ext->vblk) + b, &pblk);
                    if (ret)
                        break;
                    ret = fs->bdev->ops->read_blocks(fs->bdev, pblk, 1, blk_buf);
                    if (ret)
                        break;
                    memcpy(&crc_le, ext->tail + b * sizeof(__le32),
                           sizeof(crc_le));
                    if (vtx_crc32c(blk_buf, VTXFS_BLOCK_SIZE) !=
                        vtx_cpu32(crc_le)) {
                        ret = -EUCLEAN;
                        break;
                    }
                }
                if (ret)
                    break;

                if ((vtx_cpu32(ext->flags) & VTX_EXTENT_COMPRESSED) && phys_blks > 0) {
                    size_t csz = (size_t)phys_blks * VTXFS_BLOCK_SIZE;
                    __u8 *cdata = (__u8 *)vtx_zalloc(csz);
                    __u8 *decomp = NULL;
                    __u32 decomp_len = 0;

                    if (!cdata) {
                        ret = -ENOMEM;
                        break;
                    }
                    for (b = 0; b < phys_blks; b++) {
                        __u64 pblk = 0;
                        vtx_vag_vblk_to_pblk(&fs->vag, vtx_cpu64(ext->vblk) + b, &pblk);
                        fs->bdev->ops->read_blocks(fs->bdev, pblk, 1,
                                                   cdata + (size_t)b * VTXFS_BLOCK_SIZE);
                    }
                    ret = vtx_decompress_extent(cdata, (__u32)csz, &decomp, &decomp_len);
                    vtx_free(cdata);
                    if (ret != 0 || decomp_len != vtx_cpu32(ext->logical_bytes)) {
                        vtx_buffer_free(decomp);
                        ret = -EUCLEAN;
                        break;
                    }
                    vtx_buffer_free(decomp);
                }
            }
        }
    }

    vtx_free(ibuf);
    vtx_free(blk_buf);
    return ret;
}

static int is_valid_super(const struct vtx_super_block *sb,
                          const __u8 fsid_uuid[VTXFS_UUID_SIZE])
{
    if (!sb)
        return 0;
    if (memcmp(sb->hdr.magic, VTXFS_MAGIC_BYTES, VTXFS_MAGIC_LEN) != 0)
        return 0;
    if (!vtx_super_checksum_verify(sb))
        return 0;
    if (memcmp(sb->hdr.fsid_uuid, fsid_uuid, VTXFS_UUID_SIZE) != 0)
        return 0;
    return 1;
}

static int is_valid_cpd(const struct vtx_checkpoint_desc *cpd,
                        __u32 slot, const __u8 fsid_uuid[VTXFS_UUID_SIZE],
                        __u64 expected_sb_gen)
{
    __u64 xid;

    if (!cpd)
        return 0;
    if (memcmp(cpd->hdr.magic, VTXFS_CPD_MAGIC, VTXFS_MAGIC_LEN) != 0)
        return 0;
    if (!vtx_cpd_checksum_verify(cpd))
        return 0;
    xid = vtx_cpu64(cpd->hdr.xid);
    if (!xid || VTXFS_CPD_SLOT_FOR_XID(xid) != slot)
        return 0;
    if (memcmp(cpd->hdr.fsid_uuid, fsid_uuid, VTXFS_UUID_SIZE) != 0)
        return 0;
    if (vtx_cpu64(cpd->hdr.super_generation) != expected_sb_gen)
        return 0;
    if (vtx_cpu32(cpd->hdr.cp_flags) & ~VTXFS_CP_FLAGS_P0_KNOWN)
        return 0;
    return 1;
}

int vtx_fsck_repair(struct vtx_bdev *bdev, int dry_run,
                    struct vtx_fsck_report *report_out)
{
    struct fsck_ctx *fc;
    struct vtx_fs *fs;
    struct vtx_key cur_k = {0}, found_k;
    size_t bytes;
    __u32 i, num_subvols = 0;
    int first, ret;
    int need_commit = 0;
    int force_ro = (dry_run == 0) ? 0 : 1;

    if (!bdev)
        return -EINVAL;
    fc = (struct fsck_ctx *)vtx_zalloc(sizeof(*fc));
    fs = vtx_fs_alloc();
    if (!fc || !fs) {
        vtx_free(fc);
        vtx_fs_free(fs);
        return -ENOMEM;
    }
    fc->fs = fs;

    ret = vtx_mount(fs, bdev, force_ro);
    if (ret) {
        fc->report.errors_found++;
        goto out;
    }

    fc->report.committed_xid = fs->committed_xid;
    fc->report.super_generation = vtx_cpu64(fs->super_blk.hdr.super_generation);
    fc->report.total_vags = fs->vag.total_vags;

    /* 0. Synchronize Superblock & CPD mirrors if dry_run >= 0. */
    if (dry_run >= 0) {
        struct vtx_super_block *sb_prim =
            (struct vtx_super_block *)vtx_zalloc(sizeof(*sb_prim));
        struct vtx_super_block *sb_mirr =
            (struct vtx_super_block *)vtx_zalloc(sizeof(*sb_mirr));
        struct vtx_checkpoint_desc *cand_prim =
            (struct vtx_checkpoint_desc *)vtx_zalloc(sizeof(*cand_prim));
        struct vtx_checkpoint_desc *cand_mirr =
            (struct vtx_checkpoint_desc *)vtx_zalloc(sizeof(*cand_mirr));
        int s;
        if (!sb_prim || !sb_mirr || !cand_prim || !cand_mirr) {
            vtx_free(sb_prim);
            vtx_free(sb_mirr);
            vtx_free(cand_prim);
            vtx_free(cand_mirr);
            ret = -ENOMEM;
            goto out;
        }
        for (s = 0; s < (int)VTXFS_SUPER_RING_SLOTS; s++) {
            __u64 prim_pblk = VTXFS_SUPER_RING_PBLK + (__u64)s;
            __u64 mirr_pblk = VTXFS_BOOT_MIRROR0_PBLK + prim_pblk;
            int prim_read = (bdev->ops->read_blocks(bdev, prim_pblk, 1, sb_prim) == 0);
            int mirr_read = (bdev->ops->read_blocks(bdev, mirr_pblk, 1, sb_mirr) == 0);
            int prim_ok = prim_read && is_valid_super(sb_prim, fs->super_blk.hdr.fsid_uuid);
            int mirr_ok = mirr_read && is_valid_super(sb_mirr, fs->super_blk.hdr.fsid_uuid);

            if (prim_ok && !mirr_ok) {
                fc->report.errors_found++;
                if (dry_run) {
                    fc->report.repairs_planned++;
                } else {
                    ret = bdev->ops->write_fua(bdev, mirr_pblk, 1, sb_prim);
                    if (ret) {
                        vtx_free(sb_prim);
                        vtx_free(sb_mirr);
                        vtx_free(cand_prim);
                        vtx_free(cand_mirr);
                        goto err;
                    }
                    fc->report.repairs_performed++;
                }
            } else if (!prim_ok && mirr_ok) {
                fc->report.errors_found++;
                if (dry_run) {
                    fc->report.repairs_planned++;
                } else {
                    ret = bdev->ops->write_fua(bdev, prim_pblk, 1, sb_mirr);
                    if (ret) {
                        vtx_free(sb_prim);
                        vtx_free(sb_mirr);
                        vtx_free(cand_prim);
                        vtx_free(cand_mirr);
                        goto err;
                    }
                    fc->report.repairs_performed++;
                }
            } else if (prim_ok && mirr_ok &&
                       memcmp(sb_prim, sb_mirr, sizeof(*sb_prim)) != 0) {
                __u64 prim_gen = vtx_cpu64(sb_prim->hdr.super_generation);
                __u64 mirr_gen = vtx_cpu64(sb_mirr->hdr.super_generation);
                fc->report.errors_found++;
                if (dry_run) {
                    fc->report.repairs_planned++;
                } else {
                    if (prim_gen >= mirr_gen)
                        ret = bdev->ops->write_fua(bdev, mirr_pblk, 1, sb_prim);
                    else
                        ret = bdev->ops->write_fua(bdev, prim_pblk, 1, sb_mirr);
                    if (ret) {
                        vtx_free(sb_prim);
                        vtx_free(sb_mirr);
                        vtx_free(cand_prim);
                        vtx_free(cand_mirr);
                        goto err;
                    }
                    fc->report.repairs_performed++;
                }
            } else if (s < 2 && !prim_ok && !mirr_ok) {
                fc->report.errors_found++;
                if (dry_run) {
                    fc->report.repairs_planned++;
                } else {
                    ret = bdev->ops->write_fua(bdev, prim_pblk, 1, &fs->super_blk);
                    if (!ret)
                        ret = bdev->ops->write_fua(bdev, mirr_pblk, 1, &fs->super_blk);
                    if (ret) {
                        vtx_free(sb_prim);
                        vtx_free(sb_mirr);
                        vtx_free(cand_prim);
                        vtx_free(cand_mirr);
                        goto err;
                    }
                    fc->report.repairs_performed++;
                }
            }
        }
        vtx_free(sb_prim);
        vtx_free(sb_mirr);

        for (s = 0; s < (int)VTXFS_CPD_RING_SLOTS; s++) {
            __u64 prim_pblk = VTXFS_CPD_RING_PBLK + (__u64)s;
            __u64 mirr_pblk = VTXFS_BOOT_MIRROR0_PBLK + prim_pblk;
            int prim_read = (bdev->ops->read_blocks(bdev, prim_pblk, 1, cand_prim) == 0);
            int mirr_read = (bdev->ops->read_blocks(bdev, mirr_pblk, 1, cand_mirr) == 0);
            int prim_ok = prim_read && is_valid_cpd(cand_prim, (__u32)s,
                                                   fs->super_blk.hdr.fsid_uuid,
                                                   vtx_cpu64(fs->super_blk.hdr.super_generation));
            int mirr_ok = mirr_read && is_valid_cpd(cand_mirr, (__u32)s,
                                                   fs->super_blk.hdr.fsid_uuid,
                                                   vtx_cpu64(fs->super_blk.hdr.super_generation));

            if (prim_ok && !mirr_ok) {
                fc->report.errors_found++;
                if (dry_run) {
                    fc->report.repairs_planned++;
                } else {
                    ret = bdev->ops->write_fua(bdev, mirr_pblk, 1, cand_prim);
                    if (ret) {
                        vtx_free(cand_prim);
                        vtx_free(cand_mirr);
                        goto err;
                    }
                    fc->report.repairs_performed++;
                }
            } else if (!prim_ok && mirr_ok) {
                fc->report.errors_found++;
                if (dry_run) {
                    fc->report.repairs_planned++;
                } else {
                    ret = bdev->ops->write_fua(bdev, prim_pblk, 1, cand_mirr);
                    if (ret) {
                        vtx_free(cand_prim);
                        vtx_free(cand_mirr);
                        goto err;
                    }
                    fc->report.repairs_performed++;
                }
            } else if (prim_ok && mirr_ok &&
                       memcmp(cand_prim, cand_mirr, sizeof(*cand_prim)) != 0) {
                __u64 prim_xid = vtx_cpu64(cand_prim->hdr.xid);
                __u64 mirr_xid = vtx_cpu64(cand_mirr->hdr.xid);
                fc->report.errors_found++;
                if (dry_run) {
                    fc->report.repairs_planned++;
                } else {
                    if (prim_xid >= mirr_xid)
                        ret = bdev->ops->write_fua(bdev, mirr_pblk, 1, cand_prim);
                    else
                        ret = bdev->ops->write_fua(bdev, prim_pblk, 1, cand_mirr);
                    if (ret) {
                        vtx_free(cand_prim);
                        vtx_free(cand_mirr);
                        goto err;
                    }
                    fc->report.repairs_performed++;
                }
            }
        }
        vtx_free(cand_prim);
        vtx_free(cand_mirr);
    }

    for (i = 0; i < fs->vag.total_vags; i++) {
        fc->expected_bms[i] = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
        if (!fc->expected_bms[i]) {
            ret = -ENOMEM;
            goto out;
        }
        if (i == 0)
            fc->expected_bms[0][0] |= 1U;
    }

    /* 1. Scan VTX_TREE_REF and populate expected reference tallies. */
    memset(&cur_k, 0, sizeof(cur_k));
    first = 1;
    for (;;) {
        __u8 type;
        bytes = sizeof(fc->val_buf);
        ret = vtx_rw_next(&fs->ref_tree, &cur_k, first, &found_k,
                          fc->val_buf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            goto err;
        first = 0;
        cur_k = found_k;
        type = VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type));
        if (type == VTX_TYPE_REF) {
            const struct vtx_ref_item *r =
                (const struct vtx_ref_item *)fc->val_buf;
            struct vtx_key k_rev;
            struct vtx_alloc_by_vblk_item rev;
            size_t rbytes = sizeof(rev);
            struct fsck_ref_tally *rt;
            int rev_bad;

            if (bytes != sizeof(*r) || vtx_cpu64(r->refcount) == 0) {
                ret = -EUCLEAN;
                goto err;
            }
            if (fc->num_refs == fc->refs_capacity) {
                ret = fsck_grow_refs(fc);
                if (ret)
                    goto err;
            }
            k_rev.oid_and_type =
                vtx_le64(VTX_MAKE_OID_TYPE(vtx_cpu64(r->start_vblk),
                                           VTX_TYPE_ALLOC_BY_VBLK));
            k_rev.offset = 0;
            rev_bad = (vtx_rw_lookup(&fs->ref_tree, &k_rev, &rev, &rbytes) != 0 ||
                       rbytes != sizeof(rev) ||
                       vtx_cpu64(rev.allocation_id) != vtx_cpu64(r->allocation_id) ||
                       vtx_cpu32(rev.block_count) != vtx_cpu32(r->block_count));
            if (rev_bad && dry_run < 0) {
                ret = -EUCLEAN;
                goto err;
            }
            rt = &fc->refs[fc->num_refs++];
            rt->allocation_id = vtx_cpu64(r->allocation_id);
            rt->start_vblk = vtx_cpu64(r->start_vblk);
            rt->block_count = vtx_cpu32(r->block_count);
            rt->flags = vtx_cpu32(r->flags);
            rt->expected_refs = vtx_cpu64(r->refcount);
            rt->observed_refs = 0;
            rt->rev_corrupt = rev_bad;
            fc->report.ref_items_verified++;

            ret = mark_expected_bm(fc, rt->start_vblk, rt->block_count);
            if (ret)
                goto err;
        }
    }

    /* 2. Scan VTX_TREE_SFQ and ensure disjointness from REF. */
    memset(&cur_k, 0, sizeof(cur_k));
    first = 1;
    for (;;) {
        bytes = sizeof(fc->val_buf);
        ret = vtx_rw_next(&fs->sfq_tree, &cur_k, first, &found_k,
                          fc->val_buf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            goto err;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) == VTX_TYPE_SFQ) {
            const struct vtx_sfq_entry *s =
                (const struct vtx_sfq_entry *)fc->val_buf;
            if (bytes != sizeof(*s) ||
                !vtx_sfq_range_valid(s, fs->vag.total_vags)) {
                ret = -EUCLEAN;
                goto err;
            }
            /* Narrowing to u32 is safe only after the 64-bit guard. */
            ret = mark_expected_bm(fc, vtx_cpu64(s->start_vblk),
                                   (__u32)vtx_cpu64(s->block_count));
            if (ret)
                goto err;
            fc->report.sfq_entries_verified++;
        }
    }

    /* 3. Walk all 6 system B+Trees. */
    {
        const struct vtx_rw_tree *sys_trees[6] = {
            &fs->omap_meta_tree, &fs->root_tree, &fs->ref_tree,
            &fs->reaper_tree, &fs->sfq_tree, &fs->uuid_tree
        };
        for (i = 0; i < 6; i++) {
            ret = walk_and_verify_tree(fc, sys_trees[i],
                                       sys_trees[i]->root_oid,
                                       sys_trees[i]->root_vblk,
                                       sys_trees[i]->root_level, 0, 1, NULL);
            if (ret)
                goto err;
        }
    }

    /* 4. Enumerate all subvolumes in VTX_TREE_ROOT and verify each one. */
    memset(&cur_k, 0, sizeof(cur_k));
    first = 1;
    for (;;) {
        bytes = sizeof(fc->val_buf);
        ret = vtx_rw_next(&fs->root_tree, &cur_k, first, &found_k,
                          fc->val_buf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            goto err;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) ==
            VTX_TYPE_SUBVOL_ROOT) {
            if (bytes != sizeof(struct vtx_subvol_root_item)) {
                ret = -EUCLEAN;
                goto err;
            }
            if (num_subvols == fc->subvols_capacity) {
                ret = fsck_grow_subvols(fc, num_subvols);
                if (ret)
                    goto err;
            }
            fc->subvol_ids[num_subvols++] =
                VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type));
        }
    }

    for (i = 0; i < num_subvols; i++) {
        ret = verify_subvolume_items(fc, fc->subvol_ids[i]);
        if (ret)
            goto err;
        fc->report.subvolumes_checked++;
    }
    if (num_subvols > 0)
        (void)vtx_subvol_switch(fs, fc->subvol_ids[0]);
    else
        fs->view_only = 0;

    /* 5. Count Reaper jobs. */
    memset(&cur_k, 0, sizeof(cur_k));
    first = 1;
    for (;;) {
        bytes = sizeof(fc->val_buf);
        ret = vtx_rw_next(&fs->reaper_tree, &cur_k, first, &found_k,
                          fc->val_buf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            goto err;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) ==
            VTX_TYPE_REAPER_JOB) {
            if (bytes != sizeof(struct vtx_reaper_job)) {
                ret = -EUCLEAN;
                goto err;
            }
            fc->report.reaper_jobs_checked++;
        }
    }

    /* 6. Verify VAG bitmaps match REF union SFQ union {0} bit-for-bit.
     * We MUST verify and repair bitmaps BEFORE mutating trees in Step 7.
     * If the bitmap is corrupt on disk, repairing it in memory first ensures
     * subsequent tree operations (like REF item updates) allocate from the
     * correct free-block pool without double-allocating blocks.
     */
    for (i = 0; i < fs->vag.total_vags; i++) {
        if (memcmp(fs->vag.bitmaps[i], fc->expected_bms[i],
                   VTXFS_BLOCK_SIZE) != 0) {
            fc->report.errors_found++;
            if (dry_run == 1) {
                fc->report.repairs_planned++;
            } else if (dry_run == 0) {
                __u32 free_blks = 0, b;
                memcpy(fs->vag.bitmaps[i], fc->expected_bms[i], VTXFS_BLOCK_SIZE);
                vtx_vag_summary_invalidate(&fs->vag);
                for (b = 0; b < VTXFS_VAG_BLOCKS; b++) {
                    if ((fc->expected_bms[i][b >> 3] & (1U << (b & 7U))) == 0)
                        free_blks++;
                }
                fs->vag.descs[i].free_blocks = vtx_le32(free_blks);
                fs->vag.descs[i].bitmap_crc32c =
                    vtx_le32(vtx_crc32c(fs->vag.bitmaps[i], VTXFS_BLOCK_SIZE));
                fc->report.repairs_performed++;
                need_commit = 1;
            } else {
                ret = -EUCLEAN;
                goto err;
            }
        }
    }

    /* 7. Verify and repair reverse-allocation index and REF refcounts. */
    fs->view_only = 0;
    fs->active_subvol_id = 0;
    for (i = 0; i < fc->num_refs; i++) {
        if (fc->refs[i].rev_corrupt) {
            fc->report.errors_found++;
            if (dry_run == 1) {
                fc->report.repairs_planned++;
            } else if (dry_run == 0) {
                struct vtx_key k_rev;
                struct vtx_alloc_by_vblk_item fixed_rev;

                k_rev.oid_and_type =
                    vtx_le64(VTX_MAKE_OID_TYPE(fc->refs[i].start_vblk,
                                               VTX_TYPE_ALLOC_BY_VBLK));
                k_rev.offset = 0;
                memset(&fixed_rev, 0, sizeof(fixed_rev));
                fixed_rev.allocation_id = vtx_le64(fc->refs[i].allocation_id);
                fixed_rev.block_count = vtx_le32(fc->refs[i].block_count);
                fixed_rev.flags = vtx_le32(fc->refs[i].flags);
                ret = vtx_rw_upsert(&fs->ref_tree, &k_rev, &fixed_rev,
                                    sizeof(fixed_rev));
                if (ret)
                    goto err;
                fc->refs[i].rev_corrupt = 0;
                fc->report.repairs_performed++;
                need_commit = 1;
            } else {
                ret = -EUCLEAN;
                goto err;
            }
        }
        if (fc->refs[i].observed_refs != fc->refs[i].expected_refs) {
            fc->report.errors_found++;
            if (dry_run == 1) {
                fc->report.repairs_planned++;
            } else if (dry_run == 0) {
                struct vtx_key k_ref;
                struct vtx_ref_item r_it;
                size_t rbytes = sizeof(r_it);

                k_ref.oid_and_type =
                    vtx_le64(VTX_MAKE_OID_TYPE(fc->refs[i].allocation_id,
                                               VTX_TYPE_REF));
                k_ref.offset = 0;
                ret = vtx_rw_lookup(&fs->ref_tree, &k_ref, &r_it, &rbytes);
                if (ret == 0 && rbytes == sizeof(r_it)) {
                    if (fc->refs[i].observed_refs == 0) {
                        /* Refcount reached 0: delete REF + ALLOC_BY_VBLK and enroll in SFQ */
                        struct vtx_key k_rev, k_sfq;
                        struct vtx_sfq_entry sfq_it;
                        __u64 start_vblk = vtx_cpu64(r_it.start_vblk);
                        __u32 count = vtx_cpu32(r_it.block_count);
                        __u32 flags = vtx_cpu32(r_it.flags);

                        k_rev.oid_and_type =
                            vtx_le64(VTX_MAKE_OID_TYPE(start_vblk, VTX_TYPE_ALLOC_BY_VBLK));
                        k_rev.offset = 0;

                        ret = vtx_rw_delete(&fs->ref_tree, &k_ref);
                        if (ret)
                            goto err;
                        ret = vtx_rw_delete(&fs->ref_tree, &k_rev);
                        if (ret && ret != -ENOENT)
                            goto err;

                        k_sfq.oid_and_type =
                            vtx_le64(VTX_MAKE_OID_TYPE(start_vblk, VTX_TYPE_SFQ));
                        k_sfq.offset = vtx_le64(fs->current_xid);
                        memset(&sfq_it, 0, sizeof(sfq_it));
                        sfq_it.start_vblk = vtx_le64(start_vblk);
                        sfq_it.block_count = vtx_le64(count);
                        sfq_it.retired_xid = vtx_le64(fs->current_xid);
                        sfq_it.flags = vtx_le32((flags & VTX_REF_FLAG_META)
                                                ? VTX_SFQ_FLAG_META : 0U);
                        ret = vtx_rw_upsert(&fs->sfq_tree, &k_sfq, &sfq_it, sizeof(sfq_it));
                        if (ret)
                            goto err;
                    } else {
                        r_it.refcount = vtx_le64(fc->refs[i].observed_refs);
                        ret = vtx_rw_replace(&fs->ref_tree, &k_ref, &r_it,
                                             sizeof(r_it));
                        if (ret)
                            goto err;
                    }
                    fc->refs[i].expected_refs = fc->refs[i].observed_refs;
                    fc->report.repairs_performed++;
                    need_commit = 1;
                } else {
                    ret = -EUCLEAN;
                    goto err;
                }
            } else {
                ret = -EUCLEAN;
                goto err;
            }
        }
    }

    if (need_commit && dry_run == 0) {
        fs->view_only = 0;
        fs->active_subvol_id = 0;
        fs->cp_blk.hdr.cp_flags &= ~vtx_le32(VTXFS_CP_FLAG_FSCK_REQUIRED);
        fs->cp_blk.hdr.cp_flags |= vtx_le32(VTXFS_CP_FLAG_CLEAN_UNMOUNT);
        ret = vtx_sync_checkpoint(fs);
        if (ret)
            goto err;
        fc->report.committed_xid = fs->committed_xid;
    }

    if (dry_run == 1 && fc->report.errors_found > 0)
        ret = -EUCLEAN;
    else
        ret = 0;

    goto out;

err:
    fc->report.errors_found++;
out:
    if (report_out)
        *report_out = fc->report;
    for (i = 0; i < VTX_MAX_SUPPORTED_VAGS; i++) {
        if (fc->expected_bms[i])
            vtx_free(fc->expected_bms[i]);
    }
    vtx_free(fc->refs);
    vtx_free(fc->subvol_ids);
    vtx_fs_free(fs);
    vtx_free(fc);
    return ret;
}

int vtx_fsck(struct vtx_bdev *bdev, struct vtx_fsck_report *report_out)
{
    return vtx_fsck_repair(bdev, -1, report_out);
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(vtx_fsck);
EXPORT_SYMBOL_GPL(vtx_fsck_repair);
#endif
