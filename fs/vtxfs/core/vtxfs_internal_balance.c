/* SPDX-License-Identifier: GPL-2.0-only */
/* In-memory, all-or-nothing, two-sibling slotted internal-node operations. */
#include "vtxfs_internal.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

enum ibal_action {
    IACT_SPLIT,
    IACT_SPLIT_INSERT,
    IACT_MERGE,
    IACT_REBALANCE
};

struct ipair_stream {
    const struct vtx_node_hdr *left, *right;
    __u32 nleft, nright, insert_at;
    int inserting;
    struct vtx_key key;
    struct vtx_key_ptr_val child;
};

struct ipair_item {
    struct vtx_node_entry entry;
    struct vtx_key_ptr_val child;
};

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    unsigned long x = (unsigned long)a, y = (unsigned long)b;
    if (!an || !bn)
        return 0;
    return x <= y ? y - x < an : x - y < bn;
}

static __u32 icount(const struct ipair_stream *p)
{
    return p->nleft + p->nright + (__u32)p->inserting;
}

static void iitem_at(const struct ipair_stream *p, __u32 idx,
                     struct ipair_item *out)
{
    const struct vtx_node_hdr *node;
    __u32 src = idx;
    memset(out, 0, sizeof(*out));
    if (p->inserting && idx == p->insert_at) {
        out->entry.key = p->key;
        out->child = p->child;
        return;
    }
    if (p->inserting && idx > p->insert_at)
        src--;
    if (src < p->nleft)
        node = p->left;
    else {
        node = p->right;
        src -= p->nleft;
    }
    out->entry = node->entries[src];
    memcpy(&out->child,
           (const __u8 *)node +
           (__u32)vtx_cpu16(out->entry.slot.val_off8) * 8U,
           sizeof(out->child));
}

static int validate_internal_sibling(const void *buffer, size_t bytes)
{
    const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)buffer;
    __u16 flags;
    __u32 i;
    if (!buffer || bytes < VTXFS_MIN_NODE_SIZE ||
        bytes > VTXFS_MAX_NODE_SIZE ||
        !vtx_node_slots_valid(h, (__u32)bytes))
        return 0;
    flags = vtx_cpu16(h->flags);
    if ((flags & VTX_NODE_FLAG_LEAF) ||
        !h->level || h->level > 7 ||
        (!!(flags & VTX_NODE_FLAG_VIRTUAL_OID) ==
         !!(flags & VTX_NODE_FLAG_PHYSICAL_VBLK)) ||
        !vtx_oid_valid(vtx_cpu64(h->oid)) ||
        !vtx_cpu64(h->xid) ||
        !VTXFS_VALID_VBLK(vtx_cpu64(h->self_vblk)))
        return 0;
    for (i = 0; i < vtx_cpu16(h->nitems); i++) {
        const struct vtx_slot *sl = &h->entries[i].slot;
        struct vtx_key_ptr_val c;
        if (vtx_cpu16(sl->val_len) != sizeof(c))
            return 0;
        memcpy(&c, (const __u8 *)buffer +
               (__u32)vtx_cpu16(sl->val_off8) * 8U, sizeof(c));
        if (!vtx_oid_valid(vtx_cpu64(c.child_oid)))
            return 0;
        if ((flags & VTX_NODE_FLAG_PHYSICAL_VBLK) &&
            (!VTXFS_VALID_VBLK(vtx_cpu64(c.hint_vblk)) ||
             !vtx_cpu64(c.hint_xid)))
            return 0;
    }
    return 1;
}

static int icompatible(const struct vtx_node_hdr *a,
                       const struct vtx_node_hdr *b)
{
    __u16 fa = vtx_cpu16(a->flags);
    __u16 fb = vtx_cpu16(b->flags);
    return a->level == b->level &&
           a->tree_type == b->tree_type &&
           fa == fb &&
           vtx_cpu64(a->owner_id) == vtx_cpu64(b->owner_id) &&
           vtx_cpu64(a->xid) == vtx_cpu64(b->xid) &&
           vtx_cpu64(a->oid) != vtx_cpu64(b->oid) &&
           vtx_cpu64(a->self_vblk) != vtx_cpu64(b->self_vblk);
}

static int ibuild(void *out_buffer, size_t bytes,
                  const struct vtx_node_hdr *identity,
                  const struct ipair_stream *p, __u32 start, __u32 number)
{
    struct vtx_node_hdr *out = (struct vtx_node_hdr *)out_buffer;
    size_t head = sizeof(*out) +
                  (size_t)number * sizeof(struct vtx_node_entry);
    size_t tail = bytes;
    __u32 i;

    if (number > 65535U || head > bytes)
        return -ENOSPC;
    memset(out_buffer, 0, bytes);
    memcpy(out, identity, sizeof(*out));
    for (i = 0; i < number; i++) {
        struct ipair_item item;
        size_t span = sizeof(struct vtx_key_ptr_val);
        iitem_at(p, start + i, &item);
        if (span > tail || tail - span < head)
            return -ENOSPC;
        tail -= span;
        memcpy((__u8 *)out_buffer + tail, &item.child, sizeof(item.child));
        out->entries[i] = item.entry;
        out->entries[i].slot.val_off8 = vtx_le16((__u16)(tail / 8U));
        out->entries[i].slot.val_len =
            vtx_le16((__u16)sizeof(struct vtx_key_ptr_val));
    }
    out->nitems = vtx_le16((__u16)number);
    out->free_head8 = vtx_le16((__u16)(head / 8U));
    out->free_tail8 = vtx_le16((__u16)(tail / 8U));
    out->dead_bytes8 = vtx_le16(0);
    memset(out->checksum, 0, sizeof(out->checksum));
    return vtx_node_slots_valid(out, (__u32)bytes) ? 0 : -EUCLEAN;
}

static int ibalance(void *left, void *right, size_t bytes,
                    const struct vtx_key *key,
                    const struct vtx_key_ptr_val *child,
                    void *scratch, size_t scratch_bytes,
                    enum ibal_action action)
{
    const struct vtx_node_hdr *a = (const struct vtx_node_hdr *)left;
    const struct vtx_node_hdr *b = (const struct vtx_node_hdr *)right;
    struct ipair_stream stream;
    size_t need, max_per_node;
    __u32 n, cut;
    int ret;

    if (!bytes || bytes < VTXFS_MIN_NODE_SIZE ||
        bytes > VTXFS_MAX_NODE_SIZE || (bytes & (bytes - 1U)))
        return -EINVAL;
    need = bytes * 2U;
    if (!left || !right || !scratch || scratch_bytes < need ||
        overlap(left, bytes, right, bytes) ||
        overlap(scratch, need, left, bytes) ||
        overlap(scratch, need, right, bytes))
        return -EINVAL;
    if (!validate_internal_sibling(left, bytes) ||
        !validate_internal_sibling(right, bytes))
        return -EUCLEAN;
    if (!icompatible(a, b))
        return -EINVAL;

    memset(&stream, 0, sizeof(stream));
    stream.left = a;
    stream.right = b;
    stream.nleft = vtx_cpu16(a->nitems);

    if (action == IACT_SPLIT || action == IACT_SPLIT_INSERT) {
        if (vtx_cpu16(b->nitems))
            return -EINVAL;
    } else {
        stream.nright = vtx_cpu16(b->nitems);
        if (stream.nleft && stream.nright &&
            vtx_key_cmp(&a->entries[stream.nleft - 1U].key,
                        &b->entries[0].key) >= 0)
            return -EUCLEAN;
    }

    if (action == IACT_SPLIT_INSERT) {
        __u16 flags = vtx_cpu16(a->flags);
        if (!key || !child)
            return -EINVAL;
        stream.key = *key;
        stream.child = *child;
        if (!vtx_oid_valid(vtx_cpu64(stream.child.child_oid)))
            return -EINVAL;
        if ((flags & VTX_NODE_FLAG_PHYSICAL_VBLK) &&
            (!VTXFS_VALID_VBLK(vtx_cpu64(stream.child.hint_vblk)) ||
             !vtx_cpu64(stream.child.hint_xid)))
            return -EINVAL;
        ret = vtx_internal_find(left, bytes, &stream.key, &stream.insert_at);
        if (ret < 0)
            return ret;
        if (!ret)
            return -EEXIST;
        stream.inserting = 1;
    }

    n = icount(&stream);
    max_per_node = (bytes - sizeof(struct vtx_node_hdr)) /
                   (sizeof(struct vtx_node_entry) +
                    sizeof(struct vtx_key_ptr_val));
    if (action == IACT_MERGE) {
        if (n > max_per_node)
            return -ENOSPC;
        cut = n;
    } else {
        if (n < 2U)
            return -ENOSPC;
        cut = n / 2U;
        if (cut > max_per_node || (n - cut) > max_per_node)
            return -ENOSPC;
    }

    ret = ibuild(scratch, bytes, a, &stream, 0, cut);
    if (ret)
        return ret;
    ret = ibuild((__u8 *)scratch + bytes, bytes, b, &stream, cut, n - cut);
    if (ret)
        return ret;
    memcpy(left, scratch, bytes);
    memcpy(right, (__u8 *)scratch + bytes, bytes);
    return 0;
}

int vtx_internal_split(void *left, void *right, size_t node_size,
                       void *scratch, size_t scratch_bytes)
{
    return ibalance(left, right, node_size, NULL, NULL,
                    scratch, scratch_bytes, IACT_SPLIT);
}

int vtx_internal_split_insert(void *left, void *right, size_t node_size,
                              const struct vtx_key *key,
                              const struct vtx_key_ptr_val *child,
                              void *scratch, size_t scratch_bytes)
{
    return ibalance(left, right, node_size, key, child,
                    scratch, scratch_bytes, IACT_SPLIT_INSERT);
}

int vtx_internal_merge(void *left, void *right, size_t node_size,
                       void *scratch, size_t scratch_bytes)
{
    return ibalance(left, right, node_size, NULL, NULL,
                    scratch, scratch_bytes, IACT_MERGE);
}

int vtx_internal_rebalance(void *left, void *right, size_t node_size,
                           void *scratch, size_t scratch_bytes)
{
    return ibalance(left, right, node_size, NULL, NULL,
                    scratch, scratch_bytes, IACT_REBALANCE);
}
