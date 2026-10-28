/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/vtxfs_btree_ro.h"
#include "../include/vtxfs_leaf.h"
#include "../include/vtxfs_internal.h"

#define NS 4096U
struct ctx {
    unsigned char nodes[3][NS];
    int verified;
    int releases, reads, resolves;
    int corrupt_size;
};
static struct vtx_key keyof(__u64 oid)
{
    struct vtx_key k = {
        .oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(oid,VTX_TYPE_INODE)),
        .offset = 0
    };
    return k;
}
static int fetch(void *arg, __u64 vblk, const void **out,
                 size_t *bytes, int *verified)
{
    struct ctx *c=arg;
    unsigned int i;
    if (vblk<100 || vblk>102) return -ENOENT;
    i=(unsigned int)(vblk-100);
    *out = c->nodes[i];
    *bytes = c->corrupt_size ? 2048 : NS;
    *verified = c->verified;
    c->reads++;
    return 0;
}
static void release(void *arg, const void *buf)
{
    struct ctx *c=arg;
    (void)buf;
    c->releases++;
}
static int resolve(void *arg, __u64 view_id, __u64 oid, __u64 *vblk)
{
    struct ctx *c=arg;
    c->resolves++;
    if (view_id!=999) return -ENOENT;
    if (oid==21) { *vblk=101; return 0; }
    if (oid==22) { *vblk=102; return 0; }
    return -ENOENT;
}
static void scenario(int physical)
{
    struct ctx *c=calloc(1,sizeof(*c));
    unsigned char *scratch=malloc(NS);
    struct vtx_ro_ops ops={fetch,release,resolve};
    struct vtx_ro_tree t;
    struct vtx_key k1=keyof(10), k2=keyof(20), query;
    struct vtx_key_ptr_val p1={0},p2={0};
    unsigned char result[40];
    size_t bytes;
    int ret;
    assert(c && scratch);
    c->verified=1;
    assert(vtx_leaf_init(c->nodes[1],NS,21,30,101,VTX_TREE_FS,
                         VTX_NODE_FLAG_LEAF)==0);
    assert(vtx_leaf_init(c->nodes[2],NS,22,30,102,VTX_TREE_FS,
                         VTX_NODE_FLAG_LEAF)==0);
    assert(vtx_leaf_insert(c->nodes[1],NS,&k1,"FIRST",5,scratch,NS)==0);
    assert(vtx_leaf_insert(c->nodes[2],NS,&k2,"SECOND",6,scratch,NS)==0);
    assert(vtx_internal_init(c->nodes[0],NS,1,50,100,VTX_TREE_FS,
                             VTX_NODE_FLAG_ROOT |
                             (physical ? VTX_NODE_FLAG_PHYSICAL_VBLK :
                                         VTX_NODE_FLAG_VIRTUAL_OID),1)==0);
    p1.child_oid=vtx_le64(21);
    p2.child_oid=vtx_le64(22);
    p1.hint_vblk=vtx_le64(physical ? 101 : 79999);
    p2.hint_vblk=vtx_le64(physical ? 102 : 79999);
    p1.hint_xid=vtx_le64(30);
    p2.hint_xid=vtx_le64(30);
    assert(vtx_internal_insert(c->nodes[0],NS,&k1,&p1,scratch,NS)==0);
    assert(vtx_internal_insert(c->nodes[0],NS,&k2,&p2,scratch,NS)==0);
    t=(struct vtx_ro_tree){ .ops=&ops,.ctx=c,.view_id=999,.root_oid=1,
        .root_vblk=100,.max_xid=55,.owner_id=0,.node_bytes=NS,
        .tree_type=VTX_TREE_FS,.root_level=1 };
    bytes=sizeof(result);
    assert(vtx_ro_lookup(&t,&k1,result,&bytes)==0);
    assert(bytes==5 && !memcmp(result,"FIRST",5));
    bytes=sizeof(result);
    assert(vtx_ro_lookup(&t,&k2,result,&bytes)==0);
    assert(bytes==6 && !memcmp(result,"SECOND",6));
    assert(!physical ? c->resolves==2 : c->resolves==0);
    query=keyof(19);
    bytes=sizeof(result);
    assert(vtx_ro_lookup(&t,&query,result,&bytes)==-ENOENT);
    bytes=2;
    assert(vtx_ro_lookup(&t,&k2,result,&bytes)==-ENOBUFS && bytes==6);
    assert(c->reads==c->releases);

    c->verified=0;
    bytes=sizeof(result);
    assert(vtx_ro_lookup(&t,&k1,result,&bytes)==-EUCLEAN);
    c->verified=1;
    c->corrupt_size=1;
    assert(vtx_ro_lookup(&t,&k1,result,&bytes)==-EUCLEAN);
    c->corrupt_size=0;

    /* Wrong root XID and bad view must not give a hint fallback. */
    t.max_xid=49;
    assert(vtx_ro_lookup(&t,&k1,result,&bytes)==-EUCLEAN);
    t.max_xid=55;
    if (!physical) {
        t.view_id=1;
        assert(vtx_ro_lookup(&t,&k1,result,&bytes)==-ENOENT);
        t.view_id=999;
    }
    /* Separator must match the child's minimum key. */
    ((struct vtx_node_hdr *)(void *)c->nodes[0])->entries[0].key =
        keyof(9);
    assert(vtx_ro_lookup(&t,&k1,result,&bytes)==-EUCLEAN);
    assert(c->reads==c->releases);
    printf("PASS read-only walker: %s child mapping, validation, no fallback\n",
           physical?"physical":"OMAP view");
    free(scratch); free(c);
    (void)ret;
}
int main(void) { scenario(0); scenario(1); return 0; }
