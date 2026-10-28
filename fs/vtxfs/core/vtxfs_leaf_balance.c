/* SPDX-License-Identifier: GPL-2.0-only */
/* In-memory, all-or-nothing, two-sibling slotted-leaf operations.
 * NO parent separator, root promotion, OMAP, checksum or disk publication.
 */
#include "vtxfs_leaf.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

enum balance_action { ACT_SPLIT, ACT_SPLIT_INSERT, ACT_MERGE, ACT_REBALANCE };
struct pair_stream {
    const struct vtx_node_hdr *left, *right;
    __u32 nleft, nright, insert_at;
    int inserting;
    struct vtx_key key;
    const void *value;
    __u16 value_len;
};
struct pair_item {
    struct vtx_node_entry entry;
    const void *value;
    __u16 len;
};

static size_t align8(size_t n) { return (n + 7U) & ~(size_t)7U; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    unsigned long x = (unsigned long)a, y = (unsigned long)b;
    if (!an || !bn)
        return 0;
    return x <= y ? y-x < an : x-y < bn;
}
static __u32 count(const struct pair_stream *p)
{
    return p->nleft + p->nright + (__u32)p->inserting;
}
static void item_at(const struct pair_stream *p, __u32 idx,
                    struct pair_item *out)
{
    const struct vtx_node_hdr *node;
    __u32 src = idx;
    memset(out, 0, sizeof(*out));
    if (p->inserting && idx == p->insert_at) {
        out->entry.key = p->key;
        out->value = p->value;
        out->len = p->value_len;
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
    out->len = vtx_cpu16(out->entry.slot.val_len);
    out->value = (const __u8 *)node +
                 (__u32)vtx_cpu16(out->entry.slot.val_off8) * 8U;
}
static size_t item_size(const struct pair_item *i)
{
    return sizeof(struct vtx_node_entry) + align8(i->len);
}
static int validate_leaf(const void *buffer, size_t bytes)
{
    const struct vtx_node_hdr *h = (const void *)buffer;
    __u16 flags;
    if (!buffer || bytes < VTXFS_MIN_NODE_SIZE ||
        bytes > VTXFS_MAX_NODE_SIZE ||
        !vtx_node_slots_valid(h, (__u32)bytes))
        return 0;
    flags = vtx_cpu16(h->flags);
    return (flags & (VTX_NODE_FLAG_LEAF |
                     VTX_NODE_FLAG_VIRTUAL_OID |
                     VTX_NODE_FLAG_PHYSICAL_VBLK)) == VTX_NODE_FLAG_LEAF &&
           vtx_oid_valid(vtx_cpu64(h->oid)) &&
           vtx_cpu64(h->xid) &&
           VTXFS_VALID_VBLK(vtx_cpu64(h->self_vblk));
}
static int compatible(const struct vtx_node_hdr *a,
                      const struct vtx_node_hdr *b)
{
    return a->tree_type == b->tree_type &&
           vtx_cpu64(a->owner_id) == vtx_cpu64(b->owner_id) &&
           vtx_cpu64(a->xid) == vtx_cpu64(b->xid) &&
           vtx_cpu64(a->oid) != vtx_cpu64(b->oid) &&
           vtx_cpu64(a->self_vblk) != vtx_cpu64(b->self_vblk);
}

/* Byte-weighted feasible partition; no 50%-full requirement for varlen. */
static int partition(const struct pair_stream *p, size_t bytes, __u32 *cut)
{
    size_t capacity = bytes - sizeof(struct vtx_node_hdr);
    size_t total = 0, prefix = 0, best_distance = (size_t)-1;
    __u32 n = count(p), i, best = 0;
    struct pair_item item;
    if (n < 2)
        return -ENOSPC;
    for (i = 0; i < n; i++) {
        item_at(p, i, &item);
        if (item_size(&item) > capacity)
            return -ENOSPC;
        total += item_size(&item);
    }
    for (i = 1; i < n; i++) {
        size_t suffix, distance;
        item_at(p, i-1, &item);
        prefix += item_size(&item);
        suffix = total - prefix;
        if (prefix > capacity || suffix > capacity)
            continue;
        distance = prefix >= suffix ? prefix-suffix : suffix-prefix;
        if (!best || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    if (!best)
        return -ENOSPC;
    *cut = best;
    return 0;
}

/* Publish only after both images are rebuilt and checked. */
static int build(void *out_buffer, size_t bytes,
                 const struct vtx_node_hdr *identity,
                 const struct pair_stream *p, __u32 start, __u32 number)
{
    struct vtx_node_hdr *out = out_buffer;
    size_t head = sizeof(*out) +
                  (size_t)number * sizeof(struct vtx_node_entry);
    size_t tail = bytes;
    __u32 i;
    if (number > 65535U || head > bytes)
        return -ENOSPC;
    memset(out_buffer, 0, bytes);
    memcpy(out, identity, sizeof(*out));
    for (i = 0; i < number; i++) {
        struct pair_item item;
        size_t span;
        item_at(p, start+i, &item);
        span = align8(item.len);
        if (span > tail || tail-span < head)
            return -ENOSPC;
        tail -= span;
        if (item.len)
            memcpy((__u8 *)out_buffer+tail, item.value, item.len);
        out->entries[i] = item.entry;
        out->entries[i].slot.val_off8 = vtx_le16((__u16)(tail/8U));
        out->entries[i].slot.val_len = vtx_le16(item.len);
    }
    out->nitems = vtx_le16((__u16)number);
    out->free_head8 = vtx_le16((__u16)(head/8U));
    out->free_tail8 = vtx_le16((__u16)(tail/8U));
    out->dead_bytes8 = vtx_le16(0);
    memset(out->checksum, 0, sizeof(out->checksum));
    return vtx_node_slots_valid(out, (__u32)bytes) ? 0 : -EUCLEAN;
}

static int balance(void *left, void *right, size_t bytes,
                   const struct vtx_key *key, const void *value,
                   __u16 value_len, void *scratch, size_t scratch_bytes,
                   enum balance_action action)
{
    const struct vtx_node_hdr *a = left, *b = right;
    struct pair_stream stream;
    __u32 n, cut;
    size_t need;
    int ret;

    if (!bytes || bytes < VTXFS_MIN_NODE_SIZE ||
        bytes > VTXFS_MAX_NODE_SIZE || (bytes & (bytes-1U)))
        return -EINVAL;
    need = bytes*2U;
    if (!left || !right || !scratch || scratch_bytes < need ||
        overlap(left, bytes, right, bytes) ||
        overlap(scratch, need, left, bytes) ||
        overlap(scratch, need, right, bytes))
        return -EINVAL;
    if (!validate_leaf(left, bytes) || !validate_leaf(right, bytes))
        return -EUCLEAN;
    if (!compatible(a,b))
        return -EINVAL;

    memset(&stream, 0, sizeof(stream));
    stream.left = a;
    stream.right = b;
    stream.nleft = vtx_cpu16(a->nitems);

    if (action == ACT_SPLIT || action == ACT_SPLIT_INSERT) {
        if (vtx_cpu16(b->nitems))
            return -EINVAL;
    } else {
        stream.nright = vtx_cpu16(b->nitems);
        if (stream.nleft && stream.nright &&
            vtx_key_cmp(&a->entries[stream.nleft-1U].key,
                        &b->entries[0].key) >= 0)
            return -EUCLEAN;
    }

    if (action == ACT_SPLIT_INSERT) {
        if (!key || (value_len && !value))
            return -EINVAL;
        stream.key = *key; /* key can point to scratch */
        if (value_len && overlap(value, value_len, scratch, need))
            return -EINVAL;
        ret = vtx_leaf_find(left, bytes, &stream.key, &stream.insert_at);
        if (ret < 0)
            return ret;
        if (!ret)
            return -EEXIST;
        stream.inserting = 1;
        stream.value = value;
        stream.value_len = value_len;
    }

    n = count(&stream);
    if (action == ACT_MERGE)
        cut = n;
    else {
        ret = partition(&stream, bytes, &cut);
        if (ret)
            return ret;
    }

    ret = build(scratch, bytes, a, &stream, 0, cut);
    if (ret)
        return ret;
    ret = build((__u8 *)scratch+bytes, bytes, b, &stream, cut, n-cut);
    if (ret)
        return ret;
    memcpy(left, scratch, bytes);
    memcpy(right, (__u8 *)scratch+bytes, bytes);
    return 0;
}

int vtx_leaf_split(void *a, void *b, size_t size,
                   void *scratch, size_t scratch_bytes)
{
    return balance(a,b,size,NULL,NULL,0,scratch,scratch_bytes,ACT_SPLIT);
}
int vtx_leaf_split_insert(void *a, void *b, size_t size,
                          const struct vtx_key *key, const void *value,
                          __u16 len, void *scratch, size_t scratch_bytes)
{
    return balance(a,b,size,key,value,len,scratch,scratch_bytes,ACT_SPLIT_INSERT);
}
int vtx_leaf_merge(void *a, void *b, size_t size,
                   void *scratch, size_t scratch_bytes)
{
    return balance(a,b,size,NULL,NULL,0,scratch,scratch_bytes,ACT_MERGE);
}
int vtx_leaf_rebalance(void *a, void *b, size_t size,
                       void *scratch, size_t scratch_bytes)
{
    return balance(a,b,size,NULL,NULL,0,scratch,scratch_bytes,ACT_REBALANCE);
}
