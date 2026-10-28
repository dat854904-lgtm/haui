// SPDX-License-Identifier: GPL-2.0-only
/*
 * vtxfs_ctree_test.c — KUnit coverage for the kernel-side extent-buffer,
 * io_tree, portable slotted-leaf core and checksum primitives.
 */
#include <kunit/test.h>
#include <linux/slab.h>
#include "ctree.h"
#include "vtxfs_csum.h"
#include "vtxfs_leaf.h"
#include "vtxfs_trace.h"

static void test_crc32c_vector(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	KUNIT_EXPECT_EQ(t, vtx_crc32c("123456789", 9), 0xE3069283U);
}

static void test_blake3_empty(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	u8 out[32];
	static const u8 exp[32] = {
		0xaf,0x13,0x49,0xb9,0xf5,0xf9,0xa1,0xa6,
		0xa0,0x40,0x4d,0xea,0x36,0xdc,0xc9,0x49,
		0x9b,0xcb,0x25,0xc9,0xad,0xc1,0x12,0xb7,
		0xcc,0x9a,0x93,0xca,0xe4,0x1f,0x32,0x62 };

	vtx_blake3_256("", 0, out);
	KUNIT_EXPECT_MEMEQ(t, out, exp, 32);
}

static void test_leaf_insert_find_delete(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	void *node = kunit_kzalloc(t, 4096, GFP_KERNEL);
	void *scratch = kunit_kzalloc(t, 4096, GFP_KERNEL);
	struct vtx_key k;
	u32 slot = 0;

	KUNIT_ASSERT_NOT_NULL(t, node);
	KUNIT_ASSERT_NOT_NULL(t, scratch);
	KUNIT_ASSERT_EQ(t, vtx_leaf_init(node, 4096, 4, 1, 16,
			VTX_TREE_FS, VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT),
			0);
	KUNIT_EXPECT_EQ(t, vtxfs_mk_key(&k, 7, VTX_TYPE_INODE, 0), 0);
	KUNIT_EXPECT_EQ(t, vtx_leaf_insert(node, 4096, &k, "ABC", 3,
					   scratch, 4096), 0);
	KUNIT_EXPECT_EQ(t, vtx_leaf_find(node, 4096, &k, &slot), 0);
	KUNIT_EXPECT_EQ(t, slot, 0U);
	KUNIT_EXPECT_EQ(t, vtx_leaf_delete(node, 4096, &k, scratch, 4096), 0);
	KUNIT_EXPECT_EQ(t, vtx_leaf_find(node, 4096, &k, &slot), 1);
}

static void test_eb_refcount(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = kunit_kzalloc(t, sizeof(*fs), GFP_KERNEL);
	struct vtxfs_extent_buffer *eb;

	KUNIT_ASSERT_NOT_NULL(t, fs);
	fs->node_size = 4096;
	xa_init(&fs->eb_cache);
	spin_lock_init(&fs->eb_lock);

	eb = vtxfs_eb_alloc(fs, 42, 4096);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(t, eb);
	KUNIT_EXPECT_EQ(t, refcount_read(&eb->refs), 1);
	vtxfs_eb_get(eb);
	KUNIT_EXPECT_EQ(t, refcount_read(&eb->refs), 2);
	vtxfs_eb_put(eb);
	KUNIT_EXPECT_EQ(t, refcount_read(&eb->refs), 1);
	vtxfs_eb_put(eb); /* frees via RCU */
}

static void test_io_tree_set_clear(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_io_tree tree;

	KUNIT_ASSERT_EQ(t, vtxfs_io_tree_init(&tree, NULL), 0);
	KUNIT_EXPECT_EQ(t, vtxfs_set_extent_bit(&tree, 0, 4095,
			VTXFS_EXT_DELALLOC | VTXFS_EXT_DIRTY), 0);
	KUNIT_EXPECT_EQ(t, tree.dirty_bytes, 4096ULL);
	KUNIT_EXPECT_EQ(t, vtxfs_clear_extent_bit(&tree, 0, 4095,
			VTXFS_EXT_DELALLOC | VTXFS_EXT_DIRTY), 0);
	KUNIT_EXPECT_EQ(t, tree.dirty_bytes, 0ULL);
	vtxfs_io_tree_destroy(&tree);
}


/* Verify canonical, disjoint intervals and correct dirty-byte accounting. */
static void expect_io_invariants(struct kunit *t,
				 struct vtxfs_extent_io_tree *tree)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n;
	u64 dirty = 0, prev_end = 0;
	u32 prev_bits = 0;
	bool have_prev = false;

	for (n = rb_first(&tree->states); n; n = rb_next(n)) {
		struct vtxfs_extent_state *s =
			rb_entry(n, struct vtxfs_extent_state, rb);

		KUNIT_EXPECT_LE(t, s->start, s->end);
		KUNIT_EXPECT_NE(t, s->bits, 0U);
		if (have_prev) {
			KUNIT_EXPECT_GT(t, s->start, prev_end);
			if (prev_end != U64_MAX && prev_end + 1 == s->start)
				KUNIT_EXPECT_NE(t, prev_bits, s->bits);
		}
		if (s->bits & VTXFS_EXT_DIRTY)
			dirty += s->end - s->start + 1;
		prev_end = s->end;
		prev_bits = s->bits;
		have_prev = true;
	}
	KUNIT_EXPECT_EQ(t, tree->dirty_bytes, dirty);
}

static u32 io_bits_at(struct vtxfs_extent_io_tree *tree, u64 pos)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n = tree->states.rb_node;

	while (n) {
		struct vtxfs_extent_state *s =
			rb_entry(n, struct vtxfs_extent_state, rb);

		if (pos < s->start)
			n = n->rb_left;
		else if (pos > s->end)
			n = n->rb_right;
		else
			return s->bits;
	}
	return 0;
}

static void test_io_tree_overlapping_bits(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_io_tree tree;

	KUNIT_ASSERT_EQ(t, vtxfs_io_tree_init(&tree, NULL), 0);
	KUNIT_ASSERT_EQ(t, vtxfs_set_extent_bit(&tree, 100, 199,
					      VTXFS_EXT_DIRTY), 0);
	KUNIT_ASSERT_EQ(t, vtxfs_set_extent_bit(&tree, 150, 249,
					      VTXFS_EXT_UPTODATE), 0);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 99), 0U);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 120), VTXFS_EXT_DIRTY);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 180),
			VTXFS_EXT_DIRTY | VTXFS_EXT_UPTODATE);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 220), VTXFS_EXT_UPTODATE);
	KUNIT_EXPECT_EQ(t, tree.dirty_bytes, 100ULL);
	KUNIT_ASSERT_EQ(t, vtxfs_clear_extent_bit(&tree, 170, 180,
						VTXFS_EXT_DIRTY), 0);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 169),
			VTXFS_EXT_DIRTY | VTXFS_EXT_UPTODATE);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 175), VTXFS_EXT_UPTODATE);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 181),
			VTXFS_EXT_DIRTY | VTXFS_EXT_UPTODATE);
	KUNIT_EXPECT_EQ(t, tree.dirty_bytes, 89ULL);
	KUNIT_ASSERT_EQ(t, vtxfs_clear_extent_bit(&tree, 130, 249,
						VTXFS_EXT_UPTODATE), 0);
	expect_io_invariants(t, &tree);
	vtxfs_io_tree_destroy(&tree);
}

static void test_io_tree_gap_merge_and_clear(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_io_tree tree;

	KUNIT_ASSERT_EQ(t, vtxfs_io_tree_init(&tree, NULL), 0);
	KUNIT_ASSERT_EQ(t, vtxfs_set_extent_bit(&tree, 0, 9,
					      VTXFS_EXT_DELALLOC), 0);
	KUNIT_ASSERT_EQ(t, vtxfs_set_extent_bit(&tree, 20, 29,
					      VTXFS_EXT_DELALLOC), 0);
	KUNIT_ASSERT_EQ(t, vtxfs_set_extent_bit(&tree, 5, 25,
					      VTXFS_EXT_DELALLOC), 0);
	KUNIT_EXPECT_PTR_EQ(t, rb_first(&tree.states), rb_last(&tree.states));
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 0), VTXFS_EXT_DELALLOC);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 29), VTXFS_EXT_DELALLOC);
	KUNIT_ASSERT_EQ(t, vtxfs_set_extent_bit(&tree, 5, 24,
					      VTXFS_EXT_LOCKED), 0);
	KUNIT_ASSERT_EQ(t, vtxfs_clear_extent_bit(&tree, 8, 22,
						VTXFS_EXT_LOCKED), 0);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 7),
			VTXFS_EXT_DELALLOC | VTXFS_EXT_LOCKED);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 11), VTXFS_EXT_DELALLOC);
	KUNIT_EXPECT_EQ(t, io_bits_at(&tree, 23),
			VTXFS_EXT_DELALLOC | VTXFS_EXT_LOCKED);
	KUNIT_ASSERT_EQ(t, vtxfs_clear_extent_bit(&tree, 0, 29,
						VTXFS_EXT_DELALLOC | VTXFS_EXT_LOCKED), 0);
	KUNIT_EXPECT_TRUE(t, RB_EMPTY_ROOT(&tree.states));
	expect_io_invariants(t, &tree);
	vtxfs_io_tree_destroy(&tree);
}

static void test_map_ownership_and_ranges(struct kunit *t)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_map_tree tree = { .mappings = RB_ROOT };
	struct vtxfs_extent_map *a, *b, *overlap, *hold;

	rwlock_init(&tree.lock);
	a = kzalloc(sizeof(*a), GFP_KERNEL);
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	overlap = kzalloc(sizeof(*overlap), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t, a);
	KUNIT_ASSERT_NOT_NULL(t, b);
	KUNIT_ASSERT_NOT_NULL(t, overlap);
	a->start = 0; a->len = 100;
	b->start = 100; b->len = 100;
	overlap->start = 99; overlap->len = 2;
	KUNIT_ASSERT_EQ(t, vtxfs_add_extent_mapping(&tree, a), 0);
	KUNIT_ASSERT_EQ(t, vtxfs_add_extent_mapping(&tree, b), 0);
	KUNIT_EXPECT_EQ(t, vtxfs_add_extent_mapping(&tree, overlap), -EEXIST);
	KUNIT_EXPECT_PTR_EQ(t, vtxfs_lookup_extent_mapping(&tree, 200, 1), NULL);
	KUNIT_EXPECT_PTR_EQ(t, vtxfs_lookup_extent_mapping(&tree, U64_MAX, 2),
			    NULL);
	hold = vtxfs_lookup_extent_mapping(&tree, 75, 50);
	KUNIT_ASSERT_PTR_EQ(t, hold, a);
	KUNIT_EXPECT_EQ(t, refcount_read(&a->refs), 2);
	vtxfs_extent_map_tree_destroy(&tree);
	KUNIT_EXPECT_TRUE(t, RB_EMPTY_ROOT(&tree.mappings));
	KUNIT_EXPECT_EQ(t, refcount_read(&hold->refs), 1);
	vtxfs_free_extent_map(hold);
	kfree(overlap);
}

static struct kunit_case vtxfs_ctree_cases[] = {
	KUNIT_CASE(test_crc32c_vector),
	KUNIT_CASE(test_blake3_empty),
	KUNIT_CASE(test_leaf_insert_find_delete),
	KUNIT_CASE(test_eb_refcount),
	KUNIT_CASE(test_io_tree_set_clear),
	KUNIT_CASE(test_io_tree_overlapping_bits),
	KUNIT_CASE(test_io_tree_gap_merge_and_clear),
	KUNIT_CASE(test_map_ownership_and_ranges),
	{}
};

static struct kunit_suite vtxfs_ctree_suite = {
	.name = "vtxfs_ctree",
	.test_cases = vtxfs_ctree_cases,
};
kunit_test_suite(vtxfs_ctree_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for VTXFS kernel ctree/extent_io/csum");
