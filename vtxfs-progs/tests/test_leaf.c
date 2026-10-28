/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Deterministic model-based tests for portable VTXFS slotted-leaf helpers.
 * No block device, no CoW tree traversal, no checkpoint durability here.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/vtxfs_leaf.h"

enum { NODE_SIZE = 4096, MODEL_MAX = 512, ITERATIONS = 20000 };
struct record {
    struct vtx_key key;
    unsigned short len;
    unsigned char bytes[128];
};
static struct record model[MODEL_MAX];
static unsigned int count;
static uint64_t state = 0x8d29fac70365e9b1ULL;

static uint64_t rnd(void)
{
    uint64_t x = state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    state = x;
    return x * 2685821657736338717ULL;
}

static struct vtx_key makekey(uint64_t oid, uint8_t type, uint64_t off)
{
    struct vtx_key k;
    k.oid_and_type = vtx_cpu64(VTX_MAKE_OID_TYPE(oid, type));
    k.offset = vtx_cpu64(off);
    return k;
}

static unsigned int posof(struct vtx_key key, int *exists)
{
    unsigned int i;
    for (i = 0; i < count; ++i)
        if (vtx_key_cmp(&model[i].key, &key) >= 0)
            break;
    *exists = i < count && vtx_key_cmp(&model[i].key, &key) == 0;
    return i;
}

static void verify(const unsigned char *node)
{
    const struct vtx_node_hdr *h = (const struct vtx_node_hdr *)(const void *)node;
    unsigned int i;
    assert(vtx_node_slots_valid(h, NODE_SIZE));
    assert(vtx_cpu16(h->nitems) == count);
    assert(vtx_cpu16(h->dead_bytes8) == 0);
    for (i = 0; i < count; ++i) {
        const struct vtx_slot *s = &h->entries[i].slot;
        unsigned int off = vtx_cpu16(s->val_off8) * 8U;
        unsigned int len = vtx_cpu16(s->val_len);
        unsigned int foundslot = 999U;
        assert(vtx_key_cmp(&h->entries[i].key, &model[i].key) == 0);
        assert(len == model[i].len);
        assert(!memcmp(node + off, model[i].bytes, len));
        assert(vtx_leaf_find(node, NODE_SIZE, &model[i].key, &foundslot) == 0);
        assert(foundslot == i);
    }
}

static void stress(unsigned char *node, unsigned char *scratch)
{
    unsigned int iter;
    assert(vtx_leaf_init(node, NODE_SIZE, 4, 7, 16, VTX_TREE_FS,
                         VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT) == 0);
    verify(node);
    for (iter = 0; iter < ITERATIONS; ++iter) {
        struct vtx_key key = makekey(1U + (rnd() % 150), VTX_TYPE_DIR_ITEM,
                                     rnd() % 1000);
        unsigned char data[128];
        unsigned int n = (unsigned int)(rnd() % 98U);
        unsigned int op = (unsigned int)(rnd() % 4U);
        unsigned int i;
        int exists, result;
        unsigned int idx = posof(key, &exists);
        unsigned char snapshot[NODE_SIZE];

        for (i = 0; i < n; ++i)
            data[i] = (unsigned char)rnd();
        memcpy(snapshot, node, NODE_SIZE);

        if (op == 0) {
            result = vtx_leaf_insert(node, NODE_SIZE, &key, data, (uint16_t)n,
                                     scratch, NODE_SIZE);
            if (exists)
                assert(result == -EEXIST);
            else if (result == 0) {
                assert(count < MODEL_MAX);
                memmove(&model[idx+1], &model[idx],
                        (count-idx) * sizeof(model[0]));
                model[idx].key = key;
                model[idx].len = (unsigned short)n;
                memcpy(model[idx].bytes, data, n);
                ++count;
            } else {
                assert(result == -ENOSPC);
            }
        } else if (op == 1) {
            result = vtx_leaf_delete(node, NODE_SIZE, &key, scratch, NODE_SIZE);
            if (!exists)
                assert(result == -ENOENT);
            else {
                assert(result == 0);
                memmove(&model[idx], &model[idx+1],
                        (count-idx-1) * sizeof(model[0]));
                --count;
            }
        } else if (op == 2) {
            result = vtx_leaf_replace(node, NODE_SIZE, &key, data, (uint16_t)n,
                                      scratch, NODE_SIZE);
            if (!exists)
                assert(result == -ENOENT);
            else if (result == 0) {
                model[idx].len = (unsigned short)n;
                memcpy(model[idx].bytes, data, n);
            } else {
                assert(result == -ENOSPC);
            }
        } else {
            result = vtx_leaf_compact(node, NODE_SIZE, scratch, NODE_SIZE);
            assert(result == 0);
        }

        if (result != 0)
            assert(!memcmp(snapshot, node, NODE_SIZE));
        if (!(iter % 97U))
            verify(node);
    }
    verify(node);
    printf("PASS: %u randomized leaf operations, sorted keys and bytes intact\n",
           (unsigned int)ITERATIONS);
}

static void negative(unsigned char *node, unsigned char *scratch)
{
    struct vtx_key key = makekey(20, VTX_TYPE_EXTENT, 0);
    unsigned char data[2048];
    unsigned char saved[NODE_SIZE], invalid[NODE_SIZE];
    struct vtx_node_hdr *h = (struct vtx_node_hdr *)(void *)invalid;
    memset(data, 0x5a, sizeof(data));

    assert(vtx_leaf_init(node, NODE_SIZE, 20, 9, 2, VTX_TREE_FS,
                         VTX_NODE_FLAG_LEAF) == 0);
    assert(vtx_leaf_insert(node, NODE_SIZE, &key, data, sizeof(data),
                           scratch, NODE_SIZE) == 0);
    memcpy(saved, node, NODE_SIZE);
    assert(vtx_leaf_insert(node, NODE_SIZE, &key, data, 1,
                           scratch, NODE_SIZE) == -EEXIST);
    assert(vtx_leaf_insert(node, NODE_SIZE, &key, data, 1,
                           node, NODE_SIZE) == -EINVAL);
    assert(vtx_leaf_delete(node, NODE_SIZE, &key, scratch, 5) == -EINVAL);
    assert(!memcmp(saved, node, NODE_SIZE));

    memcpy(invalid, node, NODE_SIZE);
    h->entries[0].slot.val_off8 = vtx_cpu16(0);
    assert(!vtx_node_slots_valid(h, NODE_SIZE));
    assert(vtx_leaf_delete(invalid, NODE_SIZE, &key,
                           scratch, NODE_SIZE) == -EINVAL);
    assert(!memcmp(saved, node, NODE_SIZE));

    assert(vtx_leaf_delete(node, NODE_SIZE, &key, scratch, NODE_SIZE) == 0);
    assert(((struct vtx_node_hdr *)(void *)node)->nitems == 0);

    /* Exhaustion: adding a second 2048-byte value cannot fit a 4KiB node. */
    assert(vtx_leaf_insert(node, NODE_SIZE, &key, data, sizeof(data),
                           scratch, NODE_SIZE) == 0);
    saved[0] = 0;
    memcpy(saved, node, NODE_SIZE);
    key = makekey(21, VTX_TYPE_EXTENT, 0);
    assert(vtx_leaf_insert(node, NODE_SIZE, &key, data, sizeof(data),
                           scratch, NODE_SIZE) == -ENOSPC);
    assert(!memcmp(saved, node, NODE_SIZE));
    puts("PASS: corrupted slot, alias, undersized scratch, and ENOSPC rollback");
}

int main(void)
{
    unsigned char *node = malloc(NODE_SIZE);
    unsigned char *scratch = malloc(NODE_SIZE);
    assert(node && scratch);
    stress(node, scratch);
    negative(node, scratch);
    free(scratch);
    free(node);
    return 0;
}
