/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../include/vtxfs_validate.h"

int main(void)
{
    __u32 vag, off;
    struct vtx_key a = {
        .oid_and_type = VTX_MAKE_OID_TYPE(5, VTX_TYPE_EXTENT),
        .offset = 4096
    };
    struct vtx_key b = {
        .oid_and_type = VTX_MAKE_OID_TYPE(900, VTX_TYPE_INODE),
        .offset = 0
    };
    struct vtx_node_hdr h = {0};
    struct vtx_extent_item ext = {0};

    assert(vtx_key_cmp(&a, &b) < 0);
    assert(vtx_key_cmp(&b, &a) > 0);
    assert(vtx_key_cmp(&a, &a) == 0);
    assert(vtx_oid_valid(1));
    assert(!vtx_oid_valid(0));
    assert(!vtx_oid_valid(VTX_KEY_OID_MASK+1));

    assert(vtx_vblk_decode(1, &vag, &off) == 0 && vag == 0 && off == 1);
    assert(vtx_vblk_decode(VTXFS_MAX_VBLKS-1, &vag, &off) == 0);
    assert(vag == VTXFS_MAX_VAGS-1 && off == VTXFS_VAG_BLOCKS-1);
    assert(vtx_vblk_decode(0, &vag, &off) == -1);
    assert(vtx_vblk_decode(VTXFS_MAX_VBLKS, &vag, &off) == -1);
    assert(vtx_vblk_decode(1, NULL, &off) == -1);

    h.magic = VTXFS_NODE_MAGIC;
    h.flags = VTX_NODE_FLAG_LEAF;
    h.tree_type = VTX_TREE_FS;
    h.nitems = 0;
    h.free_head8 = sizeof(h)/8;
    h.free_tail8 = 4096/8;
    assert(vtx_node_geometry_valid(&h, 4096));
    assert(!vtx_node_geometry_valid(&h, 1024));
    assert(!vtx_node_geometry_valid(&h, 4097));
    h.free_tail8 = 1;
    assert(!vtx_node_geometry_valid(&h, 4096));
    h.free_tail8 = 4096/8;
    h.nitems = 1;
    assert(!vtx_node_geometry_valid(&h, 4096));
    h.free_head8 = (sizeof(h)+sizeof(struct vtx_node_entry))/8;
    assert(vtx_node_geometry_valid(&h, 4096));
    h.free_tail8 = 65536/8;
    assert(vtx_node_geometry_valid(&h, 65536));

    ext.header_bytes = sizeof(ext);
    ext.vblk = 1;
    ext.allocation_id = 42;
    ext.physical_blocks = 1;
    ext.logical_bytes = 4096;
    ext.csum_type = VTX_CSUM_CRC32C;
    ext.csum_count = 1;
    assert(vtx_extent_p0_value_valid(&ext, sizeof(ext)+4));
    assert(!vtx_extent_p0_value_valid(&ext, sizeof(ext)));
    ext.encryption = VTX_ENC_CHACHA20_POLY1305;
    assert(!vtx_extent_p0_value_valid(&ext, sizeof(ext)+4));
    ext.encryption = VTX_ENC_NONE;
    ext.logical_bytes = 8192;
    assert(!vtx_extent_p0_value_valid(&ext, sizeof(ext)+4));
    ext.logical_bytes = 4096;
    ext.vblk = VTXFS_MAX_VBLKS;
    assert(!vtx_extent_p0_value_valid(&ext, sizeof(ext)+4));

    puts("PASS: key comparison, VBLK bounds, node geometry, extent P0 validation");
    return 0;
}
