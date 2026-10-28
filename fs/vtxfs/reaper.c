// SPDX-License-Identifier: GPL-2.0-only
/*
 * reaper.c — background kernel thread that advances durable Reaper jobs and
 * promotes SFQ entries, so reuse progresses without userspace involvement.
 * Uses the engine's crash-resumable reaper/SFQ primitives.
 */
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "vtxfs_drv.h"
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

#define VTXFS_REAPER_INTERVAL_MS 5000UL
#define VTXFS_REAPER_STEPS 16U

struct vtxfs_reaper_ctx {
	struct vtxfs_fs_info *fs;
	struct task_struct *task;
	struct vtx_bdev *bdev; /* engine-backed handle for reaper */
	struct vtx_fs *engine;
};

static int reaper_thread(void *data)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_reaper_ctx *ctx = data;

	while (!kthread_should_stop()) {
		u32 steps = 0;
		int all_done = 0;

		if (ctx->engine) {
			struct vtxfs_sb_info *sbi = ctx->fs->sb ?
				VTXFS_SB(ctx->fs->sb) : NULL;

			if (sbi)
				vtxfs_op_begin(sbi);
			vtx_reaper_run_steps(ctx->engine,
					VTXFS_REAPER_STEPS, &steps, &all_done);
			if (all_done)
				vtx_sfq_promote(ctx->engine,
					ctx->engine->committed_xid, &steps);
			if (sbi)
				vtxfs_op_end(sbi);
		}
		msleep_interruptible(VTXFS_REAPER_INTERVAL_MS);
	}
	return 0;
}

int vtxfs_reaper_start(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_reaper_ctx *ctx;
	struct vtxfs_sb_info *sbi;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->fs = fs;
	sbi = fs->sb ? VTXFS_SB(fs->sb) : NULL;
	if (sbi) {
		ctx->bdev = &sbi->engine_bdev;
		ctx->engine = sbi->fs;
	}
	ctx->task = kthread_run(reaper_thread, ctx, "vtxfs-reaper");
	if (IS_ERR(ctx->task)) {
		int err = PTR_ERR(ctx->task);

		kfree(ctx);
		return err;
	}
	fs->reaper_priv = ctx;
	return 0;
}

void vtxfs_reaper_stop(struct vtxfs_fs_info *fs)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_reaper_ctx *ctx = fs->reaper_priv;

	if (!ctx || IS_ERR_OR_NULL(ctx->task))
		return;
	kthread_stop(ctx->task);
	kfree(ctx);
	fs->reaper_priv = NULL;
}
