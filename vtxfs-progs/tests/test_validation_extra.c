/* SPDX-License-Identifier: GPL-2.0-only */
/* Focused negative tests for v2.1 draft validators. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../include/vtxfs_validate.h"

int main(void)
{
    struct vtx_phys_ref pr = {0};
    struct vtx_node_hdr h = {0};
    struct vtx_extent_item e = {0};
    struct vtx_ref_item r = {0};
    unsigned char node[4096] = {0};
    struct vtx_node_hdr *n = (struct vtx_node_hdr *)(void *)node;
    unsigned char dirbuf[sizeof(struct vtx_dir_item) + 3] = {0};
    struct vtx_dir_item *d = (struct vtx_dir_item *)(void *)dirbuf;

    assert(vtx_csum_size(VTX_CSUM_NONE) == 0);
    assert(vtx_csum_size(VTX_CSUM_CRC32C) == 4);
    assert(vtx_csum_size(VTX_CSUM_BLAKE3_128) == 16);
    assert(vtx_csum_size(VTX_CSUM_BLAKE3_256) == 32);
    assert(vtx_csum_size(255) == -1);

    assert(vtx_feature_policy(0, 0, VTXFS_P0_REQUIRED_INCOMPAT) == VTX_MOUNT_RW);
    assert(vtx_feature_policy(0, 2, VTXFS_P0_REQUIRED_INCOMPAT) == VTX_MOUNT_RO);
    assert(vtx_feature_policy(0, 0, VTXFS_P0_REQUIRED_INCOMPAT | VTXFS_FEAT_INCOMPAT_FAST_WAL)
           == VTX_MOUNT_REFUSE);
    assert(vtx_feature_policy(0, 0, 0) == VTX_MOUNT_REFUSE);

    pr.pblk = 100;
    pr.blocks = 1;
    pr.dev_id = 0;
    pr.xid = 123;
    assert(vtx_phys_ref_valid(&pr, 101, 0));
    assert(!vtx_phys_ref_valid(NULL, 101, 0));
    assert(!vtx_phys_ref_valid(&pr, 100, 0));
    assert(!vtx_phys_ref_valid(&pr, 101, 1));
    pr.blocks = 2;
    assert(!vtx_phys_ref_valid(&pr, 101, 0));
    pr.blocks = 1;
    assert(vtx_cpd_slot_valid(VTXFS_CPD_RING_PBLK + 7));
    assert(vtx_cpd_slot_valid(VTXFS_BOOT_MIRROR0_PBLK + VTXFS_CPD_RING_PBLK));
    assert(!vtx_cpd_slot_valid(5000));

    h.magic = VTXFS_NODE_MAGIC;
    h.flags = VTX_NODE_FLAG_LEAF;
    h.tree_type = VTX_TREE_FS;
    h.free_head8 = sizeof(h) / 8;
    h.free_tail8 = 4096 / 8;
    assert(vtx_node_geometry_valid(&h, 4096));
    assert(!vtx_node_geometry_valid(&h, 12288));
    h.magic = 0;
    assert(!vtx_node_geometry_valid(&h, 4096));
    h.magic = VTXFS_NODE_MAGIC;
    h.level = 1;
    assert(!vtx_node_geometry_valid(&h, 4096));
    h.level = 0;
    h.flags |= VTX_NODE_FLAG_PHYSICAL_VBLK | VTX_NODE_FLAG_VIRTUAL_OID;
    assert(!vtx_node_geometry_valid(&h, 4096));

    n->magic = VTXFS_NODE_MAGIC;
    n->flags = VTX_NODE_FLAG_LEAF;
    n->tree_type = VTX_TREE_FS;
    n->nitems = 2;
    n->free_head8 = (sizeof(*n) + 2 * sizeof(struct vtx_node_entry)) / 8;
    n->free_tail8 = (4096 - 32) / 8;
    n->entries[0].key.oid_and_type = VTX_MAKE_OID_TYPE(10, VTX_TYPE_EXTENT);
    n->entries[1].key.oid_and_type = VTX_MAKE_OID_TYPE(11, VTX_TYPE_EXTENT);
    n->entries[0].slot.val_off8 = (4096 - 32) / 8;
    n->entries[1].slot.val_off8 = (4096 - 16) / 8;
    n->entries[0].slot.val_len = 16;
    n->entries[1].slot.val_len = 16;
    assert(vtx_node_slots_valid(n, 4096));
    n->entries[1].slot.val_off8 = (4096 - 32) / 8;
    assert(!vtx_node_slots_valid(n, 4096)); /* overlapping values */
    n->entries[1].slot.val_off8 = (4096 - 16) / 8;
    n->entries[1].key.oid_and_type = VTX_MAKE_OID_TYPE(9, VTX_TYPE_EXTENT);
    assert(!vtx_node_slots_valid(n, 4096)); /* unsorted keys */
    n->entries[1].key.oid_and_type = VTX_MAKE_OID_TYPE(11, VTX_TYPE_EXTENT);
    n->entries[1].slot.flags = 0x8000;
    assert(!vtx_node_slots_valid(n, 4096));

    d->name_len = 3;
    d->file_type = VTX_FT_REG;
    memcpy(d->name, "abc", 3);
    assert(vtx_dir_item_valid(d, sizeof(dirbuf)));
    d->name[1] = '/';
    assert(!vtx_dir_item_valid(d, sizeof(dirbuf)));
    d->name[1] = 'b';
    assert(!vtx_dir_item_valid(d, sizeof(dirbuf) - 1));

    e.header_bytes = sizeof(e);
    e.vblk = 20;
    e.allocation_id = 900;
    e.physical_blocks = 2;
    e.logical_bytes = 8192;
    e.csum_count = 2;
    e.csum_type = VTX_CSUM_CRC32C;
    assert(vtx_extent_p0_value_valid(&e, sizeof(e) + 8));
    r.start_vblk = 20;
    r.block_count = 4;
    r.allocation_id = 900;
    r.refcount = 1;
    assert(vtx_extent_within_ref(&e, &r));
    e.vblk = 23;
    assert(!vtx_extent_within_ref(&e, &r));
    e.vblk = 20;
    e.payload_bytes = 1;
    assert(!vtx_extent_p0_value_valid(&e, sizeof(e) + 9));
    e.payload_bytes = 0;
    e.allocation_id = 0;
    assert(!vtx_extent_p0_value_valid(&e, sizeof(e) + 8));

    memset(&e, 0, sizeof(e));
    e.flags = VTX_EXTENT_INLINE;
    e.header_bytes = sizeof(e);
    e.logical_bytes = 30;
    e.payload_bytes = 30;
    assert(vtx_extent_p0_value_valid(&e, sizeof(e) + 30));
    e.allocation_id = 1;
    assert(!vtx_extent_p0_value_valid(&e, sizeof(e) + 30));
    e.allocation_id = 0;
    e.flags |= VTX_EXTENT_SHARED;
    assert(!vtx_extent_p0_value_valid(&e, sizeof(e) + 30));
    e.flags = VTX_EXTENT_INLINE;
    e.payload_bytes = VTXFS_MAX_INLINE_DATA + 1;
    e.logical_bytes = e.payload_bytes;
    assert(!vtx_extent_p0_value_valid(&e, sizeof(e) + VTXFS_MAX_INLINE_DATA + 1));

    puts("PASS: malicious layouts, feature gates, value overlap, refs, inline extents");
    return 0;
}
