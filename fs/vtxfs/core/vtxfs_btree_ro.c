/* SPDX-License-Identifier: GPL-2.0-only */
#include "vtxfs_btree_ro.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

static int read_checked(const struct vtx_ro_tree *t,
                        __u64 oid, __u64 vblk, int level,
                        __u64 exact_xid, int is_root,
                        const void **out)
{
    const void *ptr = NULL;
    const struct vtx_node_hdr *h;
    size_t bytes = 0;
    __u16 flags;
    int verified = 0, ret;

    *out = NULL;
    ret = t->ops->read_node(t->ctx, vblk, &ptr, &bytes, &verified);
    if (ret)
        return ret;
    if (!ptr || !verified || bytes != t->node_bytes)
        goto corrupt;
    h = ptr;
    if (!vtx_node_slots_valid(h, t->node_bytes))
        goto corrupt;
    flags = vtx_cpu16(h->flags);
    if (vtx_cpu64(h->oid) != oid ||
        vtx_cpu64(h->self_vblk) != vblk ||
        vtx_cpu64(h->owner_id) != t->owner_id ||
        vtx_cpu64(h->xid) == 0 ||
        vtx_cpu64(h->xid) > t->max_xid ||
        (exact_xid && vtx_cpu64(h->xid) != exact_xid) ||
        h->tree_type != t->tree_type ||
        h->level != level ||
        !!(flags & VTX_NODE_FLAG_ROOT) != !!is_root)
        goto corrupt;

    if (level == 0) {
        if (!(flags & VTX_NODE_FLAG_LEAF) ||
            (flags & (VTX_NODE_FLAG_VIRTUAL_OID |
                      VTX_NODE_FLAG_PHYSICAL_VBLK)))
            goto corrupt;
    } else {
        __u32 i;
        if ((flags & VTX_NODE_FLAG_LEAF) ||
            (!!(flags & VTX_NODE_FLAG_VIRTUAL_OID) ==
             !!(flags & VTX_NODE_FLAG_PHYSICAL_VBLK)) ||
            !vtx_cpu16(h->nitems))
            goto corrupt;
        for (i = 0; i < vtx_cpu16(h->nitems); ++i) {
            struct vtx_key_ptr_val p;
            const struct vtx_slot *slot = &h->entries[i].slot;
            size_t off = (size_t)vtx_cpu16(slot->val_off8)*8U;
            if (vtx_cpu16(slot->val_len) != sizeof(p))
                goto corrupt;
            memcpy(&p, (const __u8 *)ptr+off, sizeof(p));
            if (!vtx_oid_valid(vtx_cpu64(p.child_oid)))
                goto corrupt;
            if (flags & VTX_NODE_FLAG_PHYSICAL_VBLK) {
                if (!VTXFS_VALID_VBLK(vtx_cpu64(p.hint_vblk)) ||
                    !vtx_cpu64(p.hint_xid))
                    goto corrupt;
            }
        }
    }
    *out = ptr;
    return 0;
corrupt:
    if (ptr)
        t->ops->release_node(t->ctx, ptr);
    return -EUCLEAN;
}

/* rightmost separator <= key; keys are the minima of child subtrees. */
static __u32 child_slot(const struct vtx_node_hdr *h,
                         const struct vtx_key *key)
{
    __u32 lo = 0, hi = vtx_cpu16(h->nitems);
    while (lo < hi) {
        __u32 mid = lo + (hi-lo)/2U;
        if (vtx_key_cmp(&h->entries[mid].key, key) <= 0)
            lo = mid+1U;
        else
            hi = mid;
    }
    return lo ? lo-1U : 0U;
}
static __u32 leaf_lower_bound(const struct vtx_node_hdr *h,
                               const struct vtx_key *key)
{
    __u32 lo = 0, hi = vtx_cpu16(h->nitems);
    while (lo < hi) {
        __u32 mid = lo+(hi-lo)/2U;
        if (vtx_key_cmp(&h->entries[mid].key, key) < 0)
            lo = mid+1U;
        else
            hi = mid;
    }
    return lo;
}

int vtx_ro_lookup(const struct vtx_ro_tree *t, const struct vtx_key *key,
                  void *value, size_t *value_bytes)
{
    __u64 oid, vblk, exact_xid = 0;
    struct vtx_key expected_min = {0};
    int check_min = 0;
    int level, ret, is_root = 1;

    if (!t || !key || !value_bytes || !t->ops ||
        !t->ops->read_node || !t->ops->release_node ||
        !vtx_oid_valid(t->root_oid) ||
        !VTXFS_VALID_VBLK(t->root_vblk) ||
        t->node_bytes < VTXFS_MIN_NODE_SIZE ||
        t->node_bytes > VTXFS_MAX_NODE_SIZE ||
        (t->node_bytes & (t->node_bytes-1U)) ||
        t->tree_type == 0 || t->tree_type >= VTX_TREE_MAX ||
        t->root_level >= 8 || !t->max_xid)
        return -EINVAL;

    oid = t->root_oid;
    vblk = t->root_vblk;
    for (level = t->root_level; level >= 0; --level) {
        const void *node;
        const struct vtx_node_hdr *h;
        ret = read_checked(t, oid, vblk, level, exact_xid,
                           is_root, &node);
        if (ret)
            return ret;
        h = node;
        if (check_min &&
            (!vtx_cpu16(h->nitems) ||
             vtx_key_cmp(&expected_min,&h->entries[0].key) != 0)) {
            t->ops->release_node(t->ctx,node);
            return -EUCLEAN;
        }
        if (level == 0) {
            __u32 slot = leaf_lower_bound(h,key);
            __u16 length;
            __u32 offset;
            if (slot >= vtx_cpu16(h->nitems) ||
                vtx_key_cmp(&h->entries[slot].key,key) != 0) {
                t->ops->release_node(t->ctx,node);
                return -ENOENT;
            }
            length = vtx_cpu16(h->entries[slot].slot.val_len);
            offset = (__u32)vtx_cpu16(h->entries[slot].slot.val_off8)*8U;
            if (length > *value_bytes || (length && !value)) {
                *value_bytes=length;
                t->ops->release_node(t->ctx,node);
                return -ENOBUFS;
            }
            if (length)
                memcpy(value, (const __u8 *)node+offset, length);
            *value_bytes=length;
            t->ops->release_node(t->ctx,node);
            return 0;
        } else {
            __u32 slot = child_slot(h,key);
            const struct vtx_node_entry *entry = &h->entries[slot];
            struct vtx_key_ptr_val ptr;
            __u16 flags = vtx_cpu16(h->flags);
            __u32 offset = (__u32)vtx_cpu16(entry->slot.val_off8)*8U;
            __u64 next_vblk = 0;
            memcpy(&ptr, (const __u8 *)node+offset, sizeof(ptr));
            oid = vtx_cpu64(ptr.child_oid);
            expected_min = entry->key;
            check_min = 1;
            if (flags & VTX_NODE_FLAG_PHYSICAL_VBLK) {
                next_vblk = vtx_cpu64(ptr.hint_vblk);
                exact_xid = vtx_cpu64(ptr.hint_xid);
            } else {
                /* An OMAP view is authoritative; NEVER accept a hint fallback. */
                if (!t->ops->omap_resolve) {
                    t->ops->release_node(t->ctx,node);
                    return -EOPNOTSUPP;
                }
                ret = t->ops->omap_resolve(t->ctx,t->view_id,
                                           oid,&next_vblk);
                if (ret) {
                    t->ops->release_node(t->ctx,node);
                    return ret;
                }
                exact_xid = 0;
            }
            t->ops->release_node(t->ctx,node);
            if (!VTXFS_VALID_VBLK(next_vblk))
                return -EUCLEAN;
            vblk=next_vblk;
            is_root=0;
        }
    }
    return -EUCLEAN;
}
