/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/vtxfs_btree_rw.h"
#include "../include/vtxfs_btree_ro.h"
#include "../include/vtxfs_leaf.h"
#include "../include/vtxfs_internal.h"

#define NS 4096U
#define MAX_NODES 2048U
#define MAX_OID   4096U

struct sim_ctx {
    unsigned char nodes[MAX_NODES][NS];
    int in_use[MAX_NODES];
    __u64 omap[MAX_OID];
    __u64 next_oid;
    __u64 tx_xid;
};

static int sim_read(void *arg, __u64 vblk, const void **data,
                    size_t *bytes, int *verified)
{
    struct sim_ctx *c = arg;
    if (vblk == 0 || vblk >= MAX_NODES || !c->in_use[vblk])
        return -ENOENT;
    *data = c->nodes[vblk];
    *bytes = NS;
    *verified = 1;
    return 0;
}

static void sim_release(void *arg, const void *data)
{
    (void)arg;
    (void)data;
}

static __u64 alloc_vblk(struct sim_ctx *c)
{
    __u64 v;
    for (v = 1; v < MAX_NODES; v++) {
        if (!c->in_use[v]) {
            c->in_use[v] = 1;
            memset(c->nodes[v], 0, NS);
            return v;
        }
    }
    assert(0 && "out of simulated VBLKs");
    return 0;
}

static int sim_cow(void *arg, __u8 tree_type, __u8 level, __u64 oid,
                   __u64 old_vblk, __u64 *new_vblk, void **wbuf)
{
    struct sim_ctx *c = arg;
    struct vtx_node_hdr *h;
    __u64 nv;
    (void)tree_type;
    (void)level;
    (void)oid;
    assert(old_vblk > 0 && old_vblk < MAX_NODES && c->in_use[old_vblk]);
    h = (struct vtx_node_hdr *)c->nodes[old_vblk];
    if (vtx_cpu64(h->xid) == c->tx_xid) {
        *new_vblk = old_vblk;
        *wbuf = c->nodes[old_vblk];
        return 0;
    }
    nv = alloc_vblk(c);
    memcpy(c->nodes[nv], c->nodes[old_vblk], NS);
    c->in_use[old_vblk] = 0;
    *new_vblk = nv;
    *wbuf = c->nodes[nv];
    return 0;
}

static int sim_alloc(void *arg, __u8 tree_type, __u8 level,
                     __u64 *new_oid, __u64 *new_vblk, void **wbuf)
{
    struct sim_ctx *c = arg;
    __u64 nv = alloc_vblk(c);
    (void)tree_type;
    (void)level;
    assert(c->next_oid < MAX_OID);
    *new_oid = c->next_oid++;
    *new_vblk = nv;
    *wbuf = c->nodes[nv];
    return 0;
}

static int sim_free(void *arg, __u8 tree_type, __u8 level,
                    __u64 oid, __u64 vblk)
{
    struct sim_ctx *c = arg;
    (void)tree_type;
    (void)level;
    (void)oid;
    assert(vblk > 0 && vblk < MAX_NODES && c->in_use[vblk]);
    c->in_use[vblk] = 0;
    return 0;
}

static int sim_omap_resolve(void *arg, __u64 view_id, __u64 oid, __u64 *vblk)
{
    struct sim_ctx *c = arg;
    assert(view_id == 77);
    if (oid >= MAX_OID || !c->omap[oid])
        return -ENOENT;
    *vblk = c->omap[oid];
    return 0;
}

static int sim_omap_set(void *arg, __u64 view_id, __u64 oid, __u64 vblk)
{
    struct sim_ctx *c = arg;
    assert(view_id == 77 && oid < MAX_OID);
    c->omap[oid] = vblk;
    return 0;
}

static int sim_omap_del(void *arg, __u64 view_id, __u64 oid)
{
    struct sim_ctx *c = arg;
    assert(view_id == 77 && oid < MAX_OID);
    c->omap[oid] = 0;
    return 0;
}

static struct vtx_key make_k(__u64 oid, __u64 off)
{
    struct vtx_key k = {
        .oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(oid, VTX_TYPE_EXTENT)),
        .offset = vtx_le64(off)
    };
    return k;
}

static void run_test(int is_physical)
{
    struct sim_ctx *c = calloc(1, sizeof(*c));
    unsigned char *scratch = calloc(1, NS * 3U);
    struct vtx_rw_ops rw_ops = {
        sim_read, sim_release, sim_cow, sim_alloc, sim_free,
        sim_omap_resolve, sim_omap_set, sim_omap_del
    };
    struct vtx_ro_ops ro_ops = {
        sim_read, sim_release, sim_omap_resolve
    };
    struct vtx_rw_tree t;
    struct vtx_ro_tree ro;
    unsigned char payload[512], out[512];
    size_t bytes;
    unsigned int i, count = 600;
    struct vtx_key cur_k, found_k;

    assert(c && scratch);
    c->next_oid = 10;
    c->tx_xid = 1;
    c->in_use[1] = 1;
    assert(vtx_leaf_init(c->nodes[1], NS, 1, 1, 1, VTX_TREE_FS,
                         VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT) == 0);
    ((struct vtx_node_hdr *)c->nodes[1])->owner_id = vtx_le64(256);
    if (!is_physical)
        c->omap[1] = 1;

    memset(&t, 0, sizeof(t));
    t.ops = &rw_ops;
    t.ctx = c;
    t.scratch = scratch;
    t.scratch_bytes = NS * 3U;
    t.view_id = 77;
    t.root_oid = 1;
    t.root_vblk = 1;
    t.tx_xid = 1;
    t.owner_id = 256;
    t.node_bytes = NS;
    t.tree_type = VTX_TREE_FS;
    t.root_level = 0;
    t.is_physical = (__u8)is_physical;

    /* Insert 600 384-byte items in scrambled order to force level 2 splits. */
    for (i = 0; i < count; i++) {
        unsigned int idx = (i * 173U + 29U) % count;
        struct vtx_key k = make_k(1000 + idx, idx * 4096ULL);
        c->tx_xid = 1 + i / 25U;
        t.tx_xid = c->tx_xid;
        memset(payload, (int)(idx & 0xffU), 384);
        assert(vtx_rw_insert(&t, &k, payload, 384) == 0);
        assert(vtx_rw_insert(&t, &k, payload, 384) == -EEXIST);
    }
    assert(t.root_level >= 2);

    /* Verify every single key via the strict read-only walker (vtx_ro_lookup). */
    ro.ops = &ro_ops;
    ro.ctx = c;
    ro.view_id = 77;
    ro.root_oid = t.root_oid;
    ro.root_vblk = t.root_vblk;
    ro.max_xid = t.tx_xid;
    ro.owner_id = t.owner_id;
    ro.node_bytes = NS;
    ro.tree_type = VTX_TREE_FS;
    ro.root_level = t.root_level;

    for (i = 0; i < count; i++) {
        struct vtx_key k = make_k(1000 + i, i * 4096ULL);
        bytes = sizeof(out);
        assert(vtx_ro_lookup(&ro, &k, out, &bytes) == 0);
        assert(bytes == 384 && out[0] == (__u8)(i & 0xffU));
    }

    /* Verify ordered forward cursor scan visits all 600 keys in order. */
    cur_k = make_k(1, 0);
    for (i = 0; i < count; i++) {
        struct vtx_key exp_k = make_k(1000 + i, i * 4096ULL);
        bytes = sizeof(out);
        assert(vtx_rw_next(&t, &cur_k, i == 0 ? 1 : 0,
                           &found_k, out, &bytes) == 0);
        assert(vtx_key_cmp(&found_k, &exp_k) == 0);
        cur_k = found_k;
    }
    bytes = sizeof(out);
    assert(vtx_rw_next(&t, &cur_k, 0, &found_k, out, &bytes) == -ENOENT);

    /* Replace and upsert items. */
    for (i = 0; i < count; i += 10) {
        struct vtx_key k = make_k(1000 + i, i * 4096ULL);
        memset(payload, 0xA5, 112);
        assert(vtx_rw_replace(&t, &k, payload, 112) == 0);
        bytes = sizeof(out);
        assert(vtx_rw_lookup(&t, &k, out, &bytes) == 0);
        assert(bytes == 112 && out[0] == 0xA5);
    }

    /* Delete all items and verify tree contracts back to root_level == 0. */
    for (i = 0; i < count; i++) {
        unsigned int idx = (i * 71U + 11U) % count;
        struct vtx_key k = make_k(1000 + idx, idx * 4096ULL);
        c->tx_xid++;
        t.tx_xid = c->tx_xid;
        assert(vtx_rw_delete(&t, &k) == 0);
        assert(vtx_rw_delete(&t, &k) == -ENOENT);
    }
    assert(t.root_level == 0);

    free(scratch);
    free(c);
}

int main(void)
{
    run_test(0);
    run_test(1);
    puts("PASS: multi-level CoW B+Tree (VIRTUAL_OID & PHYSICAL_VBLK), split/contract/cursor");
    return 0;
}
