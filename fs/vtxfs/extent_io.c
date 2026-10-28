// SPDX-License-Identifier: GPL-2.0-only
/*
 * extent_io.c — extent buffer lifecycle (cache/refcount/disk I/O/BLAKE3),
 * delalloc extent-state io_tree and extent_map cache, per vtxfs_mem.h.
 */
#include <linux/slab.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/ratelimit.h>

#include "vtxfs_drv.h"
#include "ctree.h"
#include "vag.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"

#define VTXFS_EB_CACHE_MAX 4096UL

/* Resolve whole metadata runs, not just their first VBLK. Bounds must
 * be checked on the actual block device (including self-test RAM disks).
 */
static int eb_resolve_sector(struct vtxfs_fs_info *fs, u64 vblk,
                             u32 blocks, sector_t *sector)
{
	VTXFS_TRACE_ENTER();

    u64 pblk = vblk, device_blocks;
    int ret;

    if (!fs || !fs->bdev || !blocks || !sector)
        return -EINVAL;
    if (fs->vag) {
        ret = vtxfs_vag_run_to_pblk(fs->vag, vblk, blocks, &pblk);
        if (ret)
            return ret;
    }
    device_blocks = bdev_nr_bytes(fs->bdev) >> VTXFS_BLOCK_SHIFT;
    if (pblk >= device_blocks || blocks > device_blocks - pblk)
        return -ERANGE;
    *sector = (sector_t)pblk << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT);
    return 0;
}

/* ---------------- extent buffer ---------------- */

struct vtxfs_extent_buffer *vtxfs_eb_alloc(struct vtxfs_fs_info *fs,
					   u64 vblk, u32 node_size)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;

	eb = kzalloc(sizeof(*eb), GFP_NOFS);
	if (!eb)
		return ERR_PTR(-ENOMEM);
	eb->hdr = kvzalloc(node_size, GFP_NOFS);
	if (!eb->hdr) {
		kfree(eb);
		return ERR_PTR(-ENOMEM);
	}
	eb->fs = fs;
	eb->vblk = vblk;
	eb->node_size = node_size;
	refcount_set(&eb->refs, 1);
	init_rwsem(&eb->lock);
	INIT_LIST_HEAD(&eb->lru);
	INIT_LIST_HEAD(&eb->dirty_link);
	return eb;
}

struct vtxfs_extent_buffer *vtxfs_find_eb(struct vtxfs_fs_info *fs, u64 vblk)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb;
	unsigned long flags;

	xa_lock_irqsave(&fs->eb_cache, flags);
	eb = xa_load(&fs->eb_cache, vblk);
	if (eb && !refcount_inc_not_zero(&eb->refs))
		eb = NULL;
	xa_unlock_irqrestore(&fs->eb_cache, flags);
	return eb;
}

static void vtxfs_eb_free_rcu(struct rcu_head *head)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb =
		container_of(head, struct vtxfs_extent_buffer, rcu);

	kvfree(eb->hdr);
	kfree(eb);
}

void vtxfs_eb_get(struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	refcount_inc(&eb->refs);
}

void vtxfs_eb_put(struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = eb->fs;
	unsigned long flags;

	if (!refcount_dec_and_test(&eb->refs))
		return;
	xa_lock_irqsave(&fs->eb_cache, flags);
	if (xa_load(&fs->eb_cache, eb->vblk) == eb)
		__xa_erase(&fs->eb_cache, eb->vblk);
	/* xa_erase() would reacquire xa_lock and deadlock this path. */
	xa_unlock_irqrestore(&fs->eb_cache, flags);
	call_rcu(&eb->rcu, vtxfs_eb_free_rcu);
}

static int eb_submit_read(struct block_device *bdev, sector_t sector,
			  u32 blocks, void *buf)
{
	VTXFS_TRACE_ENTER();

	u32 i;
	int ret = 0;

	for (i = 0; i < blocks; i++) {
		u8 *blk = (u8 *)buf + (size_t)i * VTXFS_BLOCK_SIZE;
		struct page *page = vtx_buf_to_page(blk);
		struct bio *bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_NOFS);

		bio->bi_iter.bi_sector =
			sector + ((sector_t)i << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT));
		if (bio_add_page(bio, page, VTXFS_BLOCK_SIZE,
				 offset_in_page(blk)) != VTXFS_BLOCK_SIZE) {
			bio_put(bio);
			return -EIO;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			return ret;
	}
	return 0;
}

static int eb_verify_identity(struct vtxfs_fs_info *fs,
			      struct vtxfs_extent_buffer *eb,
			      u64 oid, u8 level)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h = eb->hdr;

	if (!vtx_node_slots_valid(h, eb->node_size))
		return -EUCLEAN;
	if (!vtx_node_checksum_verify(h, eb->node_size, fs->metadata_uuid))
		return -EUCLEAN;
	if (oid && vtx_cpu64(h->oid) != oid)
		return -EUCLEAN;
	if (level != (u8)-1 && h->level != level)
		return -EUCLEAN;
	if (vtx_cpu64(h->xid) == 0 || vtx_cpu64(h->xid) > fs->committed_xid)
		return -EUCLEAN;
	return 0;
}

/*
 * Read one tree block from disk into the eb cache and verify the
 * BLAKE3-128 domain checksum plus slotted geometry.
 */
int vtxfs_read_tree_block(struct vtxfs_fs_info *fs, struct vtxfs_root *root,
			  u64 oid, u64 vblk, u8 level,
			  struct vtxfs_extent_buffer **eb_out)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb, *exist;
	sector_t sector;
	u32 blocks;
	int ret;

	VTXFS_TRACE_EVENT("tree-read oid=%llu vblk=%llu lvl=%u\n",
			  (unsigned long long)oid, (unsigned long long)vblk, level);
	(void)root;
	eb = vtxfs_find_eb(fs, vblk);
	if (eb) {
		ret = eb_verify_identity(fs, eb, oid, level);
		if (ret) {
			vtxfs_eb_put(eb);
			return ret;
		}
		*eb_out = eb;
		return 0;
	}

	eb = vtxfs_eb_alloc(fs, vblk, fs->node_size);
	if (IS_ERR(eb))
		return PTR_ERR(eb);

	/* A multi-block node must not cross VAG or device boundaries. */
	if (!fs->node_size || (fs->node_size & (VTXFS_BLOCK_SIZE - 1))) {
		vtxfs_eb_put(eb);
		return -EINVAL;
	}
	blocks = fs->node_size >> VTXFS_BLOCK_SHIFT;
	ret = eb_resolve_sector(fs, vblk, blocks, &sector);
	if (ret) {
		vtxfs_eb_put(eb);
		return ret;
	}
	ret = eb_submit_read(fs->bdev, sector, blocks, eb->hdr);
	if (ret) {
		vtxfs_eb_put(eb);
		return ret;
	}
	ret = eb_verify_identity(fs, eb, oid, level);
	if (ret) {
		vtxfs_eb_put(eb);
		return ret;
	}
	eb->oid = vtx_cpu64(eb->hdr->oid);
	eb->allocation_id = 0;	/* resolved from REF tree by extent-tree layer */
	eb->birth_xid = vtx_cpu64(eb->hdr->xid);
	set_bit(VTXFS_EB_UPTODATE, &eb->flags);
	set_bit(VTXFS_EB_VERIFIED, &eb->flags);

	exist = xa_cmpxchg(&fs->eb_cache, vblk, NULL, eb, GFP_NOFS);
	if (xa_is_err(exist)) {
		ret = xa_err(exist);
		vtxfs_eb_put(eb);
		return ret;
	}
	if (exist) {			/* race: someone else inserted first */
		vtxfs_eb_put(eb);
		if (!refcount_inc_not_zero(&exist->refs))
			return -EAGAIN;
		eb = exist;
	}
	*eb_out = eb;
	return 0;
}

void vtxfs_mark_eb_dirty(struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs = eb->fs;
	unsigned long flags;

	if (test_and_set_bit(VTXFS_EB_DIRTY, &eb->flags))
		return;
	spin_lock_irqsave(&fs->eb_lock, flags);
	list_add_tail(&eb->dirty_link, &fs->dirty_ebs);
	spin_unlock_irqrestore(&fs->eb_lock, flags);
	vtxfs_eb_get(eb);	/* dirty list holds a reference */
}

int vtxfs_write_eb(struct vtxfs_fs_info *fs, struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	u32 i, blocks = eb->node_size >> VTXFS_BLOCK_SHIFT;
	sector_t sector;
	int ret;

	VTXFS_TRACE_EVENT("tree-write vblk=%llu blocks=%u\n",
			  (unsigned long long)eb->vblk, blocks);
	down_write(&eb->lock);
	set_bit(VTXFS_EB_WRITEBACK, &eb->flags);
	ret = vtx_node_checksum_compute(eb->hdr, eb->node_size,
					fs->metadata_uuid);
	if (!ret) {
		ret = eb_resolve_sector(fs, eb->vblk, blocks, &sector);
		if (!ret) {
			for (i = 0; i < blocks && !ret; i++) {
				u8 *blk = (u8 *)eb->hdr + (size_t)i * VTXFS_BLOCK_SIZE;
				struct page *page = vtx_buf_to_page(blk);
				struct bio *bio = bio_alloc(fs->bdev, 1,
						REQ_OP_WRITE | REQ_SYNC, GFP_NOFS);

				bio->bi_iter.bi_sector =
					sector + ((sector_t)i << (VTXFS_BLOCK_SHIFT - SECTOR_SHIFT));
				if (bio_add_page(bio, page, VTXFS_BLOCK_SIZE,
						 offset_in_page(blk)) != VTXFS_BLOCK_SIZE) {
					bio_put(bio);
					ret = -EIO;
					break;
				}
				ret = submit_bio_wait(bio);
				bio_put(bio);
			}
		}
	}
	clear_bit(VTXFS_EB_WRITEBACK, &eb->flags);
	if (ret)
		set_bit(VTXFS_EB_IOERR, &eb->flags);
	up_write(&eb->lock);
	return ret;
}

/* Step 3 of the ordered checkpoint: persist every dirty tree block. */
int vtxfs_write_dirty_ebs(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *eb, *tmp;
	LIST_HEAD(list);
	LIST_HEAD(failed);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&fs->eb_lock, flags);
	list_splice_init(&fs->dirty_ebs, &list);
	spin_unlock_irqrestore(&fs->eb_lock, flags);

	list_for_each_entry_safe(eb, tmp, &list, dirty_link) {
		int err = vtxfs_write_eb(fs, eb);
		if (err) {
			if (!ret)
				ret = err;
			list_move_tail(&eb->dirty_link, &failed);
			continue;
		}
		list_del_init(&eb->dirty_link);
		clear_bit(VTXFS_EB_DIRTY, &eb->flags);
		vtxfs_eb_put(eb);
	}

	if (!list_empty(&failed)) {
		spin_lock_irqsave(&fs->eb_lock, flags);
		list_splice(&failed, &fs->dirty_ebs);
		spin_unlock_irqrestore(&fs->eb_lock, flags);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(vtxfs_write_dirty_ebs);

/* ---------------- io_tree (delalloc extent state) ---------------- */

int vtxfs_io_tree_init(struct vtxfs_extent_io_tree *tree,
		       struct vtxfs_inode *inode)
{
	VTXFS_TRACE_ENTER();

	tree->states = RB_ROOT;
	spin_lock_init(&tree->lock);
	tree->inode = inode;
	tree->dirty_bytes = 0;
	init_waitqueue_head(&tree->lock_wq);
	return 0;
}

void vtxfs_io_tree_destroy(struct vtxfs_extent_io_tree *tree)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n;
	unsigned long flags;

	spin_lock_irqsave(&tree->lock, flags);
	while ((n = rb_first(&tree->states)) != NULL) {
		struct vtxfs_extent_state *s =
			rb_entry(n, struct vtxfs_extent_state, rb);

		rb_erase(&s->rb, &tree->states);
		kfree(s);
	}
	tree->dirty_bytes = 0;
	spin_unlock_irqrestore(&tree->lock, flags);
}

/* A canonical interval tree: non-overlapping, non-empty, adjacent equal
 * states are coalesced. All mutations are serialized by tree->lock.
 * Allocation happens BEFORE taking the spinlock and is retried if a
 * concurrent writer increases the number of nodes required.
 */
static struct vtxfs_extent_state *find_state(struct rb_root *root, u64 pos)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n = root->rb_node;

	while (n) {
		struct vtxfs_extent_state *s =
			rb_entry(n, struct vtxfs_extent_state, rb);

		if (pos < s->start)
			n = n->rb_left;
		else if (pos > s->end)
			n = n->rb_right;
		else
			return s;
	}
	return NULL;
}

static struct vtxfs_extent_state *state_first_ge(struct rb_root *root,
						 u64 pos)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n = root->rb_node;
	struct vtxfs_extent_state *candidate = NULL;

	while (n) {
		struct vtxfs_extent_state *s =
			rb_entry(n, struct vtxfs_extent_state, rb);

		if (s->end < pos) {
			n = n->rb_right;
		} else {
			candidate = s;
			n = n->rb_left;
		}
	}
	return candidate;
}

static struct vtxfs_extent_state *state_next(struct vtxfs_extent_state *s)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n = rb_next(&s->rb);

	return n ? rb_entry(n, struct vtxfs_extent_state, rb) : NULL;
}

static void state_insert(struct vtxfs_extent_io_tree *tree,
			 struct vtxfs_extent_state *new)
{
	VTXFS_TRACE_ENTER();

	struct rb_node **p = &tree->states.rb_node, *parent = NULL;

	while (*p) {
		struct vtxfs_extent_state *s;

		parent = *p;
		s = rb_entry(parent, struct vtxfs_extent_state, rb);
		if (new->start < s->start)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}
	rb_link_node(&new->rb, parent, p);
	rb_insert_color(&new->rb, &tree->states);
}

static struct vtxfs_extent_state *state_take(
		struct vtxfs_extent_state **reserve, size_t *used,
		u64 start, u64 end, u32 bits)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_state *s = reserve[(*used)++];

	s->start = start;
	s->end = end;
	s->bits = bits;
	refcount_set(&s->refs, 1);
	init_waitqueue_head(&s->wq);
	return s;
}

/* Caller holds tree->lock and has reserved enough nodes. */
static void state_split(struct vtxfs_extent_io_tree *tree, u64 at,
			struct vtxfs_extent_state **reserve, size_t *used)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_state *s = find_state(&tree->states, at);
	struct vtxfs_extent_state *right;

	if (!s || s->start == at)
		return;
	right = state_take(reserve, used, at, s->end, s->bits);
	s->end = at - 1;
	state_insert(tree, right);
}

static size_t state_required(struct vtxfs_extent_io_tree *tree,
			     u64 start, u64 end, bool set)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_state *s;
	size_t needed = 2; /* at most two boundary splits */
	u64 cursor = start;

	if (!set)
		return needed;
	s = state_first_ge(&tree->states, start);
	while (cursor <= end) {
		if (!s || s->start > end) {
			needed++;
			break;
		}
		if (cursor < s->start)
			needed++; /* a gap to materialize */
		if (s->end >= end)
			break;
		cursor = s->end + 1;
		s = state_next(s);
	}
	return needed;
}

static bool state_has_locked(struct vtxfs_extent_io_tree *tree,
			     u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_state *s =
		state_first_ge(&tree->states, start);

	for (; s && s->start <= end; s = state_next(s)) {
		if (s->bits & VTXFS_EXT_LOCKED)
			return true;
	}
	return false;
}

static bool extent_range_locked(struct vtxfs_extent_io_tree *tree,
				u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	unsigned long flags;
	bool locked;

	spin_lock_irqsave(&tree->lock, flags);
	locked = state_has_locked(tree, start, end);
	spin_unlock_irqrestore(&tree->lock, flags);
	return locked;
}

static void state_update(struct vtxfs_extent_io_tree *tree,
			 struct vtxfs_extent_state *s, u32 bits, bool set)
{
	VTXFS_TRACE_ENTER();

	u32 before = s->bits;
	u32 after = set ? before | bits : before & ~bits;
	u64 bytes = s->end - s->start + 1;

	if (!!(before & VTXFS_EXT_DIRTY) != !!(after & VTXFS_EXT_DIRTY)) {
		if (after & VTXFS_EXT_DIRTY)
			tree->dirty_bytes += bytes;
		else
			tree->dirty_bytes -= bytes;
	}
	s->bits = after;
	if (!after) {
		rb_erase(&s->rb, &tree->states);
		kfree(s);
	}
}

static void state_coalesce(struct vtxfs_extent_io_tree *tree,
			   u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_state *s =
		state_first_ge(&tree->states, start);
	struct rb_node *prev;

	if (!s)
		return;
	prev = rb_prev(&s->rb);
	if (prev)
		s = rb_entry(prev, struct vtxfs_extent_state, rb);

	while (s) {
		struct vtxfs_extent_state *next = state_next(s);

		if (!next)
			break;
		if (s->end != U64_MAX && s->end + 1 == next->start &&
		    s->bits == next->bits) {
			s->end = next->end;
			rb_erase(&next->rb, &tree->states);
			kfree(next);
			continue;
		}
		if (s->start > end && next->start > end)
			break;
		s = next;
	}
}

/* Locked transaction, using preallocated objects. */
static void state_apply(struct vtxfs_extent_io_tree *tree,
			u64 start, u64 end, u32 bits, bool set,
			struct vtxfs_extent_state **reserve, size_t *used)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_state *s;
	u64 cursor = start;

	state_split(tree, start, reserve, used);
	if (end != U64_MAX)
		state_split(tree, end + 1, reserve, used);
	s = state_first_ge(&tree->states, start);
	while (cursor <= end) {
		struct vtxfs_extent_state *next;
		u64 last;

		if (!s || s->start > end) {
			if (set) {
				state_insert(tree, state_take(reserve, used,
							    cursor, end, bits));
				if (bits & VTXFS_EXT_DIRTY)
					tree->dirty_bytes += end - cursor + 1;
			}
			break;
		}
		if (cursor < s->start) {
			last = min(end, s->start - 1);
			if (set) {
				state_insert(tree, state_take(reserve, used,
							    cursor, last, bits));
				if (bits & VTXFS_EXT_DIRTY)
					tree->dirty_bytes += last - cursor + 1;
			}
		}
		next = state_next(s);
		last = s->end;
		state_update(tree, s, bits, set);
		if (last >= end)
			break;
		cursor = last + 1;
		s = next;
	}
	state_coalesce(tree, start, end);
}

/* Inclusive range, all-or-nothing allocation; never allocate with spinlock held.
 * fail_locked makes testing and setting LOCKED atomic for lock_extent().
 */
static int extent_modify(struct vtxfs_extent_io_tree *tree,
			 u64 start, u64 end, u32 bits,
			 bool set, bool fail_locked)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_state **reserve = NULL;
	size_t capacity = 0, needed, used = 0, i;
	unsigned long flags;
	int ret = 0;

	if (end < start)
		return -EINVAL;
	if (!bits)
		return 0;
	/* dirty_bytes is a u64, so the full 2^64-byte range is invalid. */
	if ((bits & VTXFS_EXT_DIRTY) && start == 0 && end == U64_MAX)
		return -ERANGE;

	for (;;) {
		spin_lock_irqsave(&tree->lock, flags);
		if (fail_locked && state_has_locked(tree, start, end)) {
			ret = -EAGAIN;
			spin_unlock_irqrestore(&tree->lock, flags);
			break;
		}
		needed = state_required(tree, start, end, set);
		if (needed <= capacity) {
			state_apply(tree, start, end, bits, set, reserve, &used);
			spin_unlock_irqrestore(&tree->lock, flags);
			if (!set)
				wake_up_all(&tree->lock_wq);
			break;
		}
		spin_unlock_irqrestore(&tree->lock, flags);

		for (i = 0; i < capacity; i++)
			kfree(reserve[i]);
		kvfree(reserve);
		reserve = kvmalloc_array(needed, sizeof(*reserve), GFP_NOFS);
		capacity = 0;
		if (!reserve)
			return -ENOMEM;
		for (i = 0; i < needed; i++) {
			reserve[i] = kzalloc(sizeof(**reserve), GFP_NOFS);
			if (!reserve[i]) {
				while (i)
					kfree(reserve[--i]);
				kvfree(reserve);
				return -ENOMEM;
			}
		}
		capacity = needed;
	}
	for (i = used; i < capacity; i++)
		kfree(reserve[i]);
	kvfree(reserve);
	return ret;
}

int vtxfs_set_extent_bit(struct vtxfs_extent_io_tree *tree,
			 u64 start, u64 end, u32 bits)
{
	VTXFS_TRACE_ENTER();

	VTXFS_TRACE_EVENT("extent-set start=%llu end=%llu bits=0x%x\n",
			  (unsigned long long)start, (unsigned long long)end, bits);
	return extent_modify(tree, start, end, bits, true, false);
}

int vtxfs_clear_extent_bit(struct vtxfs_extent_io_tree *tree,
			   u64 start, u64 end, u32 bits)
{
	VTXFS_TRACE_ENTER();

	VTXFS_TRACE_EVENT("extent-clear start=%llu end=%llu bits=0x%x\n",
			  (unsigned long long)start, (unsigned long long)end, bits);
	return extent_modify(tree, start, end, bits, false, false);
}

int vtxfs_lock_extent(struct vtxfs_extent_io_tree *tree, u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	int ret;

	for (;;) {
		ret = extent_modify(tree, start, end, VTXFS_EXT_LOCKED,
				    true, true);
		if (ret != -EAGAIN)
			return ret;
		wait_event(tree->lock_wq,
			   !extent_range_locked(tree, start, end));
	}
}

void vtxfs_unlock_extent(struct vtxfs_extent_io_tree *tree,
			 u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	vtxfs_clear_extent_bit(tree, start, end, VTXFS_EXT_LOCKED);
}

/* ---------------- extent_map ---------------- */

int vtxfs_add_extent_mapping(struct vtxfs_extent_map_tree *tree,
			     struct vtxfs_extent_map *em)
{
	VTXFS_TRACE_ENTER();

	struct rb_node **p, *parent = NULL;
	struct vtxfs_extent_map *cur;
	unsigned long flags;

	if (!em || !em->len || em->len > U64_MAX - em->start)
		return -EINVAL;
	write_lock_irqsave(&tree->lock, flags);
	p = &tree->mappings.rb_node;
	while (*p) {
		parent = *p;
		cur = rb_entry(parent, struct vtxfs_extent_map, rb);
		if (em->start + em->len <= cur->start) {
			p = &parent->rb_left;
		} else if (em->start >= cur->start + cur->len) {
			p = &parent->rb_right;
		} else {
			write_unlock_irqrestore(&tree->lock, flags);
			return -EEXIST;
		}
	}
	refcount_set(&em->refs, 1);
	rb_link_node(&em->rb, parent, p);
	rb_insert_color(&em->rb, &tree->mappings);
	write_unlock_irqrestore(&tree->lock, flags);
	return 0;
}

struct vtxfs_extent_map *vtxfs_lookup_extent_mapping(
		struct vtxfs_extent_map_tree *tree, u64 start, u64 len)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n;
	unsigned long flags;

	if (!len || len > U64_MAX - start)
		return NULL;
	read_lock_irqsave(&tree->lock, flags);
	n = tree->mappings.rb_node;
	while (n) {
		struct vtxfs_extent_map *em =
			rb_entry(n, struct vtxfs_extent_map, rb);

		if (start + len <= em->start) {
			n = n->rb_left;
		} else if (start >= em->start + em->len) {
			n = n->rb_right;
		} else {
			refcount_inc(&em->refs);
			read_unlock_irqrestore(&tree->lock, flags);
			return em;
		}
	}
	read_unlock_irqrestore(&tree->lock, flags);
	return NULL;
}

void vtxfs_free_extent_map(struct vtxfs_extent_map *em)
{
	VTXFS_TRACE_ENTER();

	if (em && refcount_dec_and_test(&em->refs)) {
		kfree(em->checksums);
		kfree(em);
	}
}

/* Remove the tree-owned reference; lookup users keep their own references.
 * Free outside the rwlock so large checksum buffers do not stall readers.
 */
void vtxfs_extent_map_tree_destroy(struct vtxfs_extent_map_tree *tree)
{
	VTXFS_TRACE_ENTER();

	for (;;) {
		struct rb_node *n;
		struct vtxfs_extent_map *em;
		unsigned long flags;

		write_lock_irqsave(&tree->lock, flags);
		n = rb_first(&tree->mappings);
		if (!n) {
			write_unlock_irqrestore(&tree->lock, flags);
			break;
		}
		em = rb_entry(n, struct vtxfs_extent_map, rb);
		rb_erase(&em->rb, &tree->mappings);
		write_unlock_irqrestore(&tree->lock, flags);
		vtxfs_free_extent_map(em);
	}
}
