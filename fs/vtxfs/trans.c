// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
#include <linux/blkdev.h>
#include "vtxfs_drv.h"
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

struct vtxfs_trans *vtxfs_start_trans(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_trans *trans;

	if (fs->shadow_only)
		return ERR_PTR(-EROFS);
	mutex_lock(&fs->trans_lock);
	if (fs->running) {
		refcount_inc(&fs->running->joiners);
		mutex_unlock(&fs->trans_lock);
		return fs->running;
	}
	trans = kzalloc(sizeof(*trans), GFP_NOFS);
	if (!trans) {
		mutex_unlock(&fs->trans_lock);
		return ERR_PTR(-ENOMEM);
	}
	trans->fs = fs;
	trans->xid = fs->current_xid;
	trans->phase = VTXFS_TRANS_OPEN;
	refcount_set(&trans->joiners, 1);
	INIT_LIST_HEAD(&trans->dirty_roots);
	INIT_LIST_HEAD(&trans->dirty_ebs);
	INIT_LIST_HEAD(&trans->retired_ebs);
	trans->delayed_refs = RB_ROOT;
	spin_lock_init(&trans->delayed_ref_lock);
	atomic64_set(&trans->bytes_reserved, 0);
	fs->running = trans;
	mutex_unlock(&fs->trans_lock);
	return trans;
}

int vtxfs_trans_join(struct vtxfs_trans *trans)
{
	VTXFS_TRACE_ENTER();

	if (!trans || trans->phase != VTXFS_TRANS_OPEN)
		return -EINVAL;
	refcount_inc(&trans->joiners);
	return 0;
}

void vtxfs_add_dirty_root(struct vtxfs_trans *trans, struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	if (!root || test_and_set_bit(VTXFS_ROOT_DIRTY, &root->state))
		return;
	spin_lock(&trans->delayed_ref_lock);
	list_add_tail(&root->dirty_link, &trans->dirty_roots);
	spin_unlock(&trans->delayed_ref_lock);
}

/* Drain queued delayed refs into REF/ALLOC_BY_VBLK/SFQ trees. */
int vtxfs_run_delayed_refs(struct vtxfs_trans *trans)
{
	VTXFS_TRACE_ENTER();

	struct rb_node *n;
	int ret = 0;

	spin_lock(&trans->delayed_ref_lock);
	while ((n = rb_first(&trans->delayed_refs)) != NULL) {
		struct vtxfs_delayed_ref *dr =
			rb_entry(n, struct vtxfs_delayed_ref, rb);

		rb_erase(&dr->rb, &trans->delayed_refs);
		spin_unlock(&trans->delayed_ref_lock);

		ret = dr->delta > 0
			? vtxfs_add_extent_ref(trans->fs,
					dr->allocation_id, dr->start_vblk,
					dr->blocks, dr->flags, 0)
			: vtxfs_drop_extent_ref(trans->fs, dr->allocation_id,
					dr->start_vblk, dr->blocks);
		kfree(dr);
		if (ret)
			return ret;
		spin_lock(&trans->delayed_ref_lock);
	}
	spin_unlock(&trans->delayed_ref_lock);
	return 0;
}

int vtxfs_commit_transaction(struct vtxfs_fs_info *fs,
			     struct vtxfs_trans *trans)
{
	VTXFS_TRACE_ENTER();

	int ret;

	if (fs->shadow_only)
		return -EROFS;
	VTXFS_TRACE_EVENT("commit xid=%llu initial_phase=%d\n",
			  (unsigned long long)(trans ? trans->xid : 0),
			  trans ? (int)trans->phase : -1);
	if (!trans || trans->phase != VTXFS_TRANS_OPEN)
		return -EINVAL;

	trans->phase = VTXFS_TRANS_QUIESCING;
	down_write(&fs->commit_sem);

	trans->phase = VTXFS_TRANS_WRITING;
	/* (a) chuyển mọi delalloc còn lại thành ordered extent */
	ret = vtxfs_flush_all_delalloc(fs);
	if (ret)
		goto abort;
	/* (b) chờ toàn bộ I/O dữ liệu bền trước khi ghi metadata */
	ret = vtxfs_wait_ordered_all(fs);
	if (ret)
		goto abort;
	/* (c) drain delayed-ref vào REF/SFQ */
	ret = vtxfs_run_delayed_refs(trans);
	if (ret)
		goto abort;
	/* (d) SFQ promotion kernel-side: thu hồi block đã an toàn */
	ret = vtxfs_sfq_promote(fs, fs->committed_xid, NULL);
	if (ret)
		goto abort;
	/* (e) ghi khối cây bẩn (metadata) */
	ret = vtxfs_write_dirty_ebs(fs);
	if (ret)
		goto abort;
	ret = blkdev_issue_flush(fs->bdev);
	if (ret)
		goto abort;

	trans->phase = VTXFS_TRANS_PUBLISHING;
	ret = vtxfs_publish_cpd(fs, trans->xid);	/* cpd.c */
	if (ret)
		goto abort;

	VTXFS_TRACE_EVENT("commit xid=%llu published=1\n",
			  (unsigned long long)trans->xid);
	trans->phase = VTXFS_TRANS_COMMITTED;
	fs->committed_xid = trans->xid;
	fs->current_xid = trans->xid + 1;
	up_write(&fs->commit_sem);
	return 0;
abort:
	VTXFS_TRACE_EVENT("commit xid=%llu ABORT ret=%d\n",
			  (unsigned long long)trans->xid, ret);
	trans->phase = VTXFS_TRANS_ABORTED;
	{
		struct rb_node *n;
		spin_lock(&trans->delayed_ref_lock);
		while ((n = rb_first(&trans->delayed_refs)) != NULL) {
			struct vtxfs_delayed_ref *dr =
				rb_entry(n, struct vtxfs_delayed_ref, rb);
			rb_erase(&dr->rb, &trans->delayed_refs);
			kfree(dr);
		}
		spin_unlock(&trans->delayed_ref_lock);
	}
	INIT_LIST_HEAD(&trans->dirty_roots);
	INIT_LIST_HEAD(&trans->dirty_ebs);
	INIT_LIST_HEAD(&trans->retired_ebs);
	if (fs->sb) {
		struct vtxfs_sb_info *sbi = VTXFS_SB(fs->sb);
		if (sbi && sbi->fs)
			sbi->fs->read_only = 1;
	}
	up_write(&fs->commit_sem);
	return ret;
}

int vtxfs_end_trans(struct vtxfs_fs_info *fs, struct vtxfs_trans *trans)
{
	VTXFS_TRACE_ENTER();

	if (!refcount_dec_and_test(&trans->joiners))
		return 0;
	mutex_lock(&fs->trans_lock);
	if (fs->running == trans)
		fs->running = NULL;
	mutex_unlock(&fs->trans_lock);
	kfree(trans);
	return 0;
}
EXPORT_SYMBOL_GPL(vtxfs_run_delayed_refs);
