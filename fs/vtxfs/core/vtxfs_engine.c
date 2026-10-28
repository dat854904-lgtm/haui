/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KERNEL__
#define _POSIX_C_SOURCE 200809L
#endif
#include "vtxfs_engine.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/timekeeping.h>
static inline void *vtx_zalloc(size_t size) { return vzalloc(size); }
static inline void vtx_free(void *ptr) { vfree(ptr); }
#else
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static inline void *vtx_zalloc(size_t size) { return calloc(1, size); }
static inline void vtx_free(void *ptr) { free(ptr); }
#endif

__u64 vtx_time_now_ns(void)
{
#ifdef __KERNEL__
    return (__u64)ktime_get_real_ns();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0ULL;
    return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
#endif
}

struct vtx_fs *vtx_fs_alloc(void)
{
    struct vtx_fs *fs = (struct vtx_fs *)vtx_zalloc(sizeof(struct vtx_fs));
    if (!fs)
        return NULL;
    fs->cache_capacity = VTX_CACHE_INITIAL_NODES;
    fs->cache_max_capacity = VTX_CACHE_MAX_NODES;
    fs->cache_count = 0;
    return fs;
}

static void clear_pending_data(struct vtx_fs *fs)
{
    __u32 i;
    for (i = 0; i < fs->num_pending_data; i++) {
        if (fs->pending_data[i].data) {
            vtx_free(fs->pending_data[i].data);
            fs->pending_data[i].data = NULL;
        }
    }
    fs->num_pending_data = 0;
}

static void clear_node_cache(struct vtx_fs *fs)
{
    struct vtx_cached_node *curr = fs->lru_head;
    while (curr) {
        struct vtx_cached_node *next = curr->lru_next;
        vtx_free(curr);
        curr = next;
    }
    memset(fs->node_hash, 0, sizeof(fs->node_hash));
    fs->lru_head = NULL;
    fs->lru_tail = NULL;
    fs->cache_count = 0;
}

void vtx_fs_free(struct vtx_fs *fs)
{
    if (!fs)
        return;
    clear_pending_data(fs);
    clear_node_cache(fs);
    vtx_vag_destroy(&fs->vag);
    vtx_free(fs);
}

/* -------------------------------------------------------------------------
 * 1. Node Cache & Delayed Reference Queue
 * ------------------------------------------------------------------------- */

static inline __u32 vtx_node_hash_fn(__u64 vblk)
{
    /* Thomas Wang 64-bit to 32-bit integer hash */
    vblk = (~vblk) + (vblk << 18);
    vblk = vblk ^ (vblk >> 31);
    vblk = vblk * 21;
    vblk = vblk ^ (vblk >> 11);
    vblk = vblk + (vblk << 6);
    vblk = vblk ^ (vblk >> 22);
    return (__u32)(vblk & (VTX_NODE_HASH_BUCKETS - 1U));
}

static void lru_add_head(struct vtx_fs *fs, struct vtx_cached_node *node)
{
    node->lru_prev = NULL;
    node->lru_next = fs->lru_head;
    if (fs->lru_head)
        fs->lru_head->lru_prev = node;
    else
        fs->lru_tail = node;
    fs->lru_head = node;
}

static void lru_remove(struct vtx_fs *fs, struct vtx_cached_node *node)
{
    if (node->lru_prev)
        node->lru_prev->lru_next = node->lru_next;
    else
        fs->lru_head = node->lru_next;
    if (node->lru_next)
        node->lru_next->lru_prev = node->lru_prev;
    else
        fs->lru_tail = node->lru_prev;
    node->lru_prev = NULL;
    node->lru_next = NULL;
}

static void lru_touch(struct vtx_fs *fs, struct vtx_cached_node *node)
{
    if (fs->lru_head == node)
        return;
    lru_remove(fs, node);
    lru_add_head(fs, node);
}

static void node_hash_insert(struct vtx_fs *fs, struct vtx_cached_node *node)
{
    __u32 h = vtx_node_hash_fn(node->vblk);
    node->hash_next = fs->node_hash[h];
    fs->node_hash[h] = node;
}

static void node_hash_remove(struct vtx_fs *fs, struct vtx_cached_node *node)
{
    __u32 h = vtx_node_hash_fn(node->vblk);
    struct vtx_cached_node **curr = &fs->node_hash[h];
    while (*curr) {
        if (*curr == node) {
            *curr = node->hash_next;
            node->hash_next = NULL;
            return;
        }
        curr = &((*curr)->hash_next);
    }
}

static struct vtx_cached_node *find_cached_node(struct vtx_fs *fs, __u64 vblk)
{
    __u32 h = vtx_node_hash_fn(vblk);
    struct vtx_cached_node *c = fs->node_hash[h];
    while (c) {
        if (c->in_use && c->vblk == vblk) {
            lru_touch(fs, c);
            return c;
        }
        c = c->hash_next;
    }
    return NULL;
}

struct vtx_cached_node *vtx_find_cached_node(struct vtx_fs *fs, __u64 vblk)
{
    if (!fs)
        return NULL;
    return find_cached_node(fs, vblk);
}

static int is_active_tree_root(const struct vtx_fs *fs, __u64 vblk)
{
    if (!vblk)
        return 0;
    if (fs->omap_meta_tree.root_vblk == vblk ||
        fs->root_tree.root_vblk == vblk ||
        fs->ref_tree.root_vblk == vblk ||
        fs->reaper_tree.root_vblk == vblk ||
        fs->sfq_tree.root_vblk == vblk ||
        fs->uuid_tree.root_vblk == vblk)
        return 1;
    if (fs->active_subvol_id != 0) {
        if (fs->active_omap_view.root_vblk == vblk ||
            fs->active_fs_tree.root_vblk == vblk)
            return 1;
    }
    return 0;
}

static struct vtx_cached_node *alloc_cache_slot(struct vtx_fs *fs, __u64 vblk)
{
    struct vtx_cached_node *c = find_cached_node(fs, vblk);
    struct vtx_cached_node *cand;

    if (c)
        return c;

    /* 1. Allocate on demand if below capacity */
    if (fs->cache_count < fs->cache_capacity) {
        c = (struct vtx_cached_node *)vtx_zalloc(sizeof(*c));
        if (c) {
            c->in_use = 1;
            c->vblk = vblk;
            node_hash_insert(fs, c);
            lru_add_head(fs, c);
            fs->cache_count++;
            return c;
        }
    }

    /* 2. Evict least recently used clean slot, skipping active tree roots */
    cand = fs->lru_tail;
    while (cand) {
        if (!cand->dirty && !cand->pinned &&
            !is_active_tree_root(fs, cand->vblk)) {
            node_hash_remove(fs, cand);
            cand->vblk = vblk;
            cand->oid = 0;
            cand->tree_type = 0;
            cand->level = 0;
            cand->dirty = 0;
            cand->pinned = 0;
            cand->in_use = 1;
            cand->hash_next = NULL;
            memset(cand->reserved, 0, sizeof(cand->reserved));
            memset(cand->data, 0, sizeof(cand->data));
            node_hash_insert(fs, cand);
            lru_touch(fs, cand);
            return cand;
        }
        cand = cand->lru_prev;
    }

    /* 3. Under memory pressure, grow dynamic cache capacity if below max limit */
    if (fs->cache_count < fs->cache_max_capacity) {
        __u32 new_cap = fs->cache_capacity * 2U;
        if (new_cap > fs->cache_max_capacity)
            new_cap = fs->cache_max_capacity;
        fs->cache_capacity = new_cap;
        c = (struct vtx_cached_node *)vtx_zalloc(sizeof(*c));
        if (c) {
            c->in_use = 1;
            c->vblk = vblk;
            node_hash_insert(fs, c);
            lru_add_head(fs, c);
            fs->cache_count++;
            return c;
        }
    }

    /* 4. Fallback: evict any clean, non-pinned LRU slot */
    cand = fs->lru_tail;
    while (cand) {
        if (!cand->dirty && !cand->pinned) {
            node_hash_remove(fs, cand);
            cand->vblk = vblk;
            cand->oid = 0;
            cand->tree_type = 0;
            cand->level = 0;
            cand->dirty = 0;
            cand->pinned = 0;
            cand->in_use = 1;
            cand->hash_next = NULL;
            memset(cand->reserved, 0, sizeof(cand->reserved));
            memset(cand->data, 0, sizeof(cand->data));
            node_hash_insert(fs, cand);
            lru_touch(fs, cand);
            return cand;
        }
        cand = cand->lru_prev;
    }

    return NULL;
}

static int load_verified_node(struct vtx_fs *fs, __u64 vblk,
                              struct vtx_cached_node **out)
{
    struct vtx_cached_node *c = find_cached_node(fs, vblk);
    __u64 pblk = 0;
    int ret;

    if (c) {
        if (c->dirty) {
            if (!vtx_node_slots_valid((const struct vtx_node_hdr *)c->data,
                                      VTXFS_BLOCK_SIZE))
                return -EUCLEAN;
        }
        *out = c;
        return 0;
    }

    ret = vtx_vag_vblk_to_pblk(&fs->vag, vblk, &pblk);
    if (ret)
        return ret;
    c = alloc_cache_slot(fs, vblk);
    if (!c)
        return -ENOMEM;
    ret = fs->bdev->ops->read_blocks(fs->bdev, pblk, 1, c->data);
    if (ret) {
        c->in_use = 0;
        return ret;
    }
    if (!vtx_node_checksum_verify(c->data, VTXFS_BLOCK_SIZE,
                                  fs->super_blk.hdr.metadata_uuid)) {
        c->in_use = 0;
        return -EUCLEAN;
    }
    {
        const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)c->data;
        c->oid = vtx_cpu64(h->oid);
        c->tree_type = h->tree_type;
        c->level = h->level;
    }
    *out = c;
    return 0;
}

int vtx_enqueue_delayed_ref(struct vtx_fs *fs, __u64 allocation_id,
                            __u64 start_vblk, __u32 block_count,
                            __u32 flags, __u32 owner_hint, __s32 delta)
{
    __u32 i;
    if (!fs || !start_vblk || !block_count || !delta)
        return -EINVAL;

    /* Coalesce opposite delta on a block allocated within the same tx. */
    for (i = 0; i < fs->num_delayed_refs; i++) {
        struct vtx_delayed_ref_entry *e = &fs->delayed_refs[i];
        if (e->start_vblk == start_vblk && e->block_count == block_count) {
            if (e->delta + delta == 0 && e->delta > 0) {
                /* Ephemeral allocation in same transaction: free immediately. */
                vtx_vag_mark_range(&fs->vag, start_vblk, block_count, 0);
                fs->num_delayed_refs--;
                if (i < fs->num_delayed_refs)
                    fs->delayed_refs[i] = fs->delayed_refs[fs->num_delayed_refs];
                return 0;
            }
        }
    }

    if (fs->num_delayed_refs >= VTX_MAX_DELAYED_REFS)
        return -ENOSPC;
    {
        struct vtx_delayed_ref_entry *e =
            &fs->delayed_refs[fs->num_delayed_refs++];
        e->allocation_id = allocation_id;
        e->start_vblk = start_vblk;
        e->block_count = block_count;
        e->flags = flags;
        e->owner_hint = owner_hint;
        e->delta = delta;
    }
    return 0;
}

int vtx_queue_data_write(struct vtx_fs *fs, __u64 vblk, __u32 blocks,
                         const void *data)
{
    __u8 *copy;
    size_t bytes = (size_t)blocks * VTXFS_BLOCK_SIZE;

    if (!fs || !vblk || !blocks || !data)
        return -EINVAL;
    if (fs->num_pending_data >= VTX_MAX_PENDING_DATA)
        return -ENOSPC;
    copy = (__u8 *)vtx_zalloc(bytes);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, data, bytes);
    fs->pending_data[fs->num_pending_data].vblk = vblk;
    fs->pending_data[fs->num_pending_data].block_count = blocks;
    fs->pending_data[fs->num_pending_data].data = copy;
    fs->num_pending_data++;
    return 0;
}

/* -------------------------------------------------------------------------
 * 2. B+Tree RW Callbacks Bound to struct vtx_fs
 * ------------------------------------------------------------------------- */

static int eng_read_node(void *ctx, __u64 vblk, const void **data,
                         size_t *data_bytes, int *checksum_verified)
{
    struct vtx_fs *fs = (struct vtx_fs *)ctx;
    struct vtx_cached_node *c = NULL;
    int ret = load_verified_node(fs, vblk, &c);
    if (ret)
        return ret;
    *data = c->data;
    *data_bytes = VTXFS_BLOCK_SIZE;
    *checksum_verified = 1;
    return 0;
}

static void eng_release_node(void *ctx, const void *data)
{
    (void)ctx;
    (void)data;
}

static int eng_cow_node(void *ctx, __u8 tree_type, __u8 level, __u64 oid,
                        __u64 old_vblk, __u64 *new_vblk, void **writable_data)
{
    struct vtx_fs *fs = (struct vtx_fs *)ctx;
    struct vtx_cached_node *old_c = NULL, *new_c = NULL;
    struct vtx_node_hdr *h;
    __u64 nv = 0, alloc_id;
    int ret;

    ret = load_verified_node(fs, old_vblk, &old_c);
    if (ret)
        return ret;
    h = (struct vtx_node_hdr *)old_c->data;
    if (vtx_cpu64(h->xid) == fs->current_xid) {
        old_c->dirty = 1;
        *new_vblk = old_vblk;
        *writable_data = old_c->data;
        return 0;
    }

    ret = vtx_vag_alloc_range(&fs->vag, 1, &nv);
    if (ret)
        return ret;
    /* During CoW, both old and new cache buffers must survive eviction.
     * The former code reloaded old_c after allocating new_c, but that
     * reload could immediately evict new_c (still clean). Under a long
     * stream this corrupted B+Tree nodes once the cache reached capacity.
     * Pin the verified old buffer until its contents have been copied. */
    old_c->pinned = 1;
    new_c = alloc_cache_slot(fs, nv);
    if (!new_c) {
        old_c->pinned = 0;
        vtx_vag_mark_range(&fs->vag, nv, 1, 0);
        return -ENOMEM;
    }
    memcpy(new_c->data, old_c->data, VTXFS_BLOCK_SIZE);
    old_c->pinned = 0;
    new_c->oid = oid;
    new_c->tree_type = tree_type;
    new_c->level = level;
    new_c->dirty = 1;
    h = (struct vtx_node_hdr *)new_c->data;
    h->xid = vtx_le64(fs->current_xid);
    h->self_vblk = vtx_le64(nv);
    memset(h->checksum, 0, sizeof(h->checksum));

    alloc_id = fs->next_oid++;
    ret = vtx_enqueue_delayed_ref(fs, 0, old_vblk, 1,
                                  VTX_REF_FLAG_META, tree_type, -1);
    if (ret)
        return ret;
    ret = vtx_enqueue_delayed_ref(fs, alloc_id, nv, 1,
                                  VTX_REF_FLAG_META, tree_type, 1);
    if (ret)
        return ret;

    *new_vblk = nv;
    *writable_data = new_c->data;
    return 0;
}

static int eng_alloc_node(void *ctx, __u8 tree_type, __u8 level,
                          __u64 *new_oid, __u64 *new_vblk,
                          void **writable_data)
{
    struct vtx_fs *fs = (struct vtx_fs *)ctx;
    struct vtx_cached_node *c;
    __u64 nv = 0, oid, alloc_id;
    int ret;

    ret = vtx_vag_alloc_range(&fs->vag, 1, &nv);
    if (ret)
        return ret;
    c = alloc_cache_slot(fs, nv);
    if (!c) {
        vtx_vag_mark_range(&fs->vag, nv, 1, 0);
        return -ENOMEM;
    }
    oid = fs->next_oid++;
    alloc_id = fs->next_oid++;
    memset(c->data, 0, VTXFS_BLOCK_SIZE);
    c->oid = oid;
    c->tree_type = tree_type;
    c->level = level;
    c->dirty = 1;

    ret = vtx_enqueue_delayed_ref(fs, alloc_id, nv, 1,
                                  VTX_REF_FLAG_META, tree_type, 1);
    if (ret)
        return ret;

    *new_oid = oid;
    *new_vblk = nv;
    *writable_data = c->data;
    return 0;
}

static int eng_free_node(void *ctx, __u8 tree_type, __u8 level,
                         __u64 oid, __u64 vblk)
{
    struct vtx_fs *fs = (struct vtx_fs *)ctx;
    struct vtx_cached_node *c = find_cached_node(fs, vblk);
    (void)level;
    (void)oid;
    if (c) {
        c->in_use = 0;
        c->dirty = 0;
    }
    return vtx_enqueue_delayed_ref(fs, 0, vblk, 1,
                                   VTX_REF_FLAG_META, tree_type, -1);
}

static struct vtx_rw_tree *select_omap_tree(struct vtx_fs *fs, __u64 view_id)
{
    if (view_id == 0)
        return &fs->omap_meta_tree;
    if (view_id == fs->active_subvol_id)
        return &fs->active_omap_view;
    return NULL;
}

static int eng_omap_resolve(void *ctx, __u64 view_id, __u64 child_oid,
                            __u64 *child_vblk)
{
    struct vtx_fs *fs = (struct vtx_fs *)ctx;
    struct vtx_rw_tree *omap = select_omap_tree(fs, view_id);
    struct vtx_key k;
    struct vtx_omap_val val;
    size_t bytes = sizeof(val);
    int ret;

    if (!omap)
        return -ENOENT;
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(child_oid, VTX_TYPE_OMAP_VAL));
    k.offset = 0;
    ret = vtx_rw_lookup(omap, &k, &val, &bytes);
    if (ret)
        return ret;
    if (bytes != sizeof(val) ||
        (vtx_cpu32(val.flags) & VTX_OMAP_VAL_TOMBSTONE))
        return -ENOENT;
    *child_vblk = vtx_cpu64(val.vblk);
    return 0;
}

static int eng_omap_set(void *ctx, __u64 view_id, __u64 child_oid,
                        __u64 child_vblk)
{
    struct vtx_fs *fs = (struct vtx_fs *)ctx;
    struct vtx_rw_tree *omap = select_omap_tree(fs, view_id);
    struct vtx_key k;
    struct vtx_omap_val val;

    if (!omap)
        return -EINVAL;
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(child_oid, VTX_TYPE_OMAP_VAL));
    k.offset = 0;
    val.vblk = vtx_le64(child_vblk);
    val.node_size = vtx_le32(VTXFS_BLOCK_SIZE);
    val.flags = vtx_le32(0);
    return vtx_rw_upsert(omap, &k, &val, sizeof(val));
}

static int eng_omap_del(void *ctx, __u64 view_id, __u64 child_oid)
{
    struct vtx_fs *fs = (struct vtx_fs *)ctx;
    struct vtx_rw_tree *omap = select_omap_tree(fs, view_id);
    struct vtx_key k;
    int ret;

    if (!omap)
        return -EINVAL;
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(child_oid, VTX_TYPE_OMAP_VAL));
    k.offset = 0;
    ret = vtx_rw_delete(omap, &k);
    return (ret == -ENOENT) ? 0 : ret;
}

static const struct vtx_rw_ops eng_rw_ops = {
    .read_node = eng_read_node,
    .release_node = eng_release_node,
    .cow_node = eng_cow_node,
    .alloc_node = eng_alloc_node,
    .free_node = eng_free_node,
    .omap_resolve = eng_omap_resolve,
    .omap_set = eng_omap_set,
    .omap_del = eng_omap_del
};

static void setup_tree_handle(struct vtx_fs *fs, struct vtx_rw_tree *t,
                              void *scratch, __u64 view_id,
                              __u64 root_oid, __u64 root_vblk,
                              __u64 owner_id, __u8 tree_type,
                              __u8 root_level, __u8 is_physical)
{
    memset(t, 0, sizeof(*t));
    t->ops = &eng_rw_ops;
    t->ctx = fs;
    t->scratch = scratch;
    t->scratch_bytes = VTXFS_BLOCK_SIZE * 3U;
    t->view_id = view_id;
    t->root_oid = root_oid;
    t->root_vblk = root_vblk;
    t->tx_xid = fs->current_xid;
    t->owner_id = owner_id;
    t->node_bytes = VTXFS_BLOCK_SIZE;
    t->tree_type = tree_type;
    t->root_level = root_level;
    t->is_physical = is_physical;
}

static void sync_tree_xids(struct vtx_fs *fs)
{
    fs->omap_meta_tree.tx_xid = fs->current_xid;
    fs->root_tree.tx_xid = fs->current_xid;
    fs->ref_tree.tx_xid = fs->current_xid;
    fs->reaper_tree.tx_xid = fs->current_xid;
    fs->sfq_tree.tx_xid = fs->current_xid;
    fs->uuid_tree.tx_xid = fs->current_xid;
    if (fs->active_subvol_id) {
        fs->active_omap_view.tx_xid = fs->current_xid;
        fs->active_fs_tree.tx_xid = fs->current_xid;
    }
}

/* -------------------------------------------------------------------------
 * 3. Delayed Reference Drain (REF +ALLOC_BY_VBLK + SFQ Enrollment)
 * ------------------------------------------------------------------------- */

static int apply_single_delayed_ref(struct vtx_fs *fs,
                                    const struct vtx_delayed_ref_entry *e)
{
    struct vtx_key k_ref, k_rev, k_sfq;
    struct vtx_ref_item ref_it;
    struct vtx_alloc_by_vblk_item rev_it;
    struct vtx_sfq_entry sfq_it;
    size_t bytes;
    __u64 alloc_id = e->allocation_id;
    int ret;

    k_rev.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(e->start_vblk, VTX_TYPE_ALLOC_BY_VBLK));
    k_rev.offset = 0;

    if (e->delta > 0) {
        if (!alloc_id) {
            bytes = sizeof(rev_it);
            if (vtx_rw_lookup(&fs->ref_tree, &k_rev, &rev_it, &bytes) == 0)
                alloc_id = vtx_cpu64(rev_it.allocation_id);
            else
                alloc_id = fs->next_oid++;
        }
        k_ref.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(alloc_id, VTX_TYPE_REF));
        k_ref.offset = 0;
        bytes = sizeof(ref_it);
        ret = vtx_rw_lookup(&fs->ref_tree, &k_ref, &ref_it, &bytes);
        if (ret == 0) {
            __u64 rc = vtx_cpu64(ref_it.refcount) + (__u64)e->delta;
            ref_it.refcount = vtx_le64(rc);
            return vtx_rw_replace(&fs->ref_tree, &k_ref, &ref_it,
                                  sizeof(ref_it));
        }
        if (ret != -ENOENT)
            return ret;

        memset(&ref_it, 0, sizeof(ref_it));
        ref_it.allocation_id = vtx_le64(alloc_id);
        ref_it.start_vblk = vtx_le64(e->start_vblk);
        ref_it.refcount = vtx_le64((__u64)e->delta);
        ref_it.birth_xid = vtx_le64(fs->current_xid);
        ref_it.block_count = vtx_le32(e->block_count);
        ref_it.flags = vtx_le32(e->flags);
        ref_it.owner_hint = vtx_le32(e->owner_hint);
        ret = vtx_rw_insert(&fs->ref_tree, &k_ref, &ref_it, sizeof(ref_it));
        if (ret)
            return ret;

        memset(&rev_it, 0, sizeof(rev_it));
        rev_it.start_vblk = vtx_le64(e->start_vblk);
        rev_it.allocation_id = vtx_le64(alloc_id);
        rev_it.block_count = vtx_le32(e->block_count);
        rev_it.flags = vtx_le32(e->flags);
        return vtx_rw_upsert(&fs->ref_tree, &k_rev, &rev_it, sizeof(rev_it));
    } else {
        __u64 drop = (__u64)(-e->delta);
        __u64 rc, start_vblk;
        __u32 count, flags;

        if (!alloc_id) {
            bytes = sizeof(rev_it);
            ret = vtx_rw_lookup(&fs->ref_tree, &k_rev, &rev_it, &bytes);
            if (ret)
                return ret;
            alloc_id = vtx_cpu64(rev_it.allocation_id);
        }
        k_ref.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(alloc_id, VTX_TYPE_REF));
        k_ref.offset = 0;
        bytes = sizeof(ref_it);
        ret = vtx_rw_lookup(&fs->ref_tree, &k_ref, &ref_it, &bytes);
        if (ret)
            return ret;
        rc = vtx_cpu64(ref_it.refcount);
        if (rc < drop)
            return -EUCLEAN;
        if (rc > drop) {
            ref_it.refcount = vtx_le64(rc - drop);
            return vtx_rw_replace(&fs->ref_tree, &k_ref, &ref_it,
                                  sizeof(ref_it));
        }

        /* Refcount reached 0: remove REF + ALLOC_BY_VBLK and enroll in SFQ. */
        start_vblk = vtx_cpu64(ref_it.start_vblk);
        count = vtx_cpu32(ref_it.block_count);
        flags = vtx_cpu32(ref_it.flags);
        k_rev.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(start_vblk, VTX_TYPE_ALLOC_BY_VBLK));
        k_rev.offset = 0;

        ret = vtx_rw_delete(&fs->ref_tree, &k_ref);
        if (ret)
            return ret;
        ret = vtx_rw_delete(&fs->ref_tree, &k_rev);
        if (ret && ret != -ENOENT)
            return ret;

        k_sfq.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(start_vblk, VTX_TYPE_SFQ));
        k_sfq.offset = vtx_le64(fs->current_xid);
        memset(&sfq_it, 0, sizeof(sfq_it));
        sfq_it.start_vblk = vtx_le64(start_vblk);
        sfq_it.block_count = vtx_le64(count);
        sfq_it.retired_xid = vtx_le64(fs->current_xid);
        sfq_it.flags = vtx_le32((flags & VTX_REF_FLAG_META)
                                ? VTX_SFQ_FLAG_META : 0U);
        return vtx_rw_upsert(&fs->sfq_tree, &k_sfq, &sfq_it, sizeof(sfq_it));
    }
}

static int drain_delayed_refs(struct vtx_fs *fs)
{
    __u32 guard = 0, head = 0;
    int ret;

    if (fs->draining_refs)
        return 0;

    fs->draining_refs = 1;
    while (head < fs->num_delayed_refs) {
        struct vtx_delayed_ref_entry cur = fs->delayed_refs[head++];
        ret = apply_single_delayed_ref(fs, &cur);
        if (ret) {
            __u32 rem = fs->num_delayed_refs - head;
            if (rem > 0)
                memmove(&fs->delayed_refs[0], &fs->delayed_refs[head],
                        rem * sizeof(struct vtx_delayed_ref_entry));
            fs->num_delayed_refs = rem;
            fs->draining_refs = 0;
            return ret;
        }
        if (++guard > 65536U) {
            __u32 rem = fs->num_delayed_refs - head;
            if (rem > 0)
                memmove(&fs->delayed_refs[0], &fs->delayed_refs[head],
                        rem * sizeof(struct vtx_delayed_ref_entry));
            fs->num_delayed_refs = rem;
            fs->draining_refs = 0;
            return -ELOOP;
        }
    }
    fs->num_delayed_refs = 0;
    memset(fs->delayed_refs, 0, sizeof(fs->delayed_refs));
    fs->draining_refs = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * 4. Subvolume Switching & Root Sync
 * ------------------------------------------------------------------------- */

static int flush_active_subvol_to_root_tree(struct vtx_fs *fs)
{
    struct vtx_key k;
    if (!fs->active_subvol_id)
        return 0;
    fs->active_subvol.root_node_oid = vtx_le64(fs->active_fs_tree.root_oid);
    fs->active_subvol.root_node_vblk = vtx_le64(fs->active_fs_tree.root_vblk);
    fs->active_subvol.omap_view_root_vblk =
        vtx_le64(fs->active_omap_view.root_vblk);
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(fs->active_subvol_id,
                                                VTX_TYPE_SUBVOL_ROOT));
    k.offset = 0;
    return vtx_rw_upsert(&fs->root_tree, &k, &fs->active_subvol,
                         sizeof(fs->active_subvol));
}

static int subvol_switch_common(struct vtx_fs *fs, __u64 subvol_id,
				int do_flush)
{
	struct vtx_key k;
	struct vtx_cached_node *fs_root = NULL, *view_root = NULL;
	const struct vtx_node_hdr *fh, *vh;
	size_t bytes = sizeof(fs->active_subvol);
	int ret;

	sync_tree_xids(fs);
	if (fs->active_subvol_id == subvol_id)
		return 0;
	if (do_flush && fs->active_subvol_id != 0 && !fs->read_only) {
		ret = flush_active_subvol_to_root_tree(fs);
		if (ret)
			return ret;
	}
	k.oid_and_type =
		vtx_le64(VTX_MAKE_OID_TYPE(subvol_id, VTX_TYPE_SUBVOL_ROOT));
	k.offset = 0;
	ret = vtx_rw_lookup(&fs->root_tree, &k, &fs->active_subvol, &bytes);
	if (ret)
		return ret;
	if (bytes != sizeof(fs->active_subvol))
		return -EUCLEAN;
	ret = load_verified_node(fs,
			vtx_cpu64(fs->active_subvol.omap_view_root_vblk),
			&view_root);
	if (ret)
		return ret;
	ret = load_verified_node(fs,
			vtx_cpu64(fs->active_subvol.root_node_vblk),
			&fs_root);
	if (ret)
		return ret;
	vh = (const struct vtx_node_hdr *)view_root->data;
	fh = (const struct vtx_node_hdr *)fs_root->data;
	fs->active_subvol_id = subvol_id;
	setup_tree_handle(fs, &fs->active_omap_view, fs->scratch_view,
			  0, vtx_cpu64(vh->oid),
			  vtx_cpu64(fs->active_subvol.omap_view_root_vblk),
			  subvol_id, VTX_TREE_OMAP_VIEW, vh->level, 1);
	setup_tree_handle(fs, &fs->active_fs_tree, fs->scratch_fs,
			  subvol_id, vtx_cpu64(fh->oid),
			  vtx_cpu64(fs->active_subvol.root_node_vblk),
			  subvol_id, VTX_TREE_FS, fh->level, 0);
	return 0;
}

int vtx_subvol_switch(struct vtx_fs *fs, __u64 subvol_id)
{
	int was_view;

	if (!fs || !fs->mounted || !subvol_id)
		return -EINVAL;
	was_view = fs->view_only;
	fs->view_only = 0;
	/* Ở chế độ view không có gì bị sửa nên không cần flush. */
	return subvol_switch_common(fs, subvol_id, !was_view);
}

int vtx_subvol_switch_view(struct vtx_fs *fs, __u64 subvol_id)
{
	int ret, do_flush;

	if (!fs || !fs->mounted || !subvol_id)
		return -EINVAL;
	do_flush = (!fs->view_only && !fs->read_only && fs->active_subvol_id != 0);
	ret = subvol_switch_common(fs, subvol_id, do_flush);
	if (ret == 0)
		fs->view_only = 1;
	return ret;
}

/* -------------------------------------------------------------------------
 * 5. Ordered Checkpoint Commit Protocol (Data -> Flush -> Meta -> Flush -> CPD)
 * ------------------------------------------------------------------------- */

static __u64 compute_free_blocks(const struct vtx_vag_state *vag)
{
    __u32 i;
    __u64 total = 0;
    for (i = 0; i < vag->total_vags; i++)
        total += vtx_cpu32(vag->descs[i].free_blocks);
    return total;
}

int vtx_sync_checkpoint(struct vtx_fs *fs)
{
    struct vtx_bdev *bdev;
    struct vtx_phys_ref vag_ref;
    __u64 xid, slot, prim_cpd_pblk, mirr_cpd_pblk, free_blks, cap_blks;
    __u32 i;
    int ret;

    if (!fs || !fs->mounted || !fs->bdev)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;

    bdev = fs->bdev;
    xid = fs->current_xid;
    sync_tree_xids(fs);

    ret = flush_active_subvol_to_root_tree(fs);
    if (ret)
        return ret;
    ret = drain_delayed_refs(fs);
    if (ret)
        return ret;

    int had_data = (fs->num_pending_data > 0);

    /* Step 1: Write all dirty data blocks. */
    for (i = 0; i < fs->num_pending_data; i++) {
        __u64 pblk = 0;
        if (bdev->fault_stage == VTX_FAULT_DATA_WRITE) {
            bdev->fault_triggered = 1;
            return -EIO;
        }
        ret = vtx_vag_vblk_to_pblk(&fs->vag, fs->pending_data[i].vblk, &pblk);
        if (ret)
            return ret;
        ret = bdev->ops->write_blocks(bdev, pblk,
                                      fs->pending_data[i].block_count,
                                      fs->pending_data[i].data);
        if (ret)
            return ret;
    }
    clear_pending_data(fs);

    /* Step 2: Flush data dependencies only if data was written. */
    if (had_data) {
        if (bdev->fault_stage == VTX_FAULT_DATA_FLUSH) {
            bdev->fault_triggered = 1;
            return -EIO;
        }
        ret = bdev->ops->flush(bdev);
        if (ret)
            return ret;
    }

    /* Step 3: Sign and persist all dirty CoW B+Tree metadata nodes. */
    for (struct vtx_cached_node *c = fs->lru_head; c != NULL; c = c->lru_next) {
        __u64 pblk = 0;
        if (!c->in_use || !c->dirty)
            continue;
        if (bdev->fault_stage == VTX_FAULT_META_WRITE) {
            bdev->fault_triggered = 1;
            return -EIO;
        }
        ret = vtx_node_checksum_compute(c->data, VTXFS_BLOCK_SIZE,
                                        fs->super_blk.hdr.metadata_uuid);
        if (ret)
            return ret;
        ret = vtx_vag_vblk_to_pblk(&fs->vag, c->vblk, &pblk);
        if (ret)
            return ret;
        ret = bdev->ops->write_blocks(bdev, pblk, 1, c->data);
        if (ret)
            return ret;
    }

    /* Step 4: Persist physical VAG directory & bitmaps for slot (xid % 8). */
    ret = vtx_vag_write_slot(bdev, &fs->vag, xid,
                             fs->super_blk.hdr.metadata_uuid, &vag_ref);
    if (ret)
        return ret;

    /* Step 5: Flush all metadata and VAG dependencies before CPD publish. */
    if (bdev->fault_stage == VTX_FAULT_META_FLUSH) {
        bdev->fault_triggered = 1;
        return -EIO;
    }
    ret = bdev->ops->flush(bdev);
    if (ret)
        return ret;

    /* Step 6: Build and sign CPD, then publish with FUA. */
    free_blks = compute_free_blocks(&fs->vag);
    cap_blks = (__u64)fs->vag.total_vags * VTXFS_VAG_BLOCKS;
    memcpy(fs->cp_blk.hdr.magic, VTXFS_CPD_MAGIC, VTXFS_MAGIC_LEN);
    fs->cp_blk.hdr.xid = vtx_le64(xid);
    fs->cp_blk.hdr.vag_dir_root = vag_ref;
    fs->cp_blk.hdr.omap_meta_tree_vblk = vtx_le64(fs->omap_meta_tree.root_vblk);
    fs->cp_blk.hdr.root_tree_vblk = vtx_le64(fs->root_tree.root_vblk);
    fs->cp_blk.hdr.ref_tree_vblk = vtx_le64(fs->ref_tree.root_vblk);
    fs->cp_blk.hdr.reaper_tree_vblk = vtx_le64(fs->reaper_tree.root_vblk);
    fs->cp_blk.hdr.sfq_tree_vblk = vtx_le64(fs->sfq_tree.root_vblk);
    fs->cp_blk.hdr.uuid_tree_vblk = vtx_le64(fs->uuid_tree.root_vblk);
    fs->cp_blk.hdr.next_oid = vtx_le64(fs->next_oid);
    fs->cp_blk.hdr.used_blocks = vtx_le64(cap_blks - free_blks);
    fs->cp_blk.hdr.root_tree_level = vtx_le32(fs->root_tree.root_level);
    memcpy(fs->cp_blk.hdr.fsid_uuid, fs->super_blk.hdr.fsid_uuid,
           VTXFS_UUID_SIZE);
    fs->cp_blk.hdr.prev_xid = vtx_le64(fs->committed_xid);
    fs->cp_blk.hdr.super_generation = fs->super_blk.hdr.super_generation;
    fs->cp_blk.hdr.commit_time_ns = vtx_le64(vtx_time_now_ns());
    fs->cp_blk.hdr.mirror_tree_vblk = vtx_le64(0);
    fs->cp_blk.hdr.evac_tree_vblk = vtx_le64(0);
    fs->cp_blk.hdr.scrub_tree_vblk = vtx_le64(0);
    fs->cp_blk.hdr.keybag_tree_vblk = vtx_le64(0);
    fs->cp_blk.hdr.chain_tree_vblk = vtx_le64(0);
    fs->cp_blk.hdr.total_blocks = fs->super_blk.hdr.total_blocks;
    fs->cp_blk.hdr.free_blocks = vtx_le64(free_blks);
    fs->cp_blk.hdr.tree_levels[VTX_TREE_ROOT] = fs->root_tree.root_level;
    fs->cp_blk.hdr.tree_levels[VTX_TREE_OMAP_META] =
        fs->omap_meta_tree.root_level;
    fs->cp_blk.hdr.tree_levels[VTX_TREE_REF] = fs->ref_tree.root_level;
    fs->cp_blk.hdr.tree_levels[VTX_TREE_REAPER] = fs->reaper_tree.root_level;
    fs->cp_blk.hdr.tree_levels[VTX_TREE_SFQ] = fs->sfq_tree.root_level;
    fs->cp_blk.hdr.tree_levels[VTX_TREE_UUID] = fs->uuid_tree.root_level;
    vtx_cpd_checksum_compute(&fs->cp_blk);

    slot = VTXFS_CPD_SLOT_FOR_XID(xid);
    prim_cpd_pblk = VTXFS_CPD_RING_PBLK + slot;
    mirr_cpd_pblk = VTXFS_BOOT_MIRROR0_PBLK + prim_cpd_pblk;

    if (bdev->fault_stage == VTX_FAULT_CPD_PRIMARY) {
        bdev->fault_triggered = 1;
        return -EIO;
    }
    ret = bdev->ops->write_fua(bdev, prim_cpd_pblk, 1, &fs->cp_blk);
    if (ret)
        return ret;

    for (struct vtx_cached_node *c = fs->lru_head; c != NULL; c = c->lru_next)
        c->dirty = 0;

    fs->committed_xid = xid;
    fs->current_xid = xid + 1ULL;
    sync_tree_xids(fs);

    /* Primary CPD is already durable via write_fua above; mirror is safely written
     * with write_blocks without redundant second synchronous FUA stall. */
    if (bdev->fault_stage == VTX_FAULT_CPD_MIRROR) {
        bdev->fault_triggered = 1;
        return 0;
    }
    (void)bdev->ops->write_blocks(bdev, mirr_cpd_pblk, 1, &fs->cp_blk);
    return 0;
}

/* -------------------------------------------------------------------------
 * 6. Mkfs, Bootstrap Discovery, Mount & Crash Recovery
 * ------------------------------------------------------------------------- */

static int init_system_root_node(struct vtx_fs *fs, __u64 oid,
                                 __u8 tree_type, __u64 owner_id,
                                 __u64 *vblk_out)
{
    struct vtx_cached_node *c;
    struct vtx_node_hdr *h;
    __u64 nv = 0, alloc_id;
    int ret;

    ret = vtx_vag_alloc_range(&fs->vag, 1, &nv);
    if (ret)
        return ret;
    c = alloc_cache_slot(fs, nv);
    if (!c)
        return -ENOMEM;
    ret = vtx_leaf_init(c->data, VTXFS_BLOCK_SIZE, oid, fs->current_xid,
                        nv, tree_type,
                        VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT);
    if (ret)
        return ret;
    h = (struct vtx_node_hdr *)c->data;
    h->owner_id = vtx_le64(owner_id);
    c->oid = oid;
    c->tree_type = tree_type;
    c->level = 0;
    c->dirty = 1;

    alloc_id = fs->next_oid++;
    ret = vtx_enqueue_delayed_ref(fs, alloc_id, nv, 1,
                                  VTX_REF_FLAG_META, tree_type, 1);
    if (ret)
        return ret;
    *vblk_out = nv;
    return 0;
}

int vtx_mkfs(struct vtx_bdev *bdev, const char *label,
             const __u8 fsid_uuid[VTXFS_UUID_SIZE])
{
    struct vtx_fs *fs;
    __u64 avail_blocks, total_vags;
    __u64 omap_vblk = 0, root_vblk = 0, ref_vblk = 0;
    __u64 reaper_vblk = 0, sfq_vblk = 0, uuid_vblk = 0;
    __u64 sub_view_oid, sub_view_vblk = 0, sub_fs_oid, sub_fs_vblk = 0;
    __u8 def_uuid[16] = {
        0x56, 0x54, 0x58, 0x46, 0x53, 0x32, 0x31, 0x00,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    };
    const __u8 *use_uuid = fsid_uuid ? fsid_uuid : def_uuid;
    struct vtx_key k;
    struct vtx_inode_item root_ino;
    struct vtx_inode_ref root_ref;
    struct vtx_uuid_map_item uuid_it;
    __u32 i;
    int ret;

    if (!bdev || bdev->total_blocks < VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS)
        return -EINVAL;
    avail_blocks = bdev->total_blocks - VTXFS_FIRST_VAG_PBLK;
    total_vags = avail_blocks / VTXFS_VAG_BLOCKS;
    if (total_vags > VTX_MAX_SUPPORTED_VAGS)
        total_vags = VTX_MAX_SUPPORTED_VAGS;

    fs = vtx_fs_alloc();
    if (!fs)
        return -ENOMEM;
    fs->bdev = bdev;
    fs->mounted = 1;
    fs->read_only = 0;
    fs->committed_xid = 0;
    fs->current_xid = 1;
    fs->next_oid = 1024;

    /* 1. Format Superblock & Device Table. */
    memcpy(fs->super_blk.hdr.magic, VTXFS_MAGIC_BYTES, VTXFS_MAGIC_LEN);
    fs->super_blk.hdr.major = vtx_le16(VTXFS_VERSION_MAJOR);
    fs->super_blk.hdr.minor = vtx_le16(VTXFS_VERSION_MINOR);
    fs->super_blk.hdr.block_size = vtx_le32(VTXFS_BLOCK_SIZE);
    memcpy(fs->super_blk.hdr.fsid_uuid, use_uuid, VTXFS_UUID_SIZE);
    for (i = 0; i < VTXFS_UUID_SIZE; i++) {
        fs->super_blk.hdr.device_uuid[i] = (__u8)(use_uuid[i] ^ 0x11U);
        fs->super_blk.hdr.metadata_uuid[i] = (__u8)(use_uuid[i] ^ 0x3AU);
        fs->super_blk.hdr.dir_hash_seed[i] = (__u8)(use_uuid[i] ^ 0x5CU);
    }
    fs->super_blk.hdr.super_generation = vtx_le64(1);
    fs->super_blk.hdr.compat_flags = vtx_le64(VTXFS_FEAT_COMPAT_UTF8_LABEL);
    fs->super_blk.hdr.ro_compat_flags =
        vtx_le64(VTXFS_FEAT_RO_COMPAT_FREE_SPACE_CACHE);
    fs->super_blk.hdr.incompat_flags = vtx_le64(VTXFS_P0_REQUIRED_INCOMPAT);
    fs->super_blk.hdr.device_count = vtx_le32(1);
    fs->super_blk.hdr.self_dev_id = vtx_le32(0);
    fs->super_blk.hdr.total_blocks = vtx_le64(bdev->total_blocks);
    fs->super_blk.hdr.node_size = vtx_le32(VTXFS_BLOCK_SIZE);
    fs->super_blk.hdr.sector_size = vtx_le32(VTXFS_BLOCK_SIZE);
    fs->super_blk.hdr.meta_csum_type = vtx_le16(VTX_CSUM_BLAKE3_128);
    fs->super_blk.hdr.data_csum_type = vtx_le16(VTX_CSUM_CRC32C);
    fs->super_blk.hdr.dir_hash_type = vtx_le16(VTX_DIR_HASH_SIPHASH24);
    fs->super_blk.hdr.state = vtx_le16(0);
    fs->super_blk.hdr.mkfs_time_ns = vtx_le64(vtx_time_now_ns());
    fs->super_blk.hdr.first_alloc_pblk = vtx_le64(VTXFS_FIRST_VAG_PBLK);
    if (label) {
        size_t llen = strlen(label);
        if (llen >= VTXFS_LABEL_SIZE)
            llen = VTXFS_LABEL_SIZE - 1U;
        memcpy(fs->super_blk.hdr.label, label, llen);
    }
    vtx_super_checksum_compute(&fs->super_blk);

    fs->dev_tbl.hdr.magic = vtx_le32(VTXFS_DEV_TABLE_MAGIC);
    fs->dev_tbl.hdr.entry_count = vtx_le32(1);
    fs->dev_tbl.hdr.generation = vtx_le64(1);
    memcpy(fs->dev_tbl.hdr.fsid_uuid, use_uuid, VTXFS_UUID_SIZE);
    memcpy(fs->dev_tbl.devs[0].dev_uuid, fs->super_blk.hdr.device_uuid,
           VTXFS_UUID_SIZE);
    fs->dev_tbl.devs[0].dev_id = vtx_le16(0);
    fs->dev_tbl.devs[0].tier_id = vtx_le16(VTX_TIER_NVME_HOT);
    fs->dev_tbl.devs[0].state_flags = vtx_le32(VTX_DEV_STATE_ONLINE);
    fs->dev_tbl.devs[0].sector_size = vtx_le32(VTXFS_BLOCK_SIZE);
    fs->dev_tbl.devs[0].generation = vtx_le32(1);
    fs->dev_tbl.devs[0].total_blocks = vtx_le64(bdev->total_blocks);
    fs->dev_tbl.devs[0].total_vag_slots = vtx_le32((__u32)total_vags);
    fs->dev_tbl.devs[0].used_vag_slots = vtx_le32((__u32)total_vags);
    vtx_dev_table_checksum_compute(&fs->dev_tbl);

    ret = vtx_vag_init_empty(&fs->vag, (__u32)total_vags, 1,
                             fs->super_blk.hdr.metadata_uuid);
    if (ret)
        goto out;

    /* 2. Initialize the 6 system trees. */
    ret = init_system_root_node(fs, VTXFS_OID_OMAP_META, VTX_TREE_OMAP_META,
                                0, &omap_vblk);
    if (ret)
        goto out;
    ret = init_system_root_node(fs, VTXFS_OID_ROOT_TREE, VTX_TREE_ROOT,
                                0, &root_vblk);
    if (ret)
        goto out;
    ret = init_system_root_node(fs, VTXFS_OID_REF_TREE, VTX_TREE_REF,
                                0, &ref_vblk);
    if (ret)
        goto out;
    ret = init_system_root_node(fs, VTXFS_OID_REAPER_TREE, VTX_TREE_REAPER,
                                0, &reaper_vblk);
    if (ret)
        goto out;
    ret = init_system_root_node(fs, VTXFS_OID_SFQ_TREE, VTX_TREE_SFQ,
                                0, &sfq_vblk);
    if (ret)
        goto out;
    ret = init_system_root_node(fs, VTXFS_OID_UUID_TREE, VTX_TREE_UUID,
                                0, &uuid_vblk);
    if (ret)
        goto out;

    setup_tree_handle(fs, &fs->omap_meta_tree, fs->scratch_omap, 0,
                      VTXFS_OID_OMAP_META, omap_vblk, 0,
                      VTX_TREE_OMAP_META, 0, 1);
    setup_tree_handle(fs, &fs->root_tree, fs->scratch_sys, 0,
                      VTXFS_OID_ROOT_TREE, root_vblk, 0,
                      VTX_TREE_ROOT, 0, 0);
    setup_tree_handle(fs, &fs->ref_tree, fs->scratch_sys, 0,
                      VTXFS_OID_REF_TREE, ref_vblk, 0,
                      VTX_TREE_REF, 0, 0);
    setup_tree_handle(fs, &fs->reaper_tree, fs->scratch_sys, 0,
                      VTXFS_OID_REAPER_TREE, reaper_vblk, 0,
                      VTX_TREE_REAPER, 0, 0);
    setup_tree_handle(fs, &fs->sfq_tree, fs->scratch_sys, 0,
                      VTXFS_OID_SFQ_TREE, sfq_vblk, 0,
                      VTX_TREE_SFQ, 0, 0);
    setup_tree_handle(fs, &fs->uuid_tree, fs->scratch_sys, 0,
                      VTXFS_OID_UUID_TREE, uuid_vblk, 0,
                      VTX_TREE_UUID, 0, 0);

    ret = eng_omap_set(fs, 0, VTXFS_OID_ROOT_TREE, root_vblk);
    if (ret)
        goto out;
    ret = eng_omap_set(fs, 0, VTXFS_OID_REF_TREE, ref_vblk);
    if (ret)
        goto out;
    ret = eng_omap_set(fs, 0, VTXFS_OID_REAPER_TREE, reaper_vblk);
    if (ret)
        goto out;
    ret = eng_omap_set(fs, 0, VTXFS_OID_SFQ_TREE, sfq_vblk);
    if (ret)
        goto out;
    ret = eng_omap_set(fs, 0, VTXFS_OID_UUID_TREE, uuid_vblk);
    if (ret)
        goto out;

    /* 3. Create default user subvolume (subvol_id = 256) with root dir (ino = 2). */
    sub_view_oid = fs->next_oid++;
    sub_fs_oid = fs->next_oid++;
    ret = init_system_root_node(fs, sub_view_oid, VTX_TREE_OMAP_VIEW,
                                VTXFS_OID_FIRST_USER_SUBVOL, &sub_view_vblk);
    if (ret)
        goto out;
    ret = init_system_root_node(fs, sub_fs_oid, VTX_TREE_FS,
                                VTXFS_OID_FIRST_USER_SUBVOL, &sub_fs_vblk);
    if (ret)
        goto out;

    fs->active_subvol_id = VTXFS_OID_FIRST_USER_SUBVOL;
    setup_tree_handle(fs, &fs->active_omap_view, fs->scratch_view, 0,
                      sub_view_oid, sub_view_vblk,
                      VTXFS_OID_FIRST_USER_SUBVOL, VTX_TREE_OMAP_VIEW, 0, 1);
    setup_tree_handle(fs, &fs->active_fs_tree, fs->scratch_fs,
                      VTXFS_OID_FIRST_USER_SUBVOL, sub_fs_oid, sub_fs_vblk,
                      VTXFS_OID_FIRST_USER_SUBVOL, VTX_TREE_FS, 0, 0);

    ret = eng_omap_set(fs, VTXFS_OID_FIRST_USER_SUBVOL, sub_fs_oid, sub_fs_vblk);
    if (ret)
        goto out;

    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.parent_ino = vtx_le64(VTXFS_INO_ROOT_DIR);
    root_ino.update_xid = vtx_le64(1);
    root_ino.mode = vtx_le32(0040755U);
    root_ino.nlink = vtx_le32(1);
    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_INO_ROOT_DIR, VTX_TYPE_INODE));
    k.offset = 0;
    ret = vtx_rw_insert(&fs->active_fs_tree, &k, &root_ino, sizeof(root_ino));
    if (ret)
        goto out;

    memset(&root_ref, 0, sizeof(root_ref));
    root_ref.dir_seq = vtx_le64(0);
    root_ref.name_len = vtx_le16(0);
    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_INO_ROOT_DIR, VTX_TYPE_INODE_REF));
    k.offset = vtx_le64(VTXFS_INO_ROOT_DIR);
    ret = vtx_rw_insert(&fs->active_fs_tree, &k, &root_ref, sizeof(root_ref));
    if (ret)
        goto out;

    memset(&fs->active_subvol, 0, sizeof(fs->active_subvol));
    fs->active_subvol.subvol_id = vtx_le64(VTXFS_OID_FIRST_USER_SUBVOL);
    fs->active_subvol.parent_subvol_id = vtx_le64(0);
    fs->active_subvol.root_node_oid = vtx_le64(fs->active_fs_tree.root_oid);
    fs->active_subvol.root_node_vblk = vtx_le64(fs->active_fs_tree.root_vblk);
    fs->active_subvol.omap_view_root_vblk =
        vtx_le64(fs->active_omap_view.root_vblk);
    fs->active_subvol.create_xid = vtx_le64(1);
    fs->active_subvol.change_xid = vtx_le64(1);
    fs->active_subvol.num_inodes = vtx_le64(1);
    for (i = 0; i < VTXFS_UUID_SIZE; i++)
        fs->active_subvol.uuid[i] = (__u8)(use_uuid[i] ^ 0xA5U);

    memset(&uuid_it, 0, sizeof(uuid_it));
    memcpy(uuid_it.uuid, fs->active_subvol.uuid, VTXFS_UUID_SIZE);
    uuid_it.subvol_id = vtx_le64(VTXFS_OID_FIRST_USER_SUBVOL);
    uuid_it.kind = vtx_le32(1);
    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_OID_FIRST_USER_SUBVOL,
                                   VTX_TYPE_UUID_MAP));
    k.offset = vtx_le64(VTXFS_OID_FIRST_USER_SUBVOL);
    ret = vtx_rw_insert(&fs->uuid_tree, &k, &uuid_it, sizeof(uuid_it));
    if (ret)
        goto out;

    /* 4. Persist Device Table & Superblock ring (primary + 64 MiB mirror). */
    ret = bdev->ops->write_blocks(bdev, VTXFS_DEV_TABLE_PBLK,
                                  VTXFS_DEV_TABLE_BLOCKS, &fs->dev_tbl);
    if (ret)
        goto out;
    ret = bdev->ops->write_blocks(bdev,
                                  VTXFS_BOOT_MIRROR0_PBLK + VTXFS_DEV_TABLE_PBLK,
                                  VTXFS_DEV_TABLE_BLOCKS, &fs->dev_tbl);
    if (ret)
        goto out;
    ret = bdev->ops->write_blocks(bdev, VTXFS_SUPER_RING_PBLK, 1,
                                  &fs->super_blk);
    if (ret)
        goto out;
    ret = bdev->ops->write_blocks(bdev, VTXFS_SUPER_RING_PBLK + 1ULL, 1,
                                  &fs->super_blk);
    if (ret)
        goto out;
    ret = bdev->ops->write_blocks(bdev,
                                  VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK,
                                  1, &fs->super_blk);
    if (ret)
        goto out;
    ret = bdev->ops->write_blocks(bdev,
                                  VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK + 1ULL,
                                  1, &fs->super_blk);
    if (ret)
        goto out;

    fs->cp_blk.hdr.cp_flags = vtx_le32(VTXFS_CP_FLAG_CLEAN_UNMOUNT);
    ret = vtx_sync_checkpoint(fs);

out:
    vtx_fs_free(fs);
    return ret;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(vtx_mkfs);
#endif

static inline int vtx_is_zero_buf(const void *buf, size_t len)
{
    const __u8 *p = (const __u8 *)buf;
    size_t i;
    for (i = 0; i < len; i++) {
        if (p[i] != 0)
            return 0;
    }
    return 1;
}

static int super_valid(const struct vtx_super_block *sb, __u64 bdev_blocks)
{
    __u64 total_blocks;
    __u64 incompat = vtx_cpu64(sb->hdr.incompat_flags);
    __u32 dev_count = vtx_cpu32(sb->hdr.device_count);

    if (memcmp(sb->hdr.magic, VTXFS_MAGIC_BYTES, VTXFS_MAGIC_LEN) != 0 ||
        !vtx_super_checksum_verify(sb) ||
        vtx_cpu16(sb->hdr.major) != VTXFS_VERSION_MAJOR ||
        vtx_cpu16(sb->hdr.minor) != VTXFS_VERSION_MINOR ||
        vtx_cpu32(sb->hdr.block_size) != VTXFS_BLOCK_SIZE ||
        vtx_cpu32(sb->hdr.node_size) != VTXFS_BLOCK_SIZE ||
        vtx_cpu32(sb->hdr.sector_size) != VTXFS_BLOCK_SIZE ||
        ((incompat & VTXFS_FEAT_INCOMPAT_MULTIDEV) ?
            (dev_count < 1 || dev_count > VTXFS_MAX_DEVICES) :
            (dev_count != 1)) ||
        vtx_cpu32(sb->hdr.self_dev_id) >= VTXFS_MAX_DEVICES ||
        vtx_cpu16(sb->hdr.meta_csum_type) != VTX_CSUM_BLAKE3_128 ||
        vtx_cpu16(sb->hdr.data_csum_type) != VTX_CSUM_CRC32C ||
        vtx_cpu16(sb->hdr.dir_hash_type) != VTX_DIR_HASH_SIPHASH24 ||
        vtx_cpu64(sb->hdr.first_alloc_pblk) < VTXFS_FIRST_VAG_PBLK)
        return 0;

    /* ABI v2.1 contract: compression feature-gate and reserved bytes zero check */
    if (!(incompat & VTXFS_FEAT_INCOMPAT_COMPRESS)) {
        if (sb->default_compression != 0)
            return 0;
    } else {
        if (sb->default_compression != VTX_COMPR_NONE &&
            sb->default_compression != VTX_COMPR_ZSTD &&
            sb->default_compression != VTX_COMPR_LZ4 &&
            sb->default_compression != VTX_COMPR_ZLIB)
            return 0;
    }
    if (!vtx_is_zero_buf(sb->reserved, sizeof(sb->reserved)))
        return 0;

    total_blocks = vtx_cpu64(sb->hdr.total_blocks);
    if (incompat & VTXFS_FEAT_INCOMPAT_MULTIDEV)
        return total_blocks >= VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS;
    return total_blocks >= VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS &&
           total_blocks <= bdev_blocks;
}

static int verify_cpd_roots(struct vtx_fs *fs,
                            const struct vtx_checkpoint_desc *cpd)
{
    const __u64 vblks[6] = {
        vtx_cpu64(cpd->hdr.root_tree_vblk),
        vtx_cpu64(cpd->hdr.omap_meta_tree_vblk),
        vtx_cpu64(cpd->hdr.ref_tree_vblk),
        vtx_cpu64(cpd->hdr.reaper_tree_vblk),
        vtx_cpu64(cpd->hdr.sfq_tree_vblk),
        vtx_cpu64(cpd->hdr.uuid_tree_vblk)
    };
    const __u8 types[6] = {
        VTX_TREE_ROOT, VTX_TREE_OMAP_META, VTX_TREE_REF,
        VTX_TREE_REAPER, VTX_TREE_SFQ, VTX_TREE_UUID
    };
    __u64 xid = vtx_cpu64(cpd->hdr.xid);
    int i, ret;

    for (i = 0; i < 6; i++) {
        struct vtx_cached_node *c = NULL;
        const struct vtx_node_hdr *h;
        ret = load_verified_node(fs, vblks[i], &c);
        if (ret)
            return ret;
        h = (const struct vtx_node_hdr *)c->data;
        if (h->tree_type != types[i] ||
            h->level != cpd->hdr.tree_levels[types[i]] ||
            !(vtx_cpu16(h->flags) & VTX_NODE_FLAG_ROOT) ||
            vtx_cpu64(h->xid) == 0 || vtx_cpu64(h->xid) > xid)
            return -EUCLEAN;
    }
    return 0;
}

static int init_tree_from_root_vblk(struct vtx_fs *fs, struct vtx_rw_tree *t,
                                    void *scratch, __u64 view_id,
                                    __u64 root_vblk, __u64 owner_id,
                                    __u8 tree_type, __u8 is_physical)
{
    struct vtx_cached_node *c = NULL;
    const struct vtx_node_hdr *h;
    int ret = load_verified_node(fs, root_vblk, &c);
    if (ret)
        return ret;
    h = (const struct vtx_node_hdr *)c->data;
    setup_tree_handle(fs, t, scratch, view_id, vtx_cpu64(h->oid),
                      root_vblk, owner_id, tree_type, h->level, is_physical);
    return 0;
}

int vtx_mount(struct vtx_fs *fs, struct vtx_bdev *bdev, int force_ro)
{
    struct vtx_super_block *cand_sb = NULL;
    struct vtx_checkpoint_desc *cand_cpd = NULL;
    __u64 best_sb_gen = 0, best_cpd_xid = 0;
    int found_sb = 0, found_cpd = 0, policy, m, s, ret = 0;

    if (!fs || !bdev || bdev->total_blocks < VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS)
        return -EINVAL;

    clear_pending_data(fs);
    clear_node_cache(fs);
    vtx_vag_destroy(&fs->vag);
    memset(fs, 0, sizeof(*fs));
    fs->bdev = bdev;
    fs->cache_capacity = VTX_CACHE_INITIAL_NODES;
    fs->cache_max_capacity = VTX_CACHE_MAX_NODES;
    fs->cache_count = 0;

    cand_sb = (struct vtx_super_block *)vtx_zalloc(sizeof(*cand_sb) * VTXFS_SUPER_RING_SLOTS);
    cand_cpd = (struct vtx_checkpoint_desc *)vtx_zalloc(sizeof(*cand_cpd) * VTXFS_CPD_RING_SLOTS);
    if (!cand_sb || !cand_cpd) {
        ret = -ENOMEM;
        goto fail;
    }

    /* 1. Scan primary and 64 MiB mirror superblock rings (vector batch read). */
    for (m = 0; m < 2; m++) {
        __u64 base = (m == 0 ? 0ULL : VTXFS_BOOT_MIRROR0_PBLK) +
                     VTXFS_SUPER_RING_PBLK;
        int bulk_ok = (bdev->ops->read_blocks(bdev, base, VTXFS_SUPER_RING_SLOTS, cand_sb) == 0);
        for (s = 0; s < (int)VTXFS_SUPER_RING_SLOTS; s++) {
            struct vtx_super_block *cur_sb = &cand_sb[s];
            if (!bulk_ok && bdev->ops->read_blocks(bdev, base + (__u64)s, 1, cur_sb) != 0)
                continue;
            if (!super_valid(cur_sb, bdev->total_blocks))
                continue;
            if (!found_sb ||
                vtx_cpu64(cur_sb->hdr.super_generation) > best_sb_gen) {
                fs->super_blk = *cur_sb;
                best_sb_gen = vtx_cpu64(cur_sb->hdr.super_generation);
                found_sb = 1;
            }
        }
    }
    if (!found_sb) {
        ret = -EUCLEAN;
        goto fail;
    }

    policy = vtx_feature_policy(vtx_cpu64(fs->super_blk.hdr.compat_flags),
                                vtx_cpu64(fs->super_blk.hdr.ro_compat_flags),
                                vtx_cpu64(fs->super_blk.hdr.incompat_flags));
    if (policy == VTX_MOUNT_REFUSE) {
        ret = -EOPNOTSUPP;
        goto fail;
    }
    if (vtx_cpu16(fs->super_blk.hdr.state) & VTXFS_STATE_ERROR) {
        ret = -EUCLEAN;
        goto fail;
    }
    fs->read_only = (force_ro || policy == VTX_MOUNT_RO) ? 1 : 0;

    /* 2. Load and verify Device Table (primary or mirror). */
    if (bdev->ops->read_blocks(bdev, VTXFS_DEV_TABLE_PBLK,
                               VTXFS_DEV_TABLE_BLOCKS, &fs->dev_tbl) != 0 ||
        vtx_cpu32(fs->dev_tbl.hdr.magic) != VTXFS_DEV_TABLE_MAGIC ||
        !vtx_dev_table_checksum_verify(&fs->dev_tbl) ||
        memcmp(fs->dev_tbl.hdr.fsid_uuid, fs->super_blk.hdr.fsid_uuid,
               VTXFS_UUID_SIZE) != 0) {
        if (bdev->ops->read_blocks(bdev,
                                   VTXFS_BOOT_MIRROR0_PBLK + VTXFS_DEV_TABLE_PBLK,
                                   VTXFS_DEV_TABLE_BLOCKS, &fs->dev_tbl) != 0 ||
            vtx_cpu32(fs->dev_tbl.hdr.magic) != VTXFS_DEV_TABLE_MAGIC ||
            !vtx_dev_table_checksum_verify(&fs->dev_tbl) ||
            memcmp(fs->dev_tbl.hdr.fsid_uuid, fs->super_blk.hdr.fsid_uuid,
                   VTXFS_UUID_SIZE) != 0) {
            ret = -EUCLEAN;
            goto fail;
        }
    }

    /* 3. Scan primary and mirror CPD rings for valid candidates (vector batch read). */
    struct cpd_cand {
        struct vtx_checkpoint_desc cpd;
        __u64 xid;
    } *cands = (struct cpd_cand *)vtx_zalloc(sizeof(struct cpd_cand) * 16);
    int n_cands = 0;

    if (!cands) {
        ret = -ENOMEM;
        goto fail;
    }

    for (m = 0; m < 2; m++) {
        __u64 base = (m == 0 ? 0ULL : VTXFS_BOOT_MIRROR0_PBLK) +
                     VTXFS_CPD_RING_PBLK;
        int bulk_ok = (bdev->ops->read_blocks(bdev, base, VTXFS_CPD_RING_SLOTS, cand_cpd) == 0);
        for (s = 0; s < (int)VTXFS_CPD_RING_SLOTS; s++) {
            struct vtx_checkpoint_desc *cur_cpd = &cand_cpd[s];
            __u64 xid;
            if (!bulk_ok && bdev->ops->read_blocks(bdev, base + (__u64)s, 1, cur_cpd) != 0)
                continue;
            if (memcmp(cur_cpd->hdr.magic, VTXFS_CPD_MAGIC,
                       VTXFS_MAGIC_LEN) != 0 ||
                !vtx_cpd_checksum_verify(cur_cpd))
                continue;
            xid = vtx_cpu64(cur_cpd->hdr.xid);
            if (!xid || VTXFS_CPD_SLOT_FOR_XID(xid) != (__u32)s ||
                memcmp(cur_cpd->hdr.fsid_uuid, fs->super_blk.hdr.fsid_uuid,
                       VTXFS_UUID_SIZE) != 0 ||
                vtx_cpu64(cur_cpd->hdr.super_generation) != best_sb_gen ||
                (vtx_cpu32(cur_cpd->hdr.cp_flags) & ~VTXFS_CP_FLAGS_P0_KNOWN))
                continue;
            if (n_cands < 16) {
                cands[n_cands].cpd = *cur_cpd;
                cands[n_cands].xid = xid;
                n_cands++;
            }
        }
    }

    /* Verify highest-XID valid candidate first (O(1) VAG load instead of N reloads). */
    while (n_cands > 0) {
        int best_idx = -1;
        __u32 i;
        for (i = 0; i < (__u32)n_cands; i++) {
            if (cands[i].xid != 0 && (best_idx < 0 || cands[i].xid > cands[best_idx].xid))
                best_idx = (int)i;
        }
        if (best_idx < 0)
            break;

        clear_node_cache(fs);
        if (vtx_vag_load_from_ref(bdev, &fs->vag,
                                  &cands[best_idx].cpd.hdr.vag_dir_root,
                                  vtx_cpu64(fs->super_blk.hdr.total_blocks),
                                  fs->super_blk.hdr.metadata_uuid) == 0 &&
            verify_cpd_roots(fs, &cands[best_idx].cpd) == 0) {
            fs->cp_blk = cands[best_idx].cpd;
            best_cpd_xid = cands[best_idx].xid;
            found_cpd = 1;
            break;
        }
        /* Candidate failed verification (e.g. crash during commit): discard and try next highest */
        cands[best_idx].xid = 0;
    }

    vtx_free(cands);
    cands = NULL;

    if (!found_cpd) {
        ret = -EUCLEAN;
        goto fail;
    }

    fs->committed_xid = best_cpd_xid;
    fs->current_xid = best_cpd_xid + 1ULL;
    fs->next_oid = vtx_cpu64(fs->cp_blk.hdr.next_oid);
    fs->mounted = 1;

    ret = init_tree_from_root_vblk(fs, &fs->omap_meta_tree, fs->scratch_omap,
                                   0, vtx_cpu64(fs->cp_blk.hdr.omap_meta_tree_vblk),
                                   0, VTX_TREE_OMAP_META, 1);
    if (ret)
        goto fail;
    ret = init_tree_from_root_vblk(fs, &fs->root_tree, fs->scratch_sys,
                                   0, vtx_cpu64(fs->cp_blk.hdr.root_tree_vblk),
                                   0, VTX_TREE_ROOT, 0);
    if (ret)
        goto fail;
    ret = init_tree_from_root_vblk(fs, &fs->ref_tree, fs->scratch_sys,
                                   0, vtx_cpu64(fs->cp_blk.hdr.ref_tree_vblk),
                                   0, VTX_TREE_REF, 0);
    if (ret)
        goto fail;
    ret = init_tree_from_root_vblk(fs, &fs->reaper_tree, fs->scratch_sys,
                                   0, vtx_cpu64(fs->cp_blk.hdr.reaper_tree_vblk),
                                   0, VTX_TREE_REAPER, 0);
    if (ret)
        goto fail;
    ret = init_tree_from_root_vblk(fs, &fs->sfq_tree, fs->scratch_sys,
                                   0, vtx_cpu64(fs->cp_blk.hdr.sfq_tree_vblk),
                                   0, VTX_TREE_SFQ, 0);
    if (ret)
        goto fail;
    ret = init_tree_from_root_vblk(fs, &fs->uuid_tree, fs->scratch_sys,
                                   0, vtx_cpu64(fs->cp_blk.hdr.uuid_tree_vblk),
                                   0, VTX_TREE_UUID, 0);
    if (ret)
        goto fail;

    ret = vtx_subvol_switch(fs, VTXFS_OID_FIRST_USER_SUBVOL);
    if (ret)
        goto fail;

    if (!fs->read_only)
        fs->cp_blk.hdr.cp_flags &= ~vtx_le32(VTXFS_CP_FLAG_CLEAN_UNMOUNT);

    vtx_free(cand_sb);
    vtx_free(cand_cpd);
    return 0;

fail:
    vtx_free(cand_sb);
    vtx_free(cand_cpd);
    clear_node_cache(fs);
    vtx_vag_destroy(&fs->vag);
    fs->mounted = 0;
    return ret;
}

int vtx_unmount(struct vtx_fs *fs)
{
    int ret = 0;
    if (!fs || !fs->mounted)
        return -EINVAL;
    if (!fs->read_only && !fs->view_only) {
        fs->cp_blk.hdr.cp_flags |= vtx_le32(VTXFS_CP_FLAG_CLEAN_UNMOUNT);
        ret = vtx_sync_checkpoint(fs);
    }
    clear_pending_data(fs);
    clear_node_cache(fs);
    vtx_vag_destroy(&fs->vag);
    fs->mounted = 0;
    return ret;
}

/* -------------------------------------------------------------------------
 * 7. SFQ Promotion, Free-Space Bitmap Rebuild & Crash-Resumable Reaper
 * ------------------------------------------------------------------------- */

int vtx_sfq_promote(struct vtx_fs *fs, __u64 safe_xid, __u32 *promoted_out)
{
    struct vtx_key cur_k = {0}, found_k;
    struct vtx_sfq_entry sfq;
    size_t bytes;
    __u32 promoted = 0;
    int first = 1, ret;

    if (!fs || !fs->mounted)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;
    sync_tree_xids(fs);

    for (;;) {
        bytes = sizeof(sfq);
        ret = vtx_rw_next(&fs->sfq_tree, &cur_k, first, &found_k, &sfq, &bytes);
        if (ret == -ENOENT)
            break;
        if (ret)
            return ret;
        first = 0;
        cur_k = found_k;

        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) != VTX_TYPE_SFQ)
            continue;
        if (vtx_cpu64(sfq.retired_xid) <= safe_xid) {
            __u64 sv = vtx_cpu64(sfq.start_vblk);
            __u32 cnt = (__u32)vtx_cpu64(sfq.block_count);
            struct vtx_key k_rev;
            struct vtx_alloc_by_vblk_item rev;
            size_t rbytes = sizeof(rev);

            /* Verify not referenced in authoritative REF tree before reuse. */
            k_rev.oid_and_type =
                vtx_le64(VTX_MAKE_OID_TYPE(sv, VTX_TYPE_ALLOC_BY_VBLK));
            k_rev.offset = 0;
            if (vtx_rw_lookup(&fs->ref_tree, &k_rev, &rev, &rbytes) == 0)
                return -EUCLEAN;

            ret = vtx_rw_delete(&fs->sfq_tree, &found_k);
            if (ret)
                return ret;
            ret = vtx_vag_mark_range(&fs->vag, sv, cnt, 0);
            if (ret)
                return ret;
            promoted++;
        }
    }
    fs->cp_blk.hdr.reuse_safe_xid = vtx_le64(safe_xid);
    if (promoted_out)
        *promoted_out = promoted;
    return 0;
}

int vtx_vag_rebuild_bitmaps(struct vtx_fs *fs)
{
    struct vtx_key cur_k = {0}, found_k;
    __u8 val_buf[128];
    size_t bytes;
    __u32 i;
    int first, ret;

    if (!fs || !fs->mounted)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;
    sync_tree_xids(fs);
    ret = drain_delayed_refs(fs);
    if (ret)
        return ret;

    /* Clear all VAG bitmaps (preserving VBLK 0 reservation in VAG 0). */
    for (i = 0; i < fs->vag.total_vags; i++) {
        memset(fs->vag.bitmaps[i], 0, VTXFS_BLOCK_SIZE);
        if (i == 0) {
            fs->vag.bitmaps[0][0] |= 1U;
            fs->vag.descs[0].free_blocks = vtx_le32(VTXFS_VAG_BLOCKS - 1U);
        } else {
            fs->vag.descs[i].free_blocks = vtx_le32(VTXFS_VAG_BLOCKS);
        }
        fs->vag.descs[i].bitmap_crc32c =
            vtx_le32(vtx_crc32c(fs->vag.bitmaps[i], VTXFS_BLOCK_SIZE));
    }
    /* Bitmaps were rewritten directly: drop the RAM-only free-run index. */
    vtx_vag_summary_invalidate(&fs->vag);

    /* Mark all authoritative REF allocations. */
    memset(&cur_k, 0, sizeof(cur_k));
    first = 1;
    for (;;) {
        bytes = sizeof(val_buf);
        ret = vtx_rw_next(&fs->ref_tree, &cur_k, first, &found_k,
                          val_buf, &bytes);
        if (ret == -ENOENT)
            break;
        if (ret)
            return ret;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) == VTX_TYPE_REF) {
            const struct vtx_ref_item *r =
                (const struct vtx_ref_item *)val_buf;
            if (bytes != sizeof(*r))
                return -EUCLEAN;
            ret = vtx_vag_mark_range(&fs->vag, vtx_cpu64(r->start_vblk),
                                     vtx_cpu32(r->block_count), 1);
            if (ret)
                return ret;
        }
    }

    /* Mark all pending-reuse SFQ allocations. */
    memset(&cur_k, 0, sizeof(cur_k));
    first = 1;
    for (;;) {
        bytes = sizeof(val_buf);
        ret = vtx_rw_next(&fs->sfq_tree, &cur_k, first, &found_k,
                          val_buf, &bytes);
        if (ret == -ENOENT)
            break;
        if (ret)
            return ret;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) == VTX_TYPE_SFQ) {
            const struct vtx_sfq_entry *s =
                (const struct vtx_sfq_entry *)val_buf;
            if (bytes != sizeof(*s))
                return -EUCLEAN;
            ret = vtx_vag_mark_range(&fs->vag, vtx_cpu64(s->start_vblk),
                                     (__u32)vtx_cpu64(s->block_count), 1);
            if (ret)
                return ret;
        }
    }
    return 0;
}

static int reap_subvol_step(struct vtx_fs *fs, struct vtx_reaper_job *job,
                            __u32 max_steps, __u32 *steps_io)
{
    __u64 subvol_id = vtx_cpu64(job->object_id);
    struct vtx_key k_sub, found_k;
    struct vtx_subvol_root_item sub;
    struct vtx_rw_tree view_t, fs_t;
    size_t bytes = sizeof(sub);
    __u8 *item_buf = NULL;
    int ret;

    if (fs->active_subvol_id == subvol_id)
        fs->active_subvol_id = 0;

    k_sub.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(subvol_id, VTX_TYPE_SUBVOL_ROOT));
    k_sub.offset = 0;
    ret = vtx_rw_lookup(&fs->root_tree, &k_sub, &sub, &bytes);
    if (ret == -ENOENT) {
        job->phase = vtx_le32(VTX_REAPER_DONE);
        job->completed_xid = vtx_le64(fs->current_xid);
        return 0;
    }
    if (ret)
        return ret;

    ret = init_tree_from_root_vblk(fs, &view_t, fs->scratch_view, 0,
                                   vtx_cpu64(sub.omap_view_root_vblk),
                                   subvol_id, VTX_TREE_OMAP_VIEW, 1);
    if (ret)
        return ret;
    ret = init_tree_from_root_vblk(fs, &fs_t, fs->scratch_fs, subvol_id,
                                   vtx_cpu64(sub.root_node_vblk),
                                   subvol_id, VTX_TREE_FS, 0);
    if (ret)
        return ret;

    /* Temporarily bind active_subvol_id so eng_omap_* resolves view_t. */
    fs->active_subvol_id = subvol_id;
    fs->active_omap_view = view_t;
    fs->active_fs_tree = fs_t;

    item_buf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!item_buf) {
        fs->active_subvol_id = 0;
        return -ENOMEM;
    }

    while (*steps_io < max_steps) {
        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &job->cursor, 1,
                          &found_k, item_buf, &bytes);
        if (ret == -ENOENT)
            break;
        if (ret) {
            vtx_free(item_buf);
            fs->active_subvol_id = 0;
            return ret;
        }
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) ==
            VTX_TYPE_EXTENT) {
            const struct vtx_extent_item *ext =
                (const struct vtx_extent_item *)item_buf;
            if (!(vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)) {
                ret = vtx_enqueue_delayed_ref(
                    fs, vtx_cpu64(ext->allocation_id),
                    vtx_cpu64(ext->vblk),
                    vtx_cpu32(ext->physical_blocks),
                    VTX_REF_FLAG_DATA, (__u32)subvol_id, -1);
                if (ret) {
                    vtx_free(item_buf);
                    fs->active_subvol_id = 0;
                    return ret;
                }
            }
        }
        ret = vtx_rw_delete(&fs->active_fs_tree, &found_k);
        if (ret) {
            vtx_free(item_buf);
            fs->active_subvol_id = 0;
            return ret;
        }
        job->cursor = found_k;
        (*steps_io)++;

        /* Persist intermediate subvolume root pointers after each step. */
        sub.root_node_oid = vtx_le64(fs->active_fs_tree.root_oid);
        sub.root_node_vblk = vtx_le64(fs->active_fs_tree.root_vblk);
        sub.omap_view_root_vblk = vtx_le64(fs->active_omap_view.root_vblk);
    }

    vtx_free(item_buf);

    /* Check if any items remain in the subvolume FS tree. */
    bytes = 0;
    ret = vtx_rw_next(&fs->active_fs_tree, &job->cursor, 1,
                      &found_k, NULL, &bytes);
    if (ret != -ENOENT) {
        /* More work remains; save updated subvolume roots and return. */
        ret = vtx_rw_replace(&fs->root_tree, &k_sub, &sub, sizeof(sub));
        fs->active_subvol_id = 0;
        return ret;
    }

    /* Subvolume FS tree is now an empty root leaf; drain OMAP_VIEW and free roots. */
    {
        struct vtx_key zero_k = {0};
        for (;;) {
            bytes = 0;
            ret = vtx_rw_next(&fs->active_omap_view, &zero_k, 1,
                              &found_k, NULL, &bytes);
            if (ret == -ENOENT)
                break;
            if (ret && ret != -ENOBUFS) {
                fs->active_subvol_id = 0;
                return ret;
            }
            ret = vtx_rw_delete(&fs->active_omap_view, &found_k);
            if (ret) {
                fs->active_subvol_id = 0;
                return ret;
            }
        }
    }
    ret = eng_free_node(fs, VTX_TREE_FS, 0,
                        fs->active_fs_tree.root_oid,
                        fs->active_fs_tree.root_vblk);
    if (ret) {
        fs->active_subvol_id = 0;
        return ret;
    }
    ret = eng_free_node(fs, VTX_TREE_OMAP_VIEW, 0,
                        fs->active_omap_view.root_oid,
                        fs->active_omap_view.root_vblk);
    if (ret) {
        fs->active_subvol_id = 0;
        return ret;
    }
    fs->active_subvol_id = 0;

    /* Remove UUID map and subvolume root item. */
    {
        struct vtx_key k_uuid;
        k_uuid.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(subvol_id, VTX_TYPE_UUID_MAP));
        k_uuid.offset = vtx_le64(subvol_id);
        (void)vtx_rw_delete(&fs->uuid_tree, &k_uuid);
    }
    ret = vtx_rw_delete(&fs->root_tree, &k_sub);
    if (ret && ret != -ENOENT)
        return ret;

    job->phase = vtx_le32(VTX_REAPER_DONE);
    job->completed_xid = vtx_le64(fs->current_xid);
    return vtx_subvol_switch(fs, VTXFS_OID_FIRST_USER_SUBVOL);
}

static int reap_orphan_inode_step(struct vtx_fs *fs, struct vtx_reaper_job *job,
                                  __u32 max_steps, __u32 *steps_io)
{
    __u64 subvol_id = vtx_cpu64(job->owning_root_id);
    __u64 ino = vtx_cpu64(job->object_id);
    struct vtx_key start_k, found_k;
    __u8 *item_buf;
    size_t bytes;
    int ret;

    ret = vtx_subvol_switch(fs, subvol_id);
    if (ret)
        return ret;
    item_buf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!item_buf)
        return -ENOMEM;

    start_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, 0));
    start_k.offset = 0;

    while (*steps_io < max_steps) {
        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &start_k, 1,
                          &found_k, item_buf, &bytes);
        if (ret == -ENOENT ||
            VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != ino) {
            /* Remove orphan marker if present and mark job done. */
            struct vtx_key k_orph;
            k_orph.oid_and_type =
                vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_INO_ORPHAN_DIR,
                                           VTX_TYPE_DIR_INDEX));
            k_orph.offset = vtx_le64(ino);
            (void)vtx_rw_delete(&fs->active_fs_tree, &k_orph);
            job->phase = vtx_le32(VTX_REAPER_DONE);
            job->completed_xid = vtx_le64(fs->current_xid);
            vtx_free(item_buf);
            return 0;
        }
        if (ret) {
            vtx_free(item_buf);
            return ret;
        }
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) ==
            VTX_TYPE_EXTENT) {
            const struct vtx_extent_item *ext =
                (const struct vtx_extent_item *)item_buf;
            if (!(vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)) {
                ret = vtx_enqueue_delayed_ref(
                    fs, vtx_cpu64(ext->allocation_id),
                    vtx_cpu64(ext->vblk),
                    vtx_cpu32(ext->physical_blocks),
                    VTX_REF_FLAG_DATA, (__u32)subvol_id, -1);
                if (ret) {
                    vtx_free(item_buf);
                    return ret;
                }
            }
        }
        ret = vtx_rw_delete(&fs->active_fs_tree, &found_k);
        if (ret) {
            vtx_free(item_buf);
            return ret;
        }
        job->cursor = found_k;
        (*steps_io)++;
    }
    vtx_free(item_buf);
    return 0;
}

int vtx_reaper_run_steps(struct vtx_fs *fs, __u32 max_steps,
                         __u32 *steps_done_out, int *all_done_out)
{
    struct vtx_key cur_k = {0}, found_k;
    struct vtx_reaper_job job;
    size_t bytes;
    __u32 steps = 0, pending_jobs = 0;
    int first = 1, ret;

    if (!fs || !fs->mounted || !max_steps)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;
    sync_tree_xids(fs);

    for (;;) {
        __u32 phase, op;
        bytes = sizeof(job);
        ret = vtx_rw_next(&fs->reaper_tree, &cur_k, first,
                          &found_k, &job, &bytes);
        if (ret == -ENOENT)
            break;
        if (ret)
            return ret;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) !=
            VTX_TYPE_REAPER_JOB || bytes != sizeof(job))
            continue;

        phase = vtx_cpu32(job.phase);
        if (phase != VTX_REAPER_QUEUED && phase != VTX_REAPER_TRAVERSING)
            continue;
        if (steps >= max_steps) {
            pending_jobs++;
            continue;
        }

        job.phase = vtx_le32(VTX_REAPER_TRAVERSING);
        op = vtx_cpu32(job.operation);
        if (op == VTX_REAPER_OP_DROP_SUBVOL)
            ret = reap_subvol_step(fs, &job, max_steps, &steps);
        else if (op == VTX_REAPER_OP_DROP_ORPHAN_INODE)
            ret = reap_orphan_inode_step(fs, &job, max_steps, &steps);
        else if (op == VTX_REAPER_OP_DEC_REF_RANGE) {
            ret = vtx_enqueue_delayed_ref(fs, vtx_cpu64(job.object_id),
                                          vtx_cpu64(job.cursor.offset), 1,
                                          VTX_REF_FLAG_DATA, 0, -1);
            job.phase = vtx_le32(VTX_REAPER_DONE);
            job.completed_xid = vtx_le64(fs->current_xid);
            steps++;
        } else {
            return -EUCLEAN;
        }
        if (ret)
            return ret;
        ret = vtx_rw_replace(&fs->reaper_tree, &found_k, &job, sizeof(job));
        if (ret)
            return ret;
        if (vtx_cpu32(job.phase) != VTX_REAPER_DONE)
            pending_jobs++;
    }

    if (pending_jobs == 0)
        fs->cp_blk.hdr.cp_flags &= ~vtx_le32(VTXFS_CP_FLAG_REAPER_PENDING);
    else
        fs->cp_blk.hdr.cp_flags |= vtx_le32(VTXFS_CP_FLAG_REAPER_PENDING);

    if (steps_done_out)
        *steps_done_out = steps;
    if (all_done_out)
        *all_done_out = (pending_jobs == 0);
    return 0;
}

int vtx_reaper_run_all(struct vtx_fs *fs)
{
    int all_done = 0, ret;
    __u32 steps = 0, guard = 0;

    while (!all_done) {
        ret = vtx_reaper_run_steps(fs, 64, &steps, &all_done);
        if (ret)
            return ret;
        if (++guard > 4096U)
            return -ELOOP;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * 8. Feature Migration APIs (ABI v2.1 Policy Enforcement)
 * ------------------------------------------------------------------------- */

static int check_subvol_for_compressed_extents(struct vtx_fs *fs, __u64 subvol_id)
{
    struct vtx_key cur = { 0, 0 }, found;
    __u8 *ibuf;
    size_t bytes;
    int first = 1, ret = 0;

    ret = vtx_subvol_switch_view(fs, subvol_id);
    if (ret)
        return ret;

    ibuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!ibuf)
        return -ENOMEM;

    for (;;) {
        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur, first, &found, ibuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur = found;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) == VTX_TYPE_EXTENT) {
            const struct vtx_extent_item *ext = (const struct vtx_extent_item *)ibuf;
            if (vtx_cpu32(ext->flags) & VTX_EXTENT_COMPRESSED) {
                ret = 1; /* Found a compressed extent */
                break;
            }
        }
    }
    vtx_free(ibuf);
    return ret;
}

static int vtx_fs_has_compressed_extents(struct vtx_fs *fs)
{
    struct vtx_key cur = { 0, 0 }, found;
    struct vtx_subvol_root_item item;
    size_t bytes;
    int first = 1, ret = 0;
    __u64 saved_subvol = fs->active_subvol_id ? fs->active_subvol_id : VTXFS_OID_FIRST_USER_SUBVOL;
    int saved_view = fs->view_only;

    for (;;) {
        bytes = sizeof(item);
        ret = vtx_rw_next(&fs->root_tree, &cur, first, &found, &item, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur = found;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) == VTX_TYPE_SUBVOL_ROOT &&
            bytes == sizeof(item)) {
            __u64 sub_id = VTX_KEY_GET_OID(vtx_cpu64(found.oid_and_type));
            int has_comp = check_subvol_for_compressed_extents(fs, sub_id);
            if (has_comp > 0) {
                ret = 1;
                break;
            } else if (has_comp < 0) {
                ret = has_comp;
                break;
            }
        }
    }

    if (saved_view)
        (void)vtx_subvol_switch_view(fs, saved_subvol);
    else
        (void)vtx_subvol_switch(fs, saved_subvol);

    return ret;
}

int vtx_migrate_enable_compression(struct vtx_fs *fs, __u8 algo)
{
    int ret;
    __u64 incompat;

    if (!fs || !fs->mounted || !fs->bdev)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;
    if (algo != VTX_COMPR_ZSTD && algo != VTX_COMPR_LZ4 && algo != VTX_COMPR_ZLIB)
        return -EINVAL;

    incompat = vtx_cpu64(fs->super_blk.hdr.incompat_flags);
    incompat |= VTXFS_FEAT_INCOMPAT_COMPRESS;
    fs->super_blk.hdr.incompat_flags = vtx_le64(incompat);
    fs->super_blk.default_compression = algo;
    memset(fs->super_blk.reserved, 0, sizeof(fs->super_blk.reserved));
    fs->super_blk.hdr.super_generation = vtx_le64(
        vtx_cpu64(fs->super_blk.hdr.super_generation) + 1ULL);
    vtx_super_checksum_compute(&fs->super_blk);

    ret = fs->bdev->ops->write_blocks(fs->bdev, VTXFS_SUPER_RING_PBLK,
                                      1, &fs->super_blk);
    if (ret == 0)
        ret = fs->bdev->ops->write_blocks(fs->bdev, VTXFS_SUPER_RING_PBLK + 1ULL,
                                          1, &fs->super_blk);
    if (ret == 0)
        ret = fs->bdev->ops->write_blocks(fs->bdev,
                                          VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK,
                                          1, &fs->super_blk);
    if (ret == 0)
        ret = fs->bdev->ops->write_blocks(fs->bdev,
                                          VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK + 1ULL,
                                          1, &fs->super_blk);
    if (ret == 0)
        ret = vtx_sync_checkpoint(fs);

    return ret;
}

int vtx_migrate_disable_compression(struct vtx_fs *fs)
{
    int ret;
    __u64 incompat;

    if (!fs || !fs->mounted || !fs->bdev)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;

    incompat = vtx_cpu64(fs->super_blk.hdr.incompat_flags);
    if (!(incompat & VTXFS_FEAT_INCOMPAT_COMPRESS)) {
        fs->super_blk.default_compression = 0;
        memset(fs->super_blk.reserved, 0, sizeof(fs->super_blk.reserved));
        return 0;
    }

    ret = vtx_fs_has_compressed_extents(fs);
    if (ret > 0)
        return -EBUSY; /* Compressed extents remain, reject disable */
    if (ret < 0)
        return ret;

    incompat &= ~VTXFS_FEAT_INCOMPAT_COMPRESS;
    fs->super_blk.hdr.incompat_flags = vtx_le64(incompat);
    fs->super_blk.default_compression = 0;
    memset(fs->super_blk.reserved, 0, sizeof(fs->super_blk.reserved));
    fs->super_blk.hdr.super_generation = vtx_le64(
        vtx_cpu64(fs->super_blk.hdr.super_generation) + 1ULL);
    vtx_super_checksum_compute(&fs->super_blk);

    ret = fs->bdev->ops->write_blocks(fs->bdev, VTXFS_SUPER_RING_PBLK,
                                      1, &fs->super_blk);
    if (ret == 0)
        ret = fs->bdev->ops->write_blocks(fs->bdev, VTXFS_SUPER_RING_PBLK + 1ULL,
                                          1, &fs->super_blk);
    if (ret == 0)
        ret = fs->bdev->ops->write_blocks(fs->bdev,
                                          VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK,
                                          1, &fs->super_blk);
    if (ret == 0)
        ret = fs->bdev->ops->write_blocks(fs->bdev,
                                          VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK + 1ULL,
                                          1, &fs->super_blk);
    if (ret == 0)
        ret = vtx_sync_checkpoint(fs);

    return ret;
}

#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(vtx_migrate_enable_compression);
EXPORT_SYMBOL_GPL(vtx_migrate_disable_compression);
#endif

