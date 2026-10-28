/* SPDX-License-Identifier: GPL-2.0-only */
#include "vtxfs_btree_rw.h"
#include "vtxfs_btree_ro.h"
#include "vtxfs_leaf.h"
#include "vtxfs_internal.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define RW_MAX_LEVEL 8

struct rw_frame {
    __u64 oid;
    __u64 vblk;
    void *wbuf;
    __u32 slot;
    __u32 nitems;
};

static int tree_params_valid(const struct vtx_rw_tree *t, int for_write)
{
    if (!t || !t->ops || !t->ops->read_node || !t->ops->release_node ||
        !vtx_oid_valid(t->root_oid) || !VTXFS_VALID_VBLK(t->root_vblk) ||
        t->node_bytes < VTXFS_MIN_NODE_SIZE ||
        t->node_bytes > VTXFS_MAX_NODE_SIZE ||
        (t->node_bytes & (t->node_bytes - 1U)) != 0 ||
        t->tree_type == 0 || t->tree_type >= VTX_TREE_MAX ||
        t->root_level >= RW_MAX_LEVEL || !t->tx_xid)
        return 0;
    if (!t->is_physical && !t->ops->omap_resolve)
        return 0;
    if (for_write) {
        if (!t->ops->cow_node || !t->ops->alloc_node || !t->ops->free_node ||
            !t->scratch || t->scratch_bytes < (size_t)t->node_bytes * 2U)
            return 0;
        if (!t->is_physical && (!t->ops->omap_set || !t->ops->omap_del))
            return 0;
    }
    return 1;
}

static __u32 child_slot(const struct vtx_node_hdr *h,
                        const struct vtx_key *key)
{
    __u32 lo = 0, hi = vtx_cpu16(h->nitems);
    while (lo < hi) {
        __u32 mid = lo + (hi - lo) / 2U;
        if (vtx_key_cmp(&h->entries[mid].key, key) <= 0)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo ? lo - 1U : 0U;
}

static __u32 leaf_bound(const struct vtx_node_hdr *h,
                        const struct vtx_key *key, int inclusive)
{
    __u32 lo = 0, hi = vtx_cpu16(h->nitems);
    while (lo < hi) {
        __u32 mid = lo + (hi - lo) / 2U;
        int cmp = vtx_key_cmp(&h->entries[mid].key, key);
        if (inclusive ? (cmp < 0) : (cmp <= 0))
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static int verify_node(const struct vtx_rw_tree *t, const void *ptr,
                       size_t bytes, __u64 oid, __u64 vblk, int level,
                       __u64 exact_xid, int is_root,
                       const struct vtx_key *expected_min)
{
    const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)ptr;
    __u16 flags;

    if (!ptr || bytes != t->node_bytes ||
        !vtx_node_slots_valid(h, t->node_bytes))
        return -EUCLEAN;
    flags = vtx_cpu16(h->flags);
    if (vtx_cpu64(h->oid) != oid ||
        vtx_cpu64(h->self_vblk) != vblk ||
        vtx_cpu64(h->owner_id) != t->owner_id ||
        vtx_cpu64(h->xid) == 0 ||
        vtx_cpu64(h->xid) > t->tx_xid ||
        (exact_xid && vtx_cpu64(h->xid) != exact_xid) ||
        h->tree_type != t->tree_type ||
        h->level != level ||
        !!(flags & VTX_NODE_FLAG_ROOT) != !!is_root)
        return -EUCLEAN;
    if (expected_min &&
        (!vtx_cpu16(h->nitems) ||
         vtx_key_cmp(expected_min, &h->entries[0].key) != 0))
        return -EUCLEAN;
    if (level == 0) {
        if (!(flags & VTX_NODE_FLAG_LEAF) ||
            (flags & (VTX_NODE_FLAG_VIRTUAL_OID |
                      VTX_NODE_FLAG_PHYSICAL_VBLK)))
            return -EUCLEAN;
    } else {
        __u16 req_flag = t->is_physical ? VTX_NODE_FLAG_PHYSICAL_VBLK
                                        : VTX_NODE_FLAG_VIRTUAL_OID;
        if ((flags & VTX_NODE_FLAG_LEAF) ||
            !(flags & req_flag) ||
            !vtx_cpu16(h->nitems))
            return -EUCLEAN;
    }
    return 0;
}

int vtx_rw_lookup(const struct vtx_rw_tree *tree,
                  const struct vtx_key *key,
                  void *value, size_t *value_bytes)
{
    struct vtx_ro_ops ro_ops;
    struct vtx_ro_tree ro;

    if (!tree_params_valid(tree, 0))
        return -EINVAL;
    ro_ops.read_node = tree->ops->read_node;
    ro_ops.release_node = tree->ops->release_node;
    ro_ops.omap_resolve = tree->ops->omap_resolve;

    ro.ops = &ro_ops;
    ro.ctx = tree->ctx;
    ro.view_id = tree->view_id;
    ro.root_oid = tree->root_oid;
    ro.root_vblk = tree->root_vblk;
    ro.max_xid = tree->tx_xid;
    ro.owner_id = tree->owner_id;
    ro.node_bytes = tree->node_bytes;
    ro.tree_type = tree->tree_type;
    ro.root_level = tree->root_level;
    return vtx_ro_lookup(&ro, key, value, value_bytes);
}

static int resolve_child_from_entry(const struct vtx_rw_tree *t,
                                    const void *node, __u32 slot,
                                    __u64 *child_oid, __u64 *child_vblk,
                                    __u64 *exact_xid,
                                    struct vtx_key *sep_key)
{
    const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)node;
    const struct vtx_node_entry *e = &h->entries[slot];
    struct vtx_key_ptr_val ptr;
    size_t off = (size_t)vtx_cpu16(e->slot.val_off8) * 8U;
    int ret;

    if (vtx_cpu16(e->slot.val_len) != sizeof(ptr))
        return -EUCLEAN;
    memcpy(&ptr, (const __u8 *)node + off, sizeof(ptr));
    *child_oid = vtx_cpu64(ptr.child_oid);
    if (!vtx_oid_valid(*child_oid))
        return -EUCLEAN;
    if (sep_key)
        *sep_key = e->key;
    if (t->is_physical) {
        *child_vblk = vtx_cpu64(ptr.hint_vblk);
        *exact_xid = vtx_cpu64(ptr.hint_xid);
        if (!VTXFS_VALID_VBLK(*child_vblk) || !*exact_xid)
            return -EUCLEAN;
    } else {
        ret = t->ops->omap_resolve(t->ctx, t->view_id, *child_oid, child_vblk);
        if (ret)
            return ret;
        if (!VTXFS_VALID_VBLK(*child_vblk))
            return -EUCLEAN;
        *exact_xid = 0;
    }
    return 0;
}

int vtx_rw_next(const struct vtx_rw_tree *t,
                const struct vtx_key *start_key, int inclusive,
                struct vtx_key *found_key,
                void *value, size_t *value_bytes)
{
    struct rw_frame path[RW_MAX_LEVEL];
    __u64 oid, vblk, exact_xid = 0;
    struct vtx_key sep_key;
    int has_sep = 0, lvl, ascend, ret;

    if (!tree_params_valid(t, 0) || !start_key || !found_key)
        return -EINVAL;

    memset(path, 0, sizeof(path));
    oid = t->root_oid;
    vblk = t->root_vblk;

    for (lvl = t->root_level; lvl >= 0; lvl--) {
        const void *node = NULL;
        const struct vtx_node_hdr *h;
        size_t bytes = 0;
        int verified = 0;

        ret = t->ops->read_node(t->ctx, vblk, &node, &bytes, &verified);
        if (ret)
            return ret;
        if (!verified ||
            verify_node(t, node, bytes, oid, vblk, lvl, exact_xid,
                        lvl == t->root_level,
                        has_sep ? &sep_key : NULL) != 0) {
            t->ops->release_node(t->ctx, node);
            return -EUCLEAN;
        }
        h = (const struct vtx_node_hdr *)node;
        path[lvl].oid = oid;
        path[lvl].vblk = vblk;
        path[lvl].nitems = vtx_cpu16(h->nitems);

        if (lvl == 0) {
            __u32 s = leaf_bound(h, start_key, inclusive);
            if (s < path[0].nitems) {
                __u16 len = vtx_cpu16(h->entries[s].slot.val_len);
                __u32 off = (__u32)vtx_cpu16(h->entries[s].slot.val_off8) * 8U;
                *found_key = h->entries[s].key;
                if (value_bytes) {
                    if (len > *value_bytes || (len && !value)) {
                        *value_bytes = len;
                        t->ops->release_node(t->ctx, node);
                        return -ENOBUFS;
                    }
                    if (len && value)
                        memcpy(value, (const __u8 *)node + off, len);
                    *value_bytes = len;
                }
                t->ops->release_node(t->ctx, node);
                return 0;
            }
            t->ops->release_node(t->ctx, node);
            break;
        } else {
            __u32 s = child_slot(h, start_key);
            path[lvl].slot = s;
            ret = resolve_child_from_entry(t, node, s, &oid, &vblk,
                                           &exact_xid, &sep_key);
            t->ops->release_node(t->ctx, node);
            if (ret)
                return ret;
            has_sep = 1;
        }
    }

    /* Advance to the next sibling subtree at the lowest ancestor with more slots. */
    for (ascend = 1; ascend <= t->root_level; ascend++) {
        if (path[ascend].slot + 1U < path[ascend].nitems) {
            const void *node = NULL;
            size_t bytes = 0;
            int verified = 0;

            path[ascend].slot++;
            ret = t->ops->read_node(t->ctx, path[ascend].vblk, &node,
                                    &bytes, &verified);
            if (ret)
                return ret;
            if (!verified || bytes != t->node_bytes) {
                t->ops->release_node(t->ctx, node);
                return -EUCLEAN;
            }
            ret = resolve_child_from_entry(t, node, path[ascend].slot,
                                           &oid, &vblk, &exact_xid, &sep_key);
            t->ops->release_node(t->ctx, node);
            if (ret)
                return ret;
            has_sep = 1;

            for (lvl = ascend - 1; lvl >= 0; lvl--) {
                const struct vtx_node_hdr *h;
                ret = t->ops->read_node(t->ctx, vblk, &node, &bytes, &verified);
                if (ret)
                    return ret;
                if (!verified ||
                    verify_node(t, node, bytes, oid, vblk, lvl, exact_xid,
                                0, has_sep ? &sep_key : NULL) != 0) {
                    t->ops->release_node(t->ctx, node);
                    return -EUCLEAN;
                }
                h = (const struct vtx_node_hdr *)node;
                if (lvl == 0) {
                    __u16 len = vtx_cpu16(h->entries[0].slot.val_len);
                    __u32 off = (__u32)vtx_cpu16(h->entries[0].slot.val_off8) * 8U;
                    *found_key = h->entries[0].key;
                    if (value_bytes) {
                        if (len > *value_bytes || (len && !value)) {
                            *value_bytes = len;
                            t->ops->release_node(t->ctx, node);
                            return -ENOBUFS;
                        }
                        if (len && value)
                            memcpy(value, (const __u8 *)node + off, len);
                        *value_bytes = len;
                    }
                    t->ops->release_node(t->ctx, node);
                    return 0;
                }
                ret = resolve_child_from_entry(t, node, 0, &oid, &vblk,
                                               &exact_xid, &sep_key);
                t->ops->release_node(t->ctx, node);
                if (ret)
                    return ret;
            }
        }
    }
    return -ENOENT;
}

static void update_parent_ptr_in_place(void *parent_buf, __u32 slot,
                                       __u64 child_oid, __u64 child_vblk,
                                       __u64 tx_xid)
{
    struct vtx_node_hdr *ph = (struct vtx_node_hdr *)parent_buf;
    struct vtx_key_ptr_val ptr;
    size_t off = (size_t)vtx_cpu16(ph->entries[slot].slot.val_off8) * 8U;

    ptr.child_oid = vtx_le64(child_oid);
    ptr.hint_vblk = vtx_le64(child_vblk);
    ptr.hint_xid = vtx_le64(tx_xid);
    memcpy((__u8 *)parent_buf + off, &ptr, sizeof(ptr));
    memset(ph->checksum, 0, sizeof(ph->checksum));
}

static void propagate_min_keys(struct vtx_rw_tree *t,
                               struct rw_frame path[RW_MAX_LEVEL],
                               int from_level)
{
    int lvl;
    for (lvl = from_level; lvl < t->root_level; lvl++) {
        struct vtx_node_hdr *ch = (struct vtx_node_hdr *)path[lvl].wbuf;
        struct vtx_node_hdr *ph = (struct vtx_node_hdr *)path[lvl + 1].wbuf;
        __u32 s = path[lvl + 1].slot;
        if (!vtx_cpu16(ch->nitems))
            break;
        if (vtx_key_cmp(&ph->entries[s].key, &ch->entries[0].key) == 0)
            break;
        ph->entries[s].key = ch->entries[0].key;
        memset(ph->checksum, 0, sizeof(ph->checksum));
    }
}

static int descend_and_cow(struct vtx_rw_tree *t, const struct vtx_key *key,
                           struct rw_frame path[RW_MAX_LEVEL])
{
    __u64 oid = t->root_oid;
    __u64 vblk = t->root_vblk;
    __u64 exact_xid = 0;
    struct vtx_key sep_key;
    int has_sep = 0, lvl, ret;

    memset(path, 0, sizeof(struct rw_frame) * RW_MAX_LEVEL);
    for (lvl = t->root_level; lvl >= 0; lvl--) {
        const void *ro_node = NULL;
        size_t bytes = 0;
        int verified = 0;
        __u64 new_vblk = 0;
        void *wbuf = NULL;
        struct vtx_node_hdr *wh;

        ret = t->ops->read_node(t->ctx, vblk, &ro_node, &bytes, &verified);
        if (ret)
            return ret;
        if (!verified ||
            verify_node(t, ro_node, bytes, oid, vblk, lvl, exact_xid,
                        lvl == t->root_level,
                        has_sep ? &sep_key : NULL) != 0) {
            t->ops->release_node(t->ctx, ro_node);
            return -EUCLEAN;
        }
        t->ops->release_node(t->ctx, ro_node);

        ret = t->ops->cow_node(t->ctx, t->tree_type, (__u8)lvl, oid, vblk,
                               &new_vblk, &wbuf);
        if (ret)
            return ret;
        wh = (struct vtx_node_hdr *)wbuf;
        wh->owner_id = vtx_le64(t->owner_id);
        wh->xid = vtx_le64(t->tx_xid);
        wh->self_vblk = vtx_le64(new_vblk);
        memset(wh->checksum, 0, sizeof(wh->checksum));

        if (!t->is_physical) {
            ret = t->ops->omap_set(t->ctx, t->view_id, oid, new_vblk);
            if (ret)
                return ret;
        }
        if (lvl == t->root_level)
            t->root_vblk = new_vblk;
        else
            update_parent_ptr_in_place(path[lvl + 1].wbuf, path[lvl + 1].slot,
                                       oid, new_vblk, t->tx_xid);

        path[lvl].oid = oid;
        path[lvl].vblk = new_vblk;
        path[lvl].wbuf = wbuf;
        path[lvl].nitems = vtx_cpu16(wh->nitems);

        if (lvl > 0) {
            __u32 s = child_slot(wh, key);
            path[lvl].slot = s;
            ret = resolve_child_from_entry(t, wbuf, s, &oid, &vblk,
                                           &exact_xid, &sep_key);
            if (ret)
                return ret;
            has_sep = 1;
        }
    }
    return 0;
}

static int insert_with_splits(struct vtx_rw_tree *t,
                              struct rw_frame path[RW_MAX_LEVEL],
                              const struct vtx_key *key,
                              const void *value, __u16 value_bytes)
{
    void *leaf = path[0].wbuf;
    struct vtx_node_hdr *lh = (struct vtx_node_hdr *)leaf;
    __u64 r_oid = 0, r_vblk = 0;
    void *r_buf = NULL;
    struct vtx_node_hdr *rh;
    struct vtx_key promo_key;
    struct vtx_key_ptr_val promo_ptr;
    int lvl, ret;

    ret = vtx_leaf_insert(leaf, t->node_bytes, key, value, value_bytes,
                          t->scratch, t->node_bytes);
    if (ret == 0) {
        lh->owner_id = vtx_le64(t->owner_id);
        propagate_min_keys(t, path, 0);
        return 0;
    }
    if (ret != -ENOSPC)
        return ret;

    /* Split leaf at level 0. */
    if (t->root_level + 1 >= RW_MAX_LEVEL && t->root_level == 7)
        return -ENOSPC;

    ret = t->ops->alloc_node(t->ctx, t->tree_type, 0,
                             &r_oid, &r_vblk, &r_buf);
    if (ret)
        return ret;
    ret = vtx_leaf_init(r_buf, t->node_bytes, r_oid, t->tx_xid, r_vblk,
                        t->tree_type, VTX_NODE_FLAG_LEAF);
    if (ret)
        return ret;
    rh = (struct vtx_node_hdr *)r_buf;
    rh->owner_id = vtx_le64(t->owner_id);

    if (t->root_level == 0)
        lh->flags = vtx_le16(VTX_NODE_FLAG_LEAF);

    ret = vtx_leaf_split_insert(leaf, r_buf, t->node_bytes, key,
                                value, value_bytes,
                                t->scratch, (size_t)t->node_bytes * 2U);
    if (ret) {
        if (t->root_level == 0)
            lh->flags = vtx_le16(VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT);
        t->ops->free_node(t->ctx, t->tree_type, 0, r_oid, r_vblk);
        return ret;
    }
    if (!t->is_physical) {
        ret = t->ops->omap_set(t->ctx, t->view_id, r_oid, r_vblk);
        if (ret)
            return ret;
    }
    propagate_min_keys(t, path, 0);

    promo_key = rh->entries[0].key;
    promo_ptr.child_oid = vtx_le64(r_oid);
    promo_ptr.hint_vblk = vtx_le64(r_vblk);
    promo_ptr.hint_xid = vtx_le64(t->tx_xid);

    for (lvl = 1; lvl <= t->root_level; lvl++) {
        void *cur = path[lvl].wbuf;
        struct vtx_node_hdr *ch = (struct vtx_node_hdr *)cur;
        __u16 mode_flag = t->is_physical ? VTX_NODE_FLAG_PHYSICAL_VBLK
                                         : VTX_NODE_FLAG_VIRTUAL_OID;

        ret = vtx_internal_insert(cur, t->node_bytes, &promo_key, &promo_ptr,
                                  t->scratch, t->node_bytes);
        if (ret == 0) {
            ch->owner_id = vtx_le64(t->owner_id);
            propagate_min_keys(t, path, lvl);
            return 0;
        }
        if (ret != -ENOSPC)
            return ret;

        ret = t->ops->alloc_node(t->ctx, t->tree_type, (__u8)lvl,
                                 &r_oid, &r_vblk, &r_buf);
        if (ret)
            return ret;
        ret = vtx_internal_init(r_buf, t->node_bytes, r_oid, t->tx_xid,
                                r_vblk, t->tree_type, mode_flag, (__u8)lvl);
        if (ret)
            return ret;
        rh = (struct vtx_node_hdr *)r_buf;
        rh->owner_id = vtx_le64(t->owner_id);

        if (lvl == t->root_level)
            ch->flags = vtx_le16(mode_flag);

        ret = vtx_internal_split_insert(cur, r_buf, t->node_bytes,
                                        &promo_key, &promo_ptr,
                                        t->scratch,
                                        (size_t)t->node_bytes * 2U);
        if (ret) {
            if (lvl == t->root_level)
                ch->flags = vtx_le16(mode_flag | VTX_NODE_FLAG_ROOT);
            t->ops->free_node(t->ctx, t->tree_type, (__u8)lvl, r_oid, r_vblk);
            return ret;
        }
        if (!t->is_physical) {
            ret = t->ops->omap_set(t->ctx, t->view_id, r_oid, r_vblk);
            if (ret)
                return ret;
        }
        propagate_min_keys(t, path, lvl);
        promo_key = rh->entries[0].key;
        promo_ptr.child_oid = vtx_le64(r_oid);
        promo_ptr.hint_vblk = vtx_le64(r_vblk);
        promo_ptr.hint_xid = vtx_le64(t->tx_xid);
    }

    /* Old root was split; allocate a new root at root_level + 1. */
    {
        __u8 new_lvl = (__u8)(t->root_level + 1U);
        __u16 root_flags = VTX_NODE_FLAG_ROOT |
                           (t->is_physical ? VTX_NODE_FLAG_PHYSICAL_VBLK
                                           : VTX_NODE_FLAG_VIRTUAL_OID);
        __u64 nr_oid = 0, nr_vblk = 0;
        void *nr_buf = NULL;
        struct vtx_node_hdr *nrh;
        struct vtx_node_hdr *left_root =
            (struct vtx_node_hdr *)path[t->root_level].wbuf;
        struct vtx_key left_key = left_root->entries[0].key;
        struct vtx_key_ptr_val left_ptr;

        if (new_lvl >= RW_MAX_LEVEL)
            return -ENOSPC;
        ret = t->ops->alloc_node(t->ctx, t->tree_type, new_lvl,
                                 &nr_oid, &nr_vblk, &nr_buf);
        if (ret)
            return ret;
        ret = vtx_internal_init(nr_buf, t->node_bytes, nr_oid, t->tx_xid,
                                nr_vblk, t->tree_type, root_flags, new_lvl);
        if (ret)
            return ret;
        nrh = (struct vtx_node_hdr *)nr_buf;
        nrh->owner_id = vtx_le64(t->owner_id);

        left_ptr.child_oid = vtx_le64(path[t->root_level].oid);
        left_ptr.hint_vblk = vtx_le64(path[t->root_level].vblk);
        left_ptr.hint_xid = vtx_le64(t->tx_xid);

        ret = vtx_internal_insert(nr_buf, t->node_bytes, &left_key, &left_ptr,
                                  t->scratch, t->node_bytes);
        if (ret)
            return ret;
        ret = vtx_internal_insert(nr_buf, t->node_bytes, &promo_key, &promo_ptr,
                                  t->scratch, t->node_bytes);
        if (ret)
            return ret;
        nrh->owner_id = vtx_le64(t->owner_id);
        if (!t->is_physical) {
            ret = t->ops->omap_set(t->ctx, t->view_id, nr_oid, nr_vblk);
            if (ret)
                return ret;
        }
        t->root_oid = nr_oid;
        t->root_vblk = nr_vblk;
        t->root_level = new_lvl;
    }
    return 0;
}

int vtx_rw_insert(struct vtx_rw_tree *tree,
                  const struct vtx_key *key,
                  const void *value, __u16 value_bytes)
{
    struct rw_frame path[RW_MAX_LEVEL];
    __u32 slot = 0;
    int ret;

    if (!tree_params_valid(tree, 1) || !key || (value_bytes && !value))
        return -EINVAL;
    ret = descend_and_cow(tree, key, path);
    if (ret)
        return ret;
    ret = vtx_leaf_find(path[0].wbuf, tree->node_bytes, key, &slot);
    if (ret < 0)
        return ret;
    if (ret == 0)
        return -EEXIST;
    return insert_with_splits(tree, path, key, value, value_bytes);
}

int vtx_rw_replace(struct vtx_rw_tree *tree,
                   const struct vtx_key *key,
                   const void *value, __u16 value_bytes)
{
    struct rw_frame path[RW_MAX_LEVEL];
    int ret;

    if (!tree_params_valid(tree, 1) || !key || (value_bytes && !value))
        return -EINVAL;
    ret = descend_and_cow(tree, key, path);
    if (ret)
        return ret;
    ret = vtx_leaf_replace(path[0].wbuf, tree->node_bytes, key,
                           value, value_bytes, tree->scratch, tree->node_bytes);
    if (ret == 0)
        return 0;
    if (ret != -ENOSPC)
        return ret;
    /* Value grew beyond current leaf free space: delete then split-insert. */
    ret = vtx_leaf_delete(path[0].wbuf, tree->node_bytes, key,
                          tree->scratch, tree->node_bytes);
    if (ret)
        return ret;
    return insert_with_splits(tree, path, key, value, value_bytes);
}

int vtx_rw_upsert(struct vtx_rw_tree *tree,
                  const struct vtx_key *key,
                  const void *value, __u16 value_bytes)
{
    struct rw_frame path[RW_MAX_LEVEL];
    __u32 slot = 0;
    int ret;

    if (!tree_params_valid(tree, 1) || !key || (value_bytes && !value))
        return -EINVAL;
    ret = descend_and_cow(tree, key, path);
    if (ret)
        return ret;
    ret = vtx_leaf_find(path[0].wbuf, tree->node_bytes, key, &slot);
    if (ret < 0)
        return ret;
    if (ret == 0) {
        ret = vtx_leaf_replace(path[0].wbuf, tree->node_bytes, key,
                               value, value_bytes,
                               tree->scratch, tree->node_bytes);
        if (ret == 0 || ret != -ENOSPC)
            return ret;
        ret = vtx_leaf_delete(path[0].wbuf, tree->node_bytes, key,
                              tree->scratch, tree->node_bytes);
        if (ret)
            return ret;
    }
    return insert_with_splits(tree, path, key, value, value_bytes);
}

int vtx_rw_delete(struct vtx_rw_tree *t,
                  const struct vtx_key *key)
{
    struct rw_frame path[RW_MAX_LEVEL];
    struct vtx_node_hdr *lh;
    int lvl, ret;

    if (!tree_params_valid(t, 1) || !key)
        return -EINVAL;
    ret = descend_and_cow(t, key, path);
    if (ret)
        return ret;

    ret = vtx_leaf_delete(path[0].wbuf, t->node_bytes, key,
                          t->scratch, t->node_bytes);
    if (ret)
        return ret;

    lh = (struct vtx_node_hdr *)path[0].wbuf;
    lh->owner_id = vtx_le64(t->owner_id);
    if (vtx_cpu16(lh->nitems) > 0 || t->root_level == 0) {
        if (vtx_cpu16(lh->nitems) > 0)
            propagate_min_keys(t, path, 0);
        return 0;
    }

    /* Non-root leaf became empty: remove it and prune empty ancestors. */
    for (lvl = 0; lvl < t->root_level; lvl++) {
        struct vtx_node_hdr *ch = (struct vtx_node_hdr *)path[lvl].wbuf;
        struct vtx_node_hdr *ph = (struct vtx_node_hdr *)path[lvl + 1].wbuf;
        struct vtx_key del_key;

        if (vtx_cpu16(ch->nitems) > 0) {
            propagate_min_keys(t, path, lvl);
            break;
        }
        del_key = ph->entries[path[lvl + 1].slot].key;
        if (!t->is_physical) {
            ret = t->ops->omap_del(t->ctx, t->view_id, path[lvl].oid);
            if (ret)
                return ret;
        }
        ret = t->ops->free_node(t->ctx, t->tree_type, (__u8)lvl,
                                path[lvl].oid, path[lvl].vblk);
        if (ret)
            return ret;

        ret = vtx_internal_delete(path[lvl + 1].wbuf, t->node_bytes, &del_key,
                                  t->scratch, t->node_bytes);
        if (ret)
            return ret;
        ph->owner_id = vtx_le64(t->owner_id);
    }

    /* Contract root while root_level > 0 and root has a single child. */
    while (t->root_level > 0) {
        struct vtx_node_hdr *rh =
            (struct vtx_node_hdr *)path[t->root_level].wbuf;
        __u64 c_oid = 0, c_vblk = 0, c_new_vblk = 0, exact_xid = 0;
        void *c_wbuf = NULL;
        struct vtx_node_hdr *ch;

        if (vtx_cpu16(rh->nitems) != 1U)
            break;
        ret = resolve_child_from_entry(t, path[t->root_level].wbuf, 0,
                                       &c_oid, &c_vblk, &exact_xid, NULL);
        if (ret)
            return ret;
        ret = t->ops->cow_node(t->ctx, t->tree_type,
                               (__u8)(t->root_level - 1U),
                               c_oid, c_vblk, &c_new_vblk, &c_wbuf);
        if (ret)
            return ret;
        ch = (struct vtx_node_hdr *)c_wbuf;
        ch->flags = vtx_le16(vtx_cpu16(ch->flags) | VTX_NODE_FLAG_ROOT);
        ch->owner_id = vtx_le64(t->owner_id);
        ch->xid = vtx_le64(t->tx_xid);
        ch->self_vblk = vtx_le64(c_new_vblk);
        memset(ch->checksum, 0, sizeof(ch->checksum));

        if (!t->is_physical) {
            ret = t->ops->omap_set(t->ctx, t->view_id, c_oid, c_new_vblk);
            if (ret)
                return ret;
            ret = t->ops->omap_del(t->ctx, t->view_id,
                                   path[t->root_level].oid);
            if (ret)
                return ret;
        }
        ret = t->ops->free_node(t->ctx, t->tree_type, t->root_level,
                                path[t->root_level].oid,
                                path[t->root_level].vblk);
        if (ret)
            return ret;

        t->root_oid = c_oid;
        t->root_vblk = c_new_vblk;
        t->root_level--;
        path[t->root_level].oid = c_oid;
        path[t->root_level].vblk = c_new_vblk;
        path[t->root_level].wbuf = c_wbuf;
    }
    return 0;
}
