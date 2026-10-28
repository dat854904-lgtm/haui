// SPDX-License-Identifier: GPL-2.0-only
/*
 * VTXFS M3: advisory in-memory free-run cache.
 *
 * This is NEVER an allocator authority: only REF+SFQ+VAG can decide reuse.
 * Cache entries are valid only for a single committed checkpoint XID.
 * All mutations are under free_space_lock and validate the entire interval
 * before changing RB nodes. start_vblk is encoded (vag_id, block_offset).
 */
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/overflow.h>
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_trace.h"

struct vtxfs_free_extent {
	struct rb_node rb;
	u64 start_vblk, blocks, checkpoint_xid;
};

static bool valid_run(u64 start, u64 blocks)
{
	u32 vag, off;

	if (!blocks || blocks > VTXFS_VAG_BLOCKS ||
	    vtx_vblk_decode(start, &vag, &off) ||
	    blocks > VTXFS_VAG_BLOCKS - off)
		return false;
	return true;
}

static void cache_clear_locked(struct vtxfs_fs_info *fs)
{
	struct rb_node *node;

	while ((node = rb_first(&fs->free_space_tree))) {
		struct vtxfs_free_extent *entry =
			rb_entry(node, struct vtxfs_free_extent, rb);
		rb_erase(node, &fs->free_space_tree);
		kfree(entry);
	}
}

void vtxfs_free_space_invalidate(struct vtxfs_fs_info *fs)
{
	unsigned long flags;

	VTXFS_TRACE_ENTER();
	if (!fs)
		return;
	spin_lock_irqsave(&fs->free_space_lock, flags);
	cache_clear_locked(fs);
	spin_unlock_irqrestore(&fs->free_space_lock, flags);
}

static struct vtxfs_free_extent *cache_nearest(struct vtxfs_fs_info *fs,
						u64 start)
{
	struct rb_node *node = fs->free_space_tree.rb_node;
	struct vtxfs_free_extent *candidate = NULL;

	while (node) {
		struct vtxfs_free_extent *entry =
			rb_entry(node, struct vtxfs_free_extent, rb);

		if (entry->start_vblk <= start) {
			candidate = entry;
			node = node->rb_right;
		} else {
			node = node->rb_left;
		}
	}
	return candidate;
}

static void cache_insert_locked(struct vtxfs_fs_info *fs,
				 struct vtxfs_free_extent *entry)
{
	struct rb_node **p = &fs->free_space_tree.rb_node, *parent = NULL;

	while (*p) {
		struct vtxfs_free_extent *cur =
			rb_entry(*p, struct vtxfs_free_extent, rb);

		parent = *p;
		if (entry->start_vblk < cur->start_vblk)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&entry->rb, parent, p);
	rb_insert_color(&entry->rb, &fs->free_space_tree);
}

int vtxfs_free_space_add(struct vtxfs_fs_info *fs, u64 start_vblk, u64 blocks)
{
	struct vtxfs_free_extent *entry, *left, *right;
	struct rb_node *node;
	unsigned long flags;
	u64 end, xid, left_end;
	int ret = 0;

	VTXFS_TRACE_ENTER();
	if (!fs || !valid_run(start_vblk, blocks))
		return -EINVAL;
	end = start_vblk + blocks;
	entry = kzalloc(sizeof(*entry), GFP_NOFS);
	if (!entry)
		return -ENOMEM;

	spin_lock_irqsave(&fs->free_space_lock, flags);
	xid = fs->committed_xid;
	left = cache_nearest(fs, start_vblk);
	if (left && left->checkpoint_xid != xid) {
		cache_clear_locked(fs);
		left = NULL;
	}
	node = left ? rb_next(&left->rb) : rb_first(&fs->free_space_tree);
	right = node ? rb_entry(node, struct vtxfs_free_extent, rb) : NULL;
	if (right && right->checkpoint_xid != xid) {
		cache_clear_locked(fs);
		left = right = NULL;
	}
	left_end = left ? left->start_vblk + left->blocks : 0;
	if ((left && left_end > start_vblk) ||
	    (right && right->start_vblk < end)) {
		ret = -EEXIST;
		goto done;
	}
	/* Merge only within one VAG (validated combined run), and only
	 * with adjacent entries belonging to the same checkpoint. */
	if (left && left_end == start_vblk &&
	    valid_run(left->start_vblk, left->blocks + blocks)) {
		left->blocks += blocks;
		if (right && right->start_vblk == end &&
		    valid_run(left->start_vblk, left->blocks + right->blocks)) {
			left->blocks += right->blocks;
			rb_erase(&right->rb, &fs->free_space_tree);
			kfree(right);
		}
	} else if (right && right->start_vblk == end &&
		   valid_run(start_vblk, blocks + right->blocks)) {
		rb_erase(&right->rb, &fs->free_space_tree);
		right->start_vblk = start_vblk;
		right->blocks += blocks;
		cache_insert_locked(fs, right);
	} else {
		entry->start_vblk = start_vblk;
		entry->blocks = blocks;
		entry->checkpoint_xid = xid;
		cache_insert_locked(fs, entry);
		entry = NULL; /* cache owns it now */
	}
done:
	spin_unlock_irqrestore(&fs->free_space_lock, flags);
	kfree(entry);
	return ret;
}

int vtxfs_free_space_find(struct vtxfs_fs_info *fs, u64 blocks,
			  u64 *start_vblk)
{
	struct rb_node *node;
	unsigned long flags;
	int ret = -ENOSPC;

	VTXFS_TRACE_ENTER();
	if (!fs || !start_vblk || !blocks || blocks > VTXFS_VAG_BLOCKS)
		return -EINVAL;
	spin_lock_irqsave(&fs->free_space_lock, flags);
	for (node = rb_first(&fs->free_space_tree); node; node = rb_next(node)) {
		struct vtxfs_free_extent *entry =
			rb_entry(node, struct vtxfs_free_extent, rb);

		if (entry->checkpoint_xid != fs->committed_xid)
			continue;
		if (entry->blocks >= blocks) {
			*start_vblk = entry->start_vblk;
			ret = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->free_space_lock, flags);
	/* A returned range is a HINT ONLY; caller MUST reserve via REF+SFQ/VAG. */
	return ret;
}

int vtxfs_free_space_remove(struct vtxfs_fs_info *fs, u64 start_vblk,
			    u64 blocks)
{
	struct vtxfs_free_extent *entry, *tail = NULL;
	unsigned long flags;
	u64 end, entry_end;
	int ret = 0;

	VTXFS_TRACE_ENTER();
	if (!fs || !valid_run(start_vblk, blocks))
		return -EINVAL;
	end = start_vblk + blocks;
	/* Preallocate for middle split; never allocate while spinlocked. */
	tail = kzalloc(sizeof(*tail), GFP_NOFS);
	if (!tail)
		return -ENOMEM;
	spin_lock_irqsave(&fs->free_space_lock, flags);
	entry = cache_nearest(fs, start_vblk);
	if (!entry || entry->checkpoint_xid != fs->committed_xid) {
		ret = -ENOENT;
		goto done;
	}
	entry_end = entry->start_vblk + entry->blocks;
	if (end > entry_end) {
		ret = -ENOENT;
		goto done;
	}
	if (start_vblk == entry->start_vblk && end == entry_end) {
		rb_erase(&entry->rb, &fs->free_space_tree);
		kfree(entry);
	} else if (start_vblk == entry->start_vblk) {
		rb_erase(&entry->rb, &fs->free_space_tree);
		entry->start_vblk = end;
		entry->blocks = entry_end - end;
		cache_insert_locked(fs, entry);
	} else if (end == entry_end) {
		entry->blocks = start_vblk - entry->start_vblk;
	} else {
		tail->start_vblk = end;
		tail->blocks = entry_end - end;
		tail->checkpoint_xid = entry->checkpoint_xid;
		entry->blocks = start_vblk - entry->start_vblk;
		cache_insert_locked(fs, tail);
		tail = NULL;
	}
done:
	spin_unlock_irqrestore(&fs->free_space_lock, flags);
	kfree(tail);
	return ret;
}

int vtxfs_free_space_load(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();
	/* Rebuildable cache is intentionally empty until an atomic snapshot
	 * of REF/SFQ/VAG is available. Avoid falsely returning successful
	 * on-disk free-space loading. */
	if (!fs)
		return -EINVAL;
	vtxfs_free_space_invalidate(fs);
	return -EOPNOTSUPP;
}
