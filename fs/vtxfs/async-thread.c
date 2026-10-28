// SPDX-License-Identifier: GPL-2.0-only
/*
 * async-thread.c — workqueue infrastructure for async delalloc/COW.
 * Lets multiple worker threads run run_delalloc_range concurrently for
 * different inodes while the engine/ctree layer serializes tree mutation.
 */
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

struct vtxfs_async_work {
	struct work_struct work;
	struct vtxfs_fs_info *fs;
	struct inode *inode;
	u64 start, end;
};

static void async_cow_worker(struct work_struct *w)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_async_work *aw =
		container_of(w, struct vtxfs_async_work, work);

	vtxfs_run_delalloc_range(aw->inode, aw->start, aw->end);
	iput(aw->inode);
	kfree(aw);
}

int vtxfs_async_init(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	if (fs->async_cow_wq)
		return 0;
	fs->async_cow_wq = alloc_workqueue("vtxfs-async-cow",
			WQ_UNBOUND | WQ_HIGHPRI, 0);
	return fs->async_cow_wq ? 0 : -ENOMEM;
}

void vtxfs_async_stop(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	if (!fs->async_cow_wq)
		return;
	destroy_workqueue(fs->async_cow_wq);
	fs->async_cow_wq = NULL;
}

int vtxfs_submit_async_cow(struct vtxfs_fs_info *fs, struct inode *inode,
			   u64 start, u64 end)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_async_work *aw;

	if (!fs->async_cow_wq)
		return vtxfs_run_delalloc_range(inode, start, end);

	aw = kzalloc(sizeof(*aw), GFP_NOFS);
	if (!aw)
		return -ENOMEM;
	aw->fs = fs;
	aw->inode = igrab(inode);
	if (!aw->inode) {
		kfree(aw);
		return -ENOENT;
	}
	aw->start = start;
	aw->end = end;
	INIT_WORK(&aw->work, async_cow_worker);
	queue_work(fs->async_cow_wq, &aw->work);
	return 0;
}
