/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "../include/vtxfs_leaf.h"
int main(void)
{
    __le16 a = vtx_le16(0x1234U);
    __le32 b = vtx_le32(0x12345678U);
    __le64 c = vtx_le64(0x0123456789abcdefULL);
    const unsigned char aw[2] = {0x34,0x12};
    const unsigned char bw[4] = {0x78,0x56,0x34,0x12};
    const unsigned char cw[8] = {0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01};
    unsigned char node[4096], scratch[4096], previous[4096];
    struct vtx_key k={vtx_le64(VTX_MAKE_OID_TYPE(500,VTX_TYPE_INODE)),0};
    struct vtx_key missing={vtx_le64(VTX_MAKE_OID_TYPE(501,VTX_TYPE_INODE)),0};
    unsigned int slot=0;
    assert(!memcmp(&a,aw,sizeof(a)) && vtx_cpu16(a)==0x1234U);
    assert(!memcmp(&b,bw,sizeof(b)) && vtx_cpu32(b)==0x12345678U);
    assert(!memcmp(&c,cw,sizeof(c)) && vtx_cpu64(c)==0x0123456789abcdefULL);
    assert(vtx_leaf_init(node,sizeof(node),123,9,400,VTX_TREE_FS,VTX_NODE_FLAG_LEAF)==0);
    /* Key can point at scratch, because it is snapshotted before memset. */
    memcpy(scratch,&k,sizeof(k));
    assert(vtx_leaf_insert(node,sizeof(node),(const void *)scratch,"ABC",3,scratch,sizeof(scratch))==0);
    assert(vtx_leaf_find(node,sizeof(node),&k,&slot)==0);
    /* Overlapping value is forbidden; node must remain unchanged. */
    memcpy(previous,node,sizeof(node));
    assert(vtx_leaf_insert(node,sizeof(node),&missing,scratch+128,3,scratch,sizeof(scratch))==-EINVAL);
    assert(!memcmp(previous,node,sizeof(node)));
    puts("PASS: on-disk endian bytes and scratch-key/value alias protections");
    return 0;
}
