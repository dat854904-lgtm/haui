// SPDX-License-Identifier: GPL-2.0-only
/*
 * VTXFS lock classes & op/commit transactions.
 * Order: VFS locks -> commit_sem -> op_lock (engine).
 */
#include <linux/export.h>
#include <linux/sched/mm.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

void vtxfs_lock_init(struct vtxfs_fs_info *fsi)
{
	VTXFS_TRACE_ENTER();

	init_rwsem(&fsi->locking.commit_sem);
	spin_lock_init(&fsi->locking.roots_lock);
}

/* Filesystem allocation under op_lock cannot recurse via filesystem reclaim.
 * op_lock serializes use of sbi->op_nofs by both write and commit paths.
 */
void vtxfs_op_begin(struct vtxfs_sb_info *sbi)
{
	VTXFS_TRACE_ENTER();

	down_read(&sbi->fs_info->locking.commit_sem);
	mutex_lock(&sbi->op_lock);
	sbi->op_nofs = memalloc_nofs_save();
}

void vtxfs_op_end(struct vtxfs_sb_info *sbi)
{
	VTXFS_TRACE_ENTER();

	/* Conservatively treat every engine operation as a mutation. Reads
	 * may cause extra checkpoints but can NEVER make fsync skip a write. */
	atomic64_inc(&sbi->op_seq);
	memalloc_nofs_restore(sbi->op_nofs);
	mutex_unlock(&sbi->op_lock);
	up_read(&sbi->fs_info->locking.commit_sem);
}

/* Checkpoint / freeze / remount-ro: exclusive commit quiesce. */
int vtxfs_commit_begin(struct vtxfs_sb_info *sbi)
{
	VTXFS_TRACE_ENTER();

	down_write(&sbi->fs_info->locking.commit_sem);
	mutex_lock(&sbi->op_lock);
	sbi->op_nofs = memalloc_nofs_save();
	return 0;
}

void vtxfs_commit_end(struct vtxfs_sb_info *sbi)
{
	VTXFS_TRACE_ENTER();

	memalloc_nofs_restore(sbi->op_nofs);
	mutex_unlock(&sbi->op_lock);
	up_write(&sbi->fs_info->locking.commit_sem);
}
