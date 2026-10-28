/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/vtxfs_internal.h"

#define NS 4096U

static struct vtx_key mk(__u64 oid)
{
    struct vtx_key k = {
        .oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(oid, VTX_TYPE_INODE)),
        .offset = 0
    };
    return k;
}

static void run_mode(__u16 mode_flag)
{
    unsigned char *left = calloc(1, NS);
    unsigned char *right = calloc(1, NS);
    unsigned char *scratch = calloc(1, NS * 2U);
    unsigned char *snap_l = calloc(1, NS);
    unsigned char *snap_r = calloc(1, NS);
    const struct vtx_node_hdr *hl, *hr;
    unsigned int i, cap;
    struct vtx_key k;
    struct vtx_key_ptr_val p = {0};

    assert(left && right && scratch && snap_l && snap_r);
    assert(vtx_internal_init(left, NS, 10, 5, 200, VTX_TREE_FS,
                             mode_flag, 1) == 0);
    assert(vtx_internal_init(right, NS, 11, 5, 201, VTX_TREE_FS,
                             mode_flag, 1) == 0);

    cap = (NS - sizeof(struct vtx_node_hdr)) /
          (sizeof(struct vtx_node_entry) + sizeof(struct vtx_key_ptr_val));
    assert(cap == 84U);

    for (i = 0; i < cap; i++) {
        k = mk(100 + i * 2);
        p.child_oid = vtx_le64(1000 + i);
        p.hint_vblk = vtx_le64(300 + i);
        p.hint_xid = vtx_le64(5);
        assert(vtx_internal_insert(left, NS, &k, &p, scratch, NS) == 0);
    }

    /* Full node refuses direct insert, succeeds via split_insert. */
    k = mk(101);
    p.child_oid = vtx_le64(9999);
    p.hint_vblk = vtx_le64(999);
    p.hint_xid = vtx_le64(5);
    assert(vtx_internal_insert(left, NS, &k, &p, scratch, NS) == -ENOSPC);

    /* Duplicate key on split_insert leaves both nodes unchanged. */
    memcpy(snap_l, left, NS);
    memcpy(snap_r, right, NS);
    k = mk(100);
    assert(vtx_internal_split_insert(left, right, NS, &k, &p,
                                     scratch, NS * 2U) == -EEXIST);
    assert(memcmp(left, snap_l, NS) == 0);
    assert(memcmp(right, snap_r, NS) == 0);

    k = mk(101);
    assert(vtx_internal_split_insert(left, right, NS, &k, &p,
                                     scratch, NS * 2U) == 0);
    hl = (const struct vtx_node_hdr *)left;
    hr = (const struct vtx_node_hdr *)right;
    assert(vtx_cpu16(hl->nitems) + vtx_cpu16(hr->nitems) == cap + 1U);
    assert(vtx_cpu64(hl->oid) == 10 && vtx_cpu64(hr->oid) == 11);
    assert(vtx_key_cmp(&hl->entries[vtx_cpu16(hl->nitems) - 1].key,
                       &hr->entries[0].key) < 0);

    /* Cannot merge 85 items into a single 84-capacity node. */
    assert(vtx_internal_merge(left, right, NS, scratch, NS * 2U) == -ENOSPC);

    /* Delete 2 items from right, rebalance, then delete enough to merge. */
    for (i = 0; i < 20; i++) {
        struct vtx_key dk = hr->entries[0].key;
        assert(vtx_internal_delete(right, NS, &dk, scratch, NS) == 0);
    }
    assert(vtx_internal_rebalance(left, right, NS, scratch, NS * 2U) == 0);
    assert(vtx_cpu16(hl->nitems) == 32 && vtx_cpu16(hr->nitems) == 33);

    assert(vtx_internal_merge(left, right, NS, scratch, NS * 2U) == 0);
    assert(vtx_cpu16(hl->nitems) == 65 && vtx_cpu16(hr->nitems) == 0);

    assert(vtx_internal_split(left, right, NS, scratch, NS * 2U) == 0);
    assert(vtx_cpu16(hl->nitems) == 32 && vtx_cpu16(hr->nitems) == 33);

    free(left);
    free(right);
    free(scratch);
    free(snap_l);
    free(snap_r);
}

int main(void)
{
    run_mode(VTX_NODE_FLAG_VIRTUAL_OID);
    run_mode(VTX_NODE_FLAG_PHYSICAL_VBLK);
    puts("PASS: internal node split/split-insert/merge/rebalance (VIRTUAL & PHYSICAL)");
    return 0;
}
