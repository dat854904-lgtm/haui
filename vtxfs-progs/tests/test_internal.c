/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../include/vtxfs_internal.h"

#define NS 4096U
static struct vtx_key key_of(unsigned int n)
{
    struct vtx_key k = {
        .oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(10U + n, VTX_TYPE_DIR_ITEM)),
        .offset = vtx_le64(n * 4096U)
    };
    return k;
}
static struct vtx_key_ptr_val child_of(unsigned int n, int phys)
{
    struct vtx_key_ptr_val p = {
        .child_oid = vtx_le64(1000U + n),
        .hint_vblk = vtx_le64(phys ? 100U + n : 0),
        .hint_xid = vtx_le64(phys ? 42 : 0)
    };
    return p;
}
static void validate(const unsigned char *n, unsigned int expected)
{
    const struct vtx_node_hdr *h = (const void *)n;
    unsigned int i;
    assert(vtx_node_slots_valid(h, NS));
    assert(vtx_cpu16(h->nitems) == expected);
    for (i = 0; i < expected; i++) {
        struct vtx_key k = key_of(i);
        unsigned int slot = 999;
        assert(vtx_key_cmp(&h->entries[i].key, &k) == 0);
        assert(vtx_cpu16(h->entries[i].slot.val_len) ==
               sizeof(struct vtx_key_ptr_val));
        assert(vtx_internal_find(n, NS, &k, &slot) == 0);
        assert(slot == i);
    }
}
static void run(int physical)
{
    unsigned char *node = malloc(NS);
    unsigned char *scratch = malloc(NS);
    unsigned char *saved = malloc(NS);
    struct vtx_node_hdr *h;
    struct vtx_key k, missing;
    struct vtx_key_ptr_val p;
    unsigned int i;
    int r;
    assert(node && scratch && saved);
    assert(vtx_internal_init(node, NS, 5, 42, 7, VTX_TREE_FS,
                             VTX_NODE_FLAG_ROOT |
                             (physical ? VTX_NODE_FLAG_PHYSICAL_VBLK :
                                         VTX_NODE_FLAG_VIRTUAL_OID), 1) == 0);
    assert(vtx_internal_init(node, NS, 5, 42, 7, VTX_TREE_FS,
                             VTX_NODE_FLAG_LEAF, 1) == -EINVAL);
    assert(vtx_internal_init(node, NS, 5, 42, 7, VTX_TREE_FS,
                             VTX_NODE_FLAG_VIRTUAL_OID |
                             VTX_NODE_FLAG_PHYSICAL_VBLK, 1) == -EINVAL);
    /* last two init failures must not mutate the valid node */
    for (i = 0; i < 70; i++) {
        k = key_of(i);
        p = child_of(i, physical);
        assert(vtx_internal_insert(node, NS, &k, &p, scratch, NS) == 0);
    }
    validate(node, 70);
    k = key_of(0);
    p = child_of(0, physical);
    memcpy(saved, node, NS);
    assert(vtx_internal_insert(node, NS, &k, &p, scratch, NS) == -EEXIST);
    assert(!memcmp(saved, node, NS));
    assert(vtx_internal_insert(node, NS, &k, &p, node, NS) == -EINVAL);
    assert(!memcmp(saved, node, NS));

    p.child_oid = 0;
    assert(vtx_internal_replace(node, NS, &k, &p, scratch, NS) == -EINVAL);
    p = child_of(0, physical);
    p.child_oid = vtx_le64(9000);
    assert(vtx_internal_replace(node, NS, &k, &p, scratch, NS) == 0);
    h = (void *)node;
    assert(vtx_cpu16(h->entries[0].slot.val_len) == 24);

    missing = key_of(500);
    assert(vtx_internal_delete(node, NS, &missing, scratch, NS) == -ENOENT);

    /* Mutation with caller key inside scratch snapshots the key first. */
    k = key_of(500);
    p = child_of(500, physical);
    memcpy(scratch, &k, sizeof(k));
    assert(vtx_internal_insert(node, NS, (const void *)scratch,
                               &p, scratch, NS) == 0);
    assert(vtx_internal_find(node, NS, &k, &i) == 0);
    assert(vtx_internal_delete(node, NS, &k, scratch, NS) == 0);
    for (i = 0; i < 70; i++) {
        k = key_of(i);
        assert(vtx_internal_delete(node, NS, &k, scratch, NS) == 0);
    }
    validate(node, 0);
    assert(vtx_internal_compact(node, NS, scratch, NS) == 0);

    /* Bad val_len is not accepted even when geometrical offsets fit. */
    k = key_of(1);
    p = child_of(1, physical);
    assert(vtx_internal_insert(node, NS, &k, &p, scratch, NS) == 0);
    h = (void *)node;
    h->entries[0].slot.val_len = vtx_le16(8);
    assert(vtx_internal_find(node, NS, &k, &i) == -EUCLEAN);

    printf("PASS: internal %s find/insert/replace/delete/compact, negative ABI\n",
           physical ? "PHYSICAL_VBLK" : "VIRTUAL_OID");
    free(saved);
    free(scratch);
    free(node);
    (void)r;
}
int main(void) { run(0); run(1); return 0; }
