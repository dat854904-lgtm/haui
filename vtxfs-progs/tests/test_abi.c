/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "../include/vtxfs_disk.h"

/* This does not test disk I/O or prove crash safety. */
_Static_assert(sizeof(struct vtx_ioctl_abi) == 32, "GET_ABI layout");
_Static_assert(sizeof(struct vtx_ioc_capabilities) == 56, "CAPS layout");
_Static_assert(sizeof(struct vtx_ioc_subvol_args) == 320, "SUBVOL layout");
_Static_assert(sizeof(struct vtx_ioc_dev_args) == 600, "DEV layout");
_Static_assert(sizeof(struct vtx_ioc_omap_lookup) == 88, "OMAP lookup layout");
_Static_assert(sizeof(struct vtx_ioc_scrub_args) == 80, "scrub layout");
_Static_assert(sizeof(struct vtx_ioc_encoded_io) == 64, "encoded layout");
_Static_assert(sizeof(struct vtx_ioc_send_args) == 64, "send layout");
_Static_assert(sizeof(struct vtx_ioc_sync) == 32, "sync ioctl layout");
_Static_assert(_IOC_DIR(VTXFS_IOC_GET_ABI) == (_IOC_READ | _IOC_WRITE), "GET_ABI needs read/write");
_Static_assert(_IOC_DIR(VTXFS_IOC_GET_CAPABILITIES) == (_IOC_READ | _IOC_WRITE), "GET_CAPABILITIES needs read/write");
_Static_assert(sizeof(struct vtx_free_space_item) == 32, "free cache");
_Static_assert(sizeof(struct vtx_tree_block_item) == 40, "tree block");
_Static_assert(sizeof(struct vtx_extent_data_item) == 48, "extent data");
_Static_assert(sizeof(struct vtx_dev_extent_item) == 32, "dev extent");
_Static_assert(sizeof(struct vtx_root_item) == 56, "root item");
_Static_assert(sizeof(struct vtx_inode_item_v2) == 128, "inode v2");

static int cmp_u64(uint64_t a, uint64_t b)
{
    return (a > b) - (a < b);
}

static int key_cmp(uint64_t aw, uint64_t ao, uint64_t bw, uint64_t bo)
{
    int c = cmp_u64(aw, bw);
    return c ? c : cmp_u64(ao, bo);
}

static int tuple_cmp(uint64_t oid1, uint8_t type1, uint64_t off1,
                     uint64_t oid2, uint8_t type2, uint64_t off2)
{
    int c = cmp_u64(oid1, oid2);
    if (c) return c;
    c = cmp_u64(type1, type2);
    return c ? c : cmp_u64(off1, off2);
}

static uint64_t rng_state = 0xc1b7e2a9f043bd27ULL;
static uint64_t next_rand(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ULL;
}

int main(void)
{
    /* Physical geometry and range checks. */
    assert(VTXFS_VAG_BYTES == 128ULL * 1024 * 1024);
    assert(VTXFS_MAX_VAGS == 516096U);
    assert(VTXFS_MAX_VBLKS * VTXFS_BLOCK_SIZE == 63ULL * 1024 * 1024 * 1024 * 1024);
    assert(!VTXFS_VALID_VBLK(0));
    assert(VTXFS_VALID_VBLK(1));
    assert(VTXFS_VALID_VBLK(VTXFS_MAX_VBLKS - 1));
    assert(!VTXFS_VALID_VBLK(VTXFS_MAX_VBLKS));
    assert(!VTXFS_VALID_VBLK(UINT64_MAX));

    /* OID ordering must take precedence over type, unlike the v2.0 key. */
    {
        uint64_t a = VTX_MAKE_OID_TYPE(5, VTX_TYPE_EXTENT);
        uint64_t b = VTX_MAKE_OID_TYPE(900, VTX_TYPE_INODE);
        uint64_t c = VTX_MAKE_OID_TYPE(5, VTX_TYPE_INODE);
        assert(c < a);
        assert(a < b);
        assert(VTX_KEY_GET_OID(a) == 5);
        assert(VTX_KEY_GET_TYPE(a) == VTX_TYPE_EXTENT);
    }

    /* Deterministic randomized property test over 100000 pairs. */
    for (unsigned i = 0; i < 100000; i++) {
        uint64_t oid1 = next_rand() & VTX_KEY_OID_MASK;
        uint64_t oid2 = next_rand() & VTX_KEY_OID_MASK;
        uint8_t type1 = (uint8_t) next_rand();
        uint8_t type2 = (uint8_t) next_rand();
        uint64_t off1 = next_rand();
        uint64_t off2 = next_rand();
        uint64_t a = VTX_MAKE_OID_TYPE(oid1, type1);
        uint64_t b = VTX_MAKE_OID_TYPE(oid2, type2);
        assert(key_cmp(a, off1, b, off2) ==
               tuple_cmp(oid1, type1, off1, oid2, type2, off2));
        assert(VTX_KEY_GET_OID(a) == oid1);
        assert(VTX_KEY_GET_TYPE(a) == type1);
        assert(key_cmp(b, off2, a, off1) == -key_cmp(a, off1, b, off2));
    }

    /* Full-file-name equality is still mandatory for hash collisions. */
    assert(VTX_DIR_MAKE_OFFSET(0x123456789abcULL, 1) !=
           VTX_DIR_MAKE_OFFSET(0x123456789abcULL, 2));
    assert((VTX_DIR_MAKE_OFFSET(0x123456789abcULL, 65535) >> 16) ==
           0x123456789abcULL);

    /* User-space pointer size never changes the encoded IO struct width. */
    assert(sizeof(struct vtx_ioc_encoded_io) == 64);
    assert(VTXFS_P0_ALLOWED_INCOMPAT ==
           (VTXFS_FEAT_INCOMPAT_V21_LAYOUT |
            VTXFS_FEAT_INCOMPAT_OMAP_VIEWS |
            VTXFS_FEAT_INCOMPAT_EXTENT_CSUM |
            VTXFS_FEAT_INCOMPAT_COMPRESS |
            VTXFS_FEAT_INCOMPAT_MULTIDEV |
            VTXFS_FEAT_INCOMPAT_SEND_RECEIVE));
    assert((VTXFS_P0_ALLOWED_INCOMPAT & VTXFS_FEAT_INCOMPAT_AEAD) == 0);
    assert((VTXFS_P0_ALLOWED_INCOMPAT & VTXFS_FEAT_INCOMPAT_FAST_WAL) == 0);
    assert(sizeof(struct vtx_vag_desc) * VTXFS_VAG_LEAF_ENTRIES == 65536);

    printf("PASS: static ABI layout + 100000 key comparator cases + geometry + feature gates\n");
    return 0;
}
