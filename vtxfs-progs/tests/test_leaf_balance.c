/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/vtxfs_leaf.h"
#define N 4096U

static struct vtx_key key_of(__u64 index)
{
    struct vtx_key key = {
        .oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(10+index, VTX_TYPE_INODE)),
        .offset = vtx_le64(index*512)
    };
    return key;
}
static unsigned int count(const void *ptr)
{
    return vtx_cpu16(((const struct vtx_node_hdr *)ptr)->nitems);
}
static void verify(void *left, void *right, __u32 expected)
{
    const struct vtx_node_hdr *a=left, *b=right;
    __u32 i, seen=0;
    assert(vtx_node_slots_valid(a,N) && vtx_node_slots_valid(b,N));
    assert(count(a)+count(b)==expected);
    for (i=0; i<count(a); ++i) {
        struct vtx_key want=key_of(seen++);
        assert(vtx_key_cmp(&want, &a->entries[i].key)==0);
        assert(vtx_cpu16(a->entries[i].slot.val_len)==40);
    }
    for (i=0; i<count(b); ++i) {
        struct vtx_key want=key_of(seen++);
        assert(vtx_key_cmp(&want, &b->entries[i].key)==0);
        assert(vtx_cpu16(b->entries[i].slot.val_len)==40);
    }
    assert(seen==expected);
}
static void prepare(unsigned char *a, unsigned char *b)
{
    assert(vtx_leaf_init(a,N,200,42,500,VTX_TREE_FS,VTX_NODE_FLAG_LEAF)==0);
    assert(vtx_leaf_init(b,N,201,42,505,VTX_TREE_FS,VTX_NODE_FLAG_LEAF)==0);
}
int main(void)
{
    unsigned char *a=malloc(N), *b=malloc(N), *sc=malloc(2*N);
    unsigned char *copya=malloc(N), *copyb=malloc(N), value[512];
    unsigned int i, original;
    struct vtx_key key;
    assert(a && b && sc && copya && copyb);
    memset(value,'Z',sizeof(value));
    prepare(a,b);
    for(i=0;i<30;++i) {
        key=key_of(i);
        assert(vtx_leaf_insert(a,N,&key,value,40,sc,N)==0);
    }
    assert(vtx_leaf_split(a,b,N,sc,2*N)==0);
    assert(count(a)>0 && count(b)>0);
    verify(a,b,30);
    assert(vtx_cpu64(((struct vtx_node_hdr *)b)->oid)==201);
    assert(vtx_cpu64(((struct vtx_node_hdr *)b)->self_vblk)==505);
    assert(vtx_leaf_merge(a,b,N,sc,2*N)==0);
    verify(a,b,30);
    assert(count(b)==0);
    assert(vtx_leaf_split(a,b,N,sc,2*N)==0);
    verify(a,b,30);
    original=count(a);
    assert(vtx_leaf_rebalance(a,b,N,sc,2*N)==0);
    verify(a,b,30);
    assert(count(a)==count(b) || count(a)==count(b)+1 ||
           count(b)==count(a)+1);
    assert(original>0);

    /* Near-full leaf; split & insert at the rightmost key in one operation. */
    prepare(a,b);
    for(i=0;i<55;++i) {
        key=key_of(i);
        assert(vtx_leaf_insert(a,N,&key,value,40,sc,N)==0);
    }
    key=key_of(55);
    assert(vtx_leaf_split_insert(a,b,N,&key,value,40,sc,2*N)==0);
    verify(a,b,56);

    /* Negative cases must leave BOTH buffers byte-for-byte unchanged. */
    memcpy(copya,a,N);
    memcpy(copyb,b,N);
    assert(vtx_leaf_split(a,b,N,sc,2*N)==-EINVAL); /* RHS nonempty */
    assert(!memcmp(a,copya,N) && !memcmp(b,copyb,N));
    assert(vtx_leaf_rebalance(a,b,N,a,2*N)==-EINVAL); /* scratch alias */
    assert(!memcmp(a,copya,N) && !memcmp(b,copyb,N));
    assert(vtx_leaf_merge(a,b,N,sc,2*N)==0); /* 56*64 fits one leaf */
    assert(count(b)==0);

    /* Stress exact capacity: two sets that cannot be merged. */
    prepare(a,b);
    for(i=0;i<45;++i) {
        key=key_of(i);
        assert(vtx_leaf_insert(a,N,&key,value,60,sc,N)==0);
    }
    for(i=45;i<90;++i) {
        key=key_of(i);
        assert(vtx_leaf_insert(b,N,&key,value,60,sc,N)==0);
    }
    memcpy(copya,a,N); memcpy(copyb,b,N);
    assert(vtx_leaf_merge(a,b,N,sc,2*N)==-ENOSPC);
    assert(!memcmp(copya,a,N) && !memcmp(copyb,b,N));
    assert(vtx_leaf_rebalance(a,b,N,sc,2*N)==0);
    assert(count(a)+count(b)==90);

    puts("PASS: split/split-insert/merge/rebalance, identity, ENOSPC all-or-nothing");
    free(copyb); free(copya); free(sc); free(b); free(a);
    return 0;
}
