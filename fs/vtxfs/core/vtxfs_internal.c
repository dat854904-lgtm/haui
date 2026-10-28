/* SPDX-License-Identifier: GPL-2.0-only */
#include "vtxfs_internal.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

enum internal_op { INTERNAL_INSERT, INTERNAL_REPLACE, INTERNAL_DELETE, INTERNAL_COMPACT };

static size_t padded(size_t n)
{
    return (n + 7U) & ~(size_t)7U;
}

static int buffer_aliases(const void *a, const void *b, size_t n)
{
    unsigned long x = (unsigned long)a;
    unsigned long y = (unsigned long)b;
    return x <= y ? y - x < n : x - y < n;
}

static int internal_valid(const void *buffer, size_t size)
{
    if (!buffer || size < VTXFS_MIN_NODE_SIZE ||
        size > VTXFS_MAX_NODE_SIZE ||
        !vtx_node_slots_valid((const struct vtx_node_hdr *)buffer,
                              (__u32)size))
        return 0;
    const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)buffer;
    __u16 flags = vtx_cpu16(h->flags);
    __u32 i;
    if ((flags & VTX_NODE_FLAG_LEAF) || !h->level ||
        (!!(flags & VTX_NODE_FLAG_VIRTUAL_OID) ==
         !!(flags & VTX_NODE_FLAG_PHYSICAL_VBLK)))
        return 0;
    for (i = 0; i < vtx_cpu16(h->nitems); ++i) {
        const struct vtx_slot *sl = &h->entries[i].slot;
        struct vtx_key_ptr_val child;
        if (vtx_cpu16(sl->val_len) != sizeof(child))
            return 0;
        memcpy(&child, (const __u8 *)buffer +
               (__u32)vtx_cpu16(sl->val_off8) * 8U, sizeof(child));
        if (!vtx_oid_valid(vtx_cpu64(child.child_oid)))
            return 0;
        if (flags & VTX_NODE_FLAG_PHYSICAL_VBLK) {
            if (!VTXFS_VALID_VBLK(vtx_cpu64(child.hint_vblk)) ||
                !vtx_cpu64(child.hint_xid))
                return 0;
        }
    }
    return 1;
}

int vtx_internal_init(void *buffer, size_t size, __u64 oid, __u64 xid,
                  __u64 vblk, __u8 tree_type, __u16 flags, __u8 level)
{
    struct vtx_node_hdr *h;
    if (!buffer || size < VTXFS_MIN_NODE_SIZE ||
        size > VTXFS_MAX_NODE_SIZE ||
        (size & (size - 1U)) != 0 || !vtx_oid_valid(oid) ||
        !xid || !VTXFS_VALID_VBLK(vblk) ||
        tree_type == VTX_TREE_NONE || tree_type >= VTX_TREE_MAX ||
        !level || level > 7 ||
        (flags & VTX_NODE_FLAG_LEAF) ||
        (flags & ~(VTX_NODE_FLAG_ROOT | VTX_NODE_FLAG_VIRTUAL_OID |
                   VTX_NODE_FLAG_PHYSICAL_VBLK)) ||
        (!!(flags & VTX_NODE_FLAG_VIRTUAL_OID) ==
         !!(flags & VTX_NODE_FLAG_PHYSICAL_VBLK))) 
        return -EINVAL;

    memset(buffer, 0, size);
    h = (struct vtx_node_hdr *)buffer;
    h->magic = vtx_le32(VTXFS_NODE_MAGIC);
    h->oid = vtx_le64(oid);
    h->xid = vtx_le64(xid);
    h->self_vblk = vtx_le64(vblk);
    h->level = level;
    h->tree_type = tree_type;
    h->flags = vtx_le16(flags);
    h->free_head8 = vtx_le16(sizeof(*h) / 8U);
    h->free_tail8 = vtx_le16((__u16)(size / 8U));
    return vtx_node_slots_valid(h, (__u32)size) ? 0 : -EUCLEAN;
}

int vtx_internal_find(const void *buffer, size_t size,
                  const struct vtx_key *key, __u32 *slot)
{
    const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)buffer;
    __u32 lo, hi;
    if (!key || !slot || !internal_valid(buffer, size))
        return -EUCLEAN;
    lo = 0;
    hi = vtx_cpu16(h->nitems);
    while (lo < hi) {
        __u32 mid = lo + (hi - lo) / 2U;
        int cmp = vtx_key_cmp(&h->entries[mid].key, key);
        if (cmp < 0)
            lo = mid + 1U;
        else
            hi = mid;
    }
    *slot = lo;
    if (lo < vtx_cpu16(h->nitems) &&
        vtx_key_cmp(&h->entries[lo].key, key) == 0)
        return 0;
    return 1;
}

/* Pure rebuild in caller scratch; original buffer untouched on all errors.
 * Repacking (including deletion) always produces dead_bytes=0, rather than
 * trying to reclaim overlapping or out-of-order dead fragments in place.
 */
static int rebuild(void *buffer, size_t size, const struct vtx_key *key,
                   const void *value, __u16 value_bytes,
                   void *scratch, size_t scratch_bytes, enum internal_op op)
{
    const struct vtx_node_hdr *old = (const struct vtx_node_hdr *)buffer;
    struct vtx_node_hdr *out = (struct vtx_node_hdr *)scratch;
    __u32 slot = 0, n, new_n, i;
    size_t pos, header_need;
    int found = 0;
    struct vtx_key saved_key;

    if (!scratch || scratch_bytes < size || !internal_valid(buffer, size) ||
        buffer_aliases(buffer, scratch, size))
        return -EINVAL;

    if (op != INTERNAL_COMPACT) {
        if (!key)
            return -EINVAL;
        saved_key = *key; /* key may be inside scratch */
        key = &saved_key;
        found = vtx_internal_find(buffer, size, key, &slot);
        if (found < 0)
            return found;
        if (op == INTERNAL_INSERT && found == 0)
            return -EEXIST;
        if (op != INTERNAL_INSERT && found != 0)
            return -ENOENT;
    }
    if ((op == INTERNAL_INSERT || op == INTERNAL_REPLACE) &&
        (!value || value_bytes != sizeof(struct vtx_key_ptr_val)))
        return -EINVAL;
    if (op == INTERNAL_INSERT || op == INTERNAL_REPLACE) {
        struct vtx_key_ptr_val child;
        __u16 flags = vtx_cpu16(old->flags);
        memcpy(&child, value, sizeof(child));
        if (!vtx_oid_valid(vtx_cpu64(child.child_oid)))
            return -EINVAL;
        if ((flags & VTX_NODE_FLAG_PHYSICAL_VBLK) &&
            (!VTXFS_VALID_VBLK(vtx_cpu64(child.hint_vblk)) ||
             !vtx_cpu64(child.hint_xid)))
            return -EINVAL;
    }
    if ((op == INTERNAL_INSERT || op == INTERNAL_REPLACE) && value_bytes) {
        unsigned long x = (unsigned long)value;
        unsigned long y = (unsigned long)scratch;
        if (x <= y ? y-x < value_bytes : x-y < size)
            return -EINVAL; /* value overlaps scratch */
    }

    n = vtx_cpu16(old->nitems);
    new_n = n + (op == INTERNAL_INSERT) - (op == INTERNAL_DELETE);
    if (new_n > 65535U)
        return -ENOSPC;
    header_need = sizeof(*old) + (size_t)new_n *
                                  sizeof(struct vtx_node_entry);
    if (header_need > size)
        return -ENOSPC;

    memset(scratch, 0, size);
    memcpy(out, old, sizeof(*out));
    pos = size;

    for (i = 0; i < new_n; i++) {
        __u32 old_i = i;
        const struct vtx_key *use_key;
        const void *use_data;
        __u16 use_len;
        struct vtx_slot source_slot = {0};
        size_t span;

        if (op == INTERNAL_INSERT && i > slot)
            old_i--;
        if (op == INTERNAL_DELETE && i >= slot)
            old_i++;

        if ((op == INTERNAL_INSERT || op == INTERNAL_REPLACE) && i == slot) {
            use_key = key;
            use_data = value;
            use_len = value_bytes;
        } else {
            const struct vtx_node_entry *src = &old->entries[old_i];
            source_slot = src->slot;
            use_key = &src->key;
            use_len = vtx_cpu16(source_slot.val_len);
            use_data = (const __u8 *)buffer +
                       (__u32)vtx_cpu16(source_slot.val_off8) * 8U;
        }

        span = padded(use_len);
        if (span > pos || pos - span < header_need)
            return -ENOSPC;
        pos -= span;
        if (use_len)
            memcpy((__u8 *)scratch + pos, use_data, use_len);

        out->entries[i].key = *use_key;
        out->entries[i].slot.val_off8 = vtx_le16((__u16)(pos / 8U));
        out->entries[i].slot.val_len = vtx_le16(use_len);
        if (!((op == INTERNAL_INSERT || op == INTERNAL_REPLACE) && i == slot)) {
            out->entries[i].slot.flags = source_slot.flags;
            out->entries[i].slot.key_prefix = source_slot.key_prefix;
        }
    }

    out->nitems = vtx_le16((__u16)new_n);
    out->free_head8 = vtx_le16((__u16)(header_need / 8U));
    out->free_tail8 = vtx_le16((__u16)(pos / 8U));
    out->dead_bytes8 = 0;
    /* This helper is NOT a checksum implementation. */
    memset(out->checksum, 0, sizeof(out->checksum));
    if (!vtx_node_slots_valid(out, (__u32)size))
        return -EUCLEAN;
    memcpy(buffer, scratch, size);
    return 0;
}

int vtx_internal_insert(void *buffer, size_t size, const struct vtx_key *key,
                    const struct vtx_key_ptr_val *value,
                    void *scratch, size_t scratch_len)
{
    return rebuild(buffer, size, key, value,
                   (__u16)sizeof(struct vtx_key_ptr_val),
                   scratch, scratch_len, INTERNAL_INSERT);
}

int vtx_internal_replace(void *buffer, size_t size, const struct vtx_key *key,
                     const struct vtx_key_ptr_val *value,
                     void *scratch, size_t scratch_len)
{
    return rebuild(buffer, size, key, value,
                   (__u16)sizeof(struct vtx_key_ptr_val),
                   scratch, scratch_len, INTERNAL_REPLACE);
}

int vtx_internal_delete(void *buffer, size_t size, const struct vtx_key *key,
                    void *scratch, size_t scratch_len)
{
    return rebuild(buffer, size, key, NULL, 0,
                   scratch, scratch_len, INTERNAL_DELETE);
}

int vtx_internal_compact(void *buffer, size_t size,
                     void *scratch, size_t scratch_len)
{
    return rebuild(buffer, size, NULL, NULL, 0,
                   scratch, scratch_len, INTERNAL_COMPACT);
}
