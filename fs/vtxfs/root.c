// SPDX-License-Identifier: GPL-2.0-only
/* root.c — per-subvolume root object cache (xarray + refcount + RCU). */
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

static void vtxfs_root_free_rcu(struct rcu_head *rcu)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *root = container_of(rcu, struct vtxfs_root, rcu);

	xa_destroy(&root->inodes);
	xa_destroy(&root->delayed_nodes);
	kfree(root);
}

int vtxfs_root_register(struct vtxfs_sb_info *sbi, u64 subvol_id,
			const struct vtx_subvol_root_item *item)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fsi = sbi->fs_info;
	struct vtxfs_root *root, *old;

	root = kzalloc(sizeof(*root), GFP_KERNEL);
	if (!root)
		return -ENOMEM;
	root->fs = fsi;
	root->fs_info = fsi;
	root->tree_type = VTX_TREE_FS;
	root->subvol_id = subvol_id;
	root->root_oid = vtx_cpu64(item->root_node_oid);
	root->item = *item;
	root->on_disk = *item;
	mutex_init(&root->op_mutex);
	xa_init(&root->inodes);
	xa_init(&root->delayed_nodes);
	INIT_LIST_HEAD(&root->dirty_link);
	refcount_set(&root->refs, 1);

	old = xa_store(&fsi->roots, subvol_id, root, GFP_KERNEL);
	if (xa_is_err(old)) {
		xa_destroy(&root->inodes);
		xa_destroy(&root->delayed_nodes);
		kfree(root);
		return xa_err(old);
	}
	if (old)
		vtxfs_root_put(old);
	return 0;
}

struct vtxfs_root *vtxfs_root_get(struct super_block *sb, u64 subvol_id)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *root;

	rcu_read_lock();
	root = xa_load(&VTXFS_FSI(sb)->roots, subvol_id);
	if (root && !refcount_inc_not_zero(&root->refs))
		root = NULL;
	rcu_read_unlock();
	return root;
}

void vtxfs_root_put(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	if (refcount_dec_and_test(&root->refs))
		call_rcu(&root->rcu, vtxfs_root_free_rcu);
}

void vtxfs_roots_destroy(struct super_block *sb)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fsi = VTXFS_FSI(sb);
	struct vtxfs_root *root;
	unsigned long id;

	if (!fsi)
		return;
	xa_for_each(&fsi->roots, id, root)
		vtxfs_root_put(root);
	xa_destroy(&fsi->roots);
}
