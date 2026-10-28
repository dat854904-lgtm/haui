/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * VTXFS v2.1 draft format validators.
 * These are pure bounds/ABI helpers, not a transactional filesystem parser.
 * Hash verification, ownership traversal, durability and recovery are NOT
 * implemented here. The caller must supply a full readable node buffer.
 */
#ifndef _VTXFS_VALIDATE_H
#define _VTXFS_VALIDATE_H
#include "vtxfs_disk.h"
#ifdef __KERNEL__
#include <linux/stddef.h>
#include <asm/byteorder.h>
#include <linux/byteorder/generic.h>
#define vtx_cpu16(v) ((__u16)le16_to_cpu(v))
#define vtx_cpu32(v) ((__u32)le32_to_cpu(v))
#define vtx_cpu64(v) ((__u64)le64_to_cpu(v))
#define vtx_le16(v) cpu_to_le16((__u16)(v))
#define vtx_le32(v) cpu_to_le32((__u32)(v))
#define vtx_le64(v) cpu_to_le64((__u64)(v))
#else
#include <stddef.h>
#if !defined(__BYTE_ORDER__) || \
    (!(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) && \
     !(__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
#error "VTXFS: unknown host endianness"
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define vtx_cpu16(v) ((__u16)__builtin_bswap16((__u16)(v)))
#define vtx_cpu32(v) ((__u32)__builtin_bswap32((__u32)(v)))
#define vtx_cpu64(v) ((__u64)__builtin_bswap64((__u64)(v)))
#define vtx_le16(v) ((__le16)__builtin_bswap16((__u16)(v)))
#define vtx_le32(v) ((__le32)__builtin_bswap32((__u32)(v)))
#define vtx_le64(v) ((__le64)__builtin_bswap64((__u64)(v)))
#else
#define vtx_cpu16(v) ((__u16)(v))
#define vtx_cpu32(v) ((__u32)(v))
#define vtx_cpu64(v) ((__u64)(v))
#define vtx_le16(v) ((__le16)(v))
#define vtx_le32(v) ((__le32)(v))
#define vtx_le64(v) ((__le64)(v))
#endif
#endif

static inline int vtx_csum_size(__u16 type)
{
    switch (type) {
    case VTX_CSUM_NONE:       return 0;
    case VTX_CSUM_CRC32C:     return 4;
    case VTX_CSUM_BLAKE3_128: return 16;
    case VTX_CSUM_BLAKE3_256: return 32;
    default: return -1;
    }
}

/* All ordering compares decoded numeric values; memcmp is not valid. */
static inline int vtx_key_cmp(const struct vtx_key *a,
                              const struct vtx_key *b)
{
    __u64 aw = vtx_cpu64(a->oid_and_type);
    __u64 bw = vtx_cpu64(b->oid_and_type);
    __u64 ao = vtx_cpu64(a->offset);
    __u64 bo = vtx_cpu64(b->offset);
    if (aw != bw)
        return (aw > bw) ? 1 : -1;
    return (ao > bo) - (ao < bo);
}

static inline int vtx_oid_valid(__u64 oid)
{
    return oid != 0 && oid <= VTX_KEY_OID_MASK;
}

static inline int vtx_vblk_decode(__u64 vblk, __u32 *vag, __u32 *off)
{
    if (!VTXFS_VALID_VBLK(vblk) || !vag || !off)
        return -1;
    *vag = VTXFS_VBLK_TO_VAG_ID(vblk);
    *off = VTXFS_VBLK_TO_AG_OFF(vblk);
    return 0;
}

/* Feature policy helper, not a mount-readiness certification.
 * R/O fallback is permitted only for unknown RO_COMPAT bits.
 */
enum vtx_mount_decision {
    VTX_MOUNT_REFUSE=0, VTX_MOUNT_RO=1, VTX_MOUNT_RW=2
};
static inline int vtx_feature_policy(__u64 compat, __u64 ro,
                                     __u64 incompat)
{
    (void)compat;
    if ((incompat & VTXFS_P0_REQUIRED_INCOMPAT) !=
        VTXFS_P0_REQUIRED_INCOMPAT ||
        (incompat & ~VTXFS_P0_ALLOWED_INCOMPAT))
        return VTX_MOUNT_REFUSE;
    if (ro & ~VTXFS_P0_ALLOWED_RO_COMPAT)
        return VTX_MOUNT_RO;
    return VTX_MOUNT_RW;
}

/* Requires fully readable input. The full-root XID is 64-bit; do not
 * silently accept a reallocated physical block via low-32 generation.
 */
static inline int vtx_phys_ref_valid(const struct vtx_phys_ref *r,
                                     __u64 total_blocks, __u16 dev)
{
    __u64 pblk;
    __u32 n;
    if (!r)
        return 0;
    pblk = vtx_cpu64(r->pblk);
    n = vtx_cpu16(r->blocks);
    if (!n || pblk >= total_blocks ||
        n > total_blocks - pblk ||
        vtx_cpu16(r->dev_id) != dev ||
        vtx_cpu32(r->reserved) != 0)
        return 0;
    return 1;
}

/* Primary and the 64MiB mirror have the same slot-relative layout.
 * Actual checkpoint selection additionally checks FSID, digest,
 * complete root dependencies and transaction ordering.
 */
static inline int vtx_cpd_slot_valid(__u64 pblk)
{
    return (pblk >= VTXFS_CPD_RING_PBLK &&
            pblk < VTXFS_CPD_RING_PBLK + VTXFS_CPD_RING_SLOTS) ||
           (pblk >= VTXFS_BOOT_MIRROR0_PBLK + VTXFS_CPD_RING_PBLK &&
            pblk < VTXFS_BOOT_MIRROR0_PBLK + VTXFS_CPD_RING_PBLK +
                   VTXFS_CPD_RING_SLOTS);
}

/* Strict geometry. Full node buffer must be available to the caller.
 * With 8-byte units, free_tail8 represents a 65536-byte boundary.
 */
static inline int vtx_node_geometry_valid(const struct vtx_node_hdr *h,
                                          __u32 node_size)
{
    __u32 head, tail, dead, nitems, flags;
    if (!h || node_size < VTXFS_MIN_NODE_SIZE ||
        node_size > VTXFS_MAX_NODE_SIZE ||
        (node_size & (node_size - 1U)) != 0)
        return 0;
    flags = vtx_cpu16(h->flags);
    if (vtx_cpu32(h->magic) != VTXFS_NODE_MAGIC ||
        (flags & ~(VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT |
                   VTX_NODE_FLAG_VIRTUAL_OID | VTX_NODE_FLAG_PHYSICAL_VBLK)) ||
        ((flags & VTX_NODE_FLAG_LEAF) != 0) != (h->level == 0) ||
        ((flags & VTX_NODE_FLAG_VIRTUAL_OID) &&
         (flags & VTX_NODE_FLAG_PHYSICAL_VBLK)) ||
        h->level > 7 || h->tree_type == 0 ||
        h->tree_type >= VTX_TREE_MAX)
        return 0;
    nitems = vtx_cpu16(h->nitems);
    head = (__u32)vtx_cpu16(h->free_head8) * VTXFS_NODE_OFFSET_UNIT;
    tail = (__u32)vtx_cpu16(h->free_tail8) * VTXFS_NODE_OFFSET_UNIT;
    dead = (__u32)vtx_cpu16(h->dead_bytes8) * VTXFS_NODE_OFFSET_UNIT;
    if (sizeof(*h) + (__u64)nitems * sizeof(struct vtx_node_entry) > node_size ||
        head != sizeof(*h) + nitems * sizeof(struct vtx_node_entry) ||
        tail > node_size || tail < head || dead > node_size - tail)
        return 0;
    return 1;
}

/* Check key order, each slotted value's boundaries and intersections.
 * A bounded 1024-byte bitset catches overlapping 8-byte value regions
 * in any 4K..64K node, without quadratic pairwise comparison.
 */
static inline int vtx_node_slots_valid(const struct vtx_node_hdr *h,
                                       __u32 node_size)
{
    __u64 occupied[128] = {0}; /* 8192 8-byte slots */
    __u32 i, n, tail, dead, claimed = 0;
    if (!vtx_node_geometry_valid(h, node_size))
        return 0;
    n = vtx_cpu16(h->nitems);
    tail = (__u32)vtx_cpu16(h->free_tail8) * VTXFS_NODE_OFFSET_UNIT;
    dead = (__u32)vtx_cpu16(h->dead_bytes8) * VTXFS_NODE_OFFSET_UNIT;
    for (i = 0; i < n; i++) {
        const struct vtx_slot *slot = &h->entries[i].slot;
        __u32 off = (__u32)vtx_cpu16(slot->val_off8) * 8U;
        __u32 len = vtx_cpu16(slot->val_len);
        __u32 padded = (len + 7U) & ~7U;
        __u32 unit, first = off / 8U, units = padded / 8U;
        if (vtx_cpu16(slot->flags) & ~VTX_SLOT_FLAGS_P0_KNOWN)
            return 0;
        if (off < tail || off > node_size || padded > node_size - off)
            return 0;
        if (i && vtx_key_cmp(&h->entries[i-1].key,
                              &h->entries[i].key) >= 0)
            return 0;
        for (unit = 0; unit < units; unit++) {
            __u32 bit = first + unit;
            __u64 mask = 1ULL << (bit & 63U);
            if (occupied[bit / 64U] & mask)
                return 0;
            occupied[bit / 64U] |= mask;
        }
        claimed += padded;
    }
    return claimed + dead == node_size - tail;
}

/* Safe half-open range intersection for FIEMAP, scrub cursors and iterators.
 * No unchecked end = start + length and no result can precede the request.
 * The caller computes phys += (clipped_start - extent_start) for data.
 * Returns 1 for overlap, 0 for no overlap or invalid arguments.
 */
static inline int vtx_range_intersect(__u64 extent_start, __u64 extent_len,
                                      __u64 request_start, __u64 request_end,
                                      __u64 *clipped_start, __u64 *clipped_len)
{
    __u64 extent_end, lo, hi;
    if (!clipped_start || !clipped_len || !extent_len ||
        request_start >= request_end)
        return 0;
    extent_end = (extent_len > ~0ULL - extent_start) ?
                 ~0ULL : extent_start + extent_len;
    lo = extent_start > request_start ? extent_start : request_start;
    hi = extent_end < request_end ? extent_end : request_end;
    if (hi <= lo)
        return 0;
    *clipped_start = lo;
    *clipped_len = hi - lo;
    return 1;
}

/* Btrfs free-space-tree/APFS spaceman-style bounds validation for VTXFS.
 * Validate 64-bit SFQ ranges BEFORE narrowing block_count to u32 or updating
 * the expected VAG bitmap. SFQ is not allowed to span VAG boundaries.
 */
static inline int vtx_sfq_range_valid(const struct vtx_sfq_entry *sfq,
                                       __u32 total_vags)
{
    __u32 vag, offset;
    __u64 blocks;
    if (!sfq || !total_vags || total_vags > VTXFS_MAX_VAGS)
        return 0;
    blocks = vtx_cpu64(sfq->block_count);
    if (!blocks || blocks > VTXFS_VAG_BLOCKS ||
        vtx_vblk_decode(vtx_cpu64(sfq->start_vblk), &vag, &offset))
        return 0;
    return vag < total_vags && blocks <= VTXFS_VAG_BLOCKS - offset;
}

/* Check that the extent fits inside the owning (authoritative) allocation.
 * No physical refcount may be inferred just from birth_xid/snapshot age.
 */
static inline int vtx_extent_within_ref(const struct vtx_extent_item *e,
                                        const struct vtx_ref_item *r)
{
    __u64 start, block, id;
    __u32 blocks, count;
    if (!e || !r)
        return 0;
    start = vtx_cpu64(r->start_vblk);
    count = vtx_cpu32(r->block_count);
    block = vtx_cpu64(e->vblk);
    blocks = vtx_cpu32(e->physical_blocks);
    id = vtx_cpu64(r->allocation_id);
    return id != 0 && id == vtx_cpu64(e->allocation_id) &&
           VTXFS_VALID_VBLK(start) && count != 0 &&
           count <= VTXFS_MAX_VBLKS - start &&
           block >= start && block < start + count &&
           blocks != 0 && blocks <= start + count - block &&
           vtx_cpu64(r->refcount) != 0;
}

/* P0 extent payload: CRC32C per 4K data block, no compression/crypto.
 * A leaf parser must also check item size against actual node free space.
 */
static inline int vtx_extent_p0_value_valid(const struct vtx_extent_item *e,
                                            size_t item_bytes)
{
    __u32 count, flags, phys_blocks, logical_bytes, payload_bytes;
    size_t required;
    if (!e || item_bytes < sizeof(*e))
        return 0;
    count = vtx_cpu16(e->csum_count);
    flags = vtx_cpu32(e->flags);
    phys_blocks = vtx_cpu32(e->physical_blocks);
    logical_bytes = vtx_cpu32(e->logical_bytes);
    payload_bytes = vtx_cpu32(e->payload_bytes);
    if (vtx_cpu16(e->header_bytes) != sizeof(*e) ||
        (e->compression != VTX_COMPR_NONE &&
         e->compression != VTX_COMPR_ZSTD &&
         e->compression != VTX_COMPR_LZ4 &&
         e->compression != VTX_COMPR_ZLIB) ||
        e->encryption != VTX_ENC_NONE ||
        (flags & ~(VTX_EXTENT_INLINE | VTX_EXTENT_SHARED | VTX_EXTENT_COMPRESSED)) != 0 ||
        (flags & (VTX_EXTENT_INLINE | VTX_EXTENT_SHARED)) ==
                 (VTX_EXTENT_INLINE | VTX_EXTENT_SHARED) ||
        vtx_cpu64(e->crypto_identity) != 0)
        return 0;
    required = sizeof(*e) + (size_t)count * sizeof(__le32) +
               ((flags & VTX_EXTENT_INLINE) ? payload_bytes : 0);
    if (required != item_bytes)
        return 0;
    if (flags & VTX_EXTENT_INLINE) {
        return payload_bytes != 0 &&
               payload_bytes <= VTXFS_MAX_INLINE_DATA &&
               logical_bytes == payload_bytes &&
               phys_blocks == 0 && count == 0 &&
               vtx_cpu64(e->vblk) == 0 &&
               vtx_cpu64(e->allocation_id) == 0 &&
               e->compression == VTX_COMPR_NONE &&
               vtx_cpu16(e->csum_type) == VTX_CSUM_NONE;
    }
    if (flags & VTX_EXTENT_COMPRESSED) {
        return payload_bytes == 0 &&
               vtx_cpu64(e->allocation_id) != 0 &&
               VTXFS_VALID_VBLK(vtx_cpu64(e->vblk)) &&
               phys_blocks != 0 && phys_blocks <= 256 &&
               phys_blocks <= VTXFS_MAX_VBLKS - vtx_cpu64(e->vblk) &&
               count == phys_blocks && logical_bytes != 0 &&
               e->compression != VTX_COMPR_NONE &&
               vtx_cpu16(e->csum_type) == VTX_CSUM_CRC32C;
    }
    return payload_bytes == 0 &&
           vtx_cpu64(e->allocation_id) != 0 &&
           VTXFS_VALID_VBLK(vtx_cpu64(e->vblk)) &&
           phys_blocks != 0 && phys_blocks <= 256 &&
           phys_blocks <= VTXFS_MAX_VBLKS - vtx_cpu64(e->vblk) &&
           count == phys_blocks && logical_bytes != 0 &&
           logical_bytes <= phys_blocks * VTXFS_BLOCK_SIZE &&
           e->compression == VTX_COMPR_NONE &&
           vtx_cpu16(e->csum_type) == VTX_CSUM_CRC32C;
}

/* Validate a complete directory item, including raw name restrictions.
 * UTF-8/casefold policies and keyed SipHash must be checked separately.
 */
static inline int vtx_dir_item_valid(const struct vtx_dir_item *d, size_t bytes)
{
    __u32 n, i;
    if (!d || bytes < sizeof(*d))
        return 0;
    n = vtx_cpu16(d->name_len);
    if (!n || n > VTXFS_NAME_MAX || sizeof(*d) + n != bytes ||
        d->file_type > VTX_FT_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        if (d->name[i] == 0 || d->name[i] == '/')
            return 0;
    }
    if ((n == 1 && d->name[0] == '.') ||
        (n == 2 && d->name[0] == '.' && d->name[1] == '.'))
        return 0;
    return 1;
}
#endif /* _VTXFS_VALIDATE_H */
