// SPDX-License-Identifier: GPL-2.0-only
/*
 * delayed-inode.c — batch directory-item / inode-ref mutations per inode in
 * memory, then flush them into the tree as one CoW update. Reduces tree CoW
 * amplification for heavy directory workloads.
 */
#include <linux/slab.h>

#include "vtxfs_subsys.h"
#include "vtxfs_leaf.h"
#include "vtxfs_trace.h"

static struct vtxfs_delayed_item *item_alloc(const struct vtx_key *key,
		enum vtxfs_delayed_type type, const void *data, u32 len)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_delayed_item *it;

	it = kzalloc(sizeof(*it) + len, GFP_NOFS);
	if (!it)
		return NULL;
	refcount_set(&it->refs, 1);
	it->key = *key;
	it->type = type;
	it->data_len = len;
	if (len && data)
		memcpy(it->data, data, len);
	return it;
}

static int tree_insert(struct rb_root *root, struct vtxfs_delayed_item *it)
{
	VTXFS_TRACE_ENTER();

	struct rb_node **p = &root->rb_node, *parent = NULL;

	while (*p) {
		struct vtxfs_delayed_item *cur =
			rb_entry(*p, struct vtxfs_delayed_item, rb);
		int cmp = vtx_key_cmp(&it->key, &cur->key);

		parent = *p;
		if (cmp < 0)
			p = &parent->rb_left;
		else if (cmp > 0)
			p = &parent->rb_right;
		else
			return -EEXIST;
	}
	rb_link_node(&it->rb, parent, p);
	rb_insert_color(&it->rb, root);
	return 0;
}

struct vtxfs_delayed_node *vtxfs_delayed_node_get(struct vtxfs_root *root,
						  u64 ino)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_delayed_node *node;
	void *old;

	node = xa_load(&root->delayed_nodes, ino);
	if (node) {
		refcount_inc(&node->refs);
		return node;
	}
	node = kzalloc(sizeof(*node), GFP_NOFS);
	if (!node)
		return ERR_PTR(-ENOMEM);
	node->root = root;
	node->inode_id = ino;
	refcount_set(&node->refs, 1);
	mutex_init(&node->mutex);
	node->insertions = RB_ROOT;
	node->deletions = RB_ROOT;
	old = xa_store(&root->delayed_nodes, ino, node, GFP_NOFS);
	if (xa_is_err(old)) {
		kfree(node);
		return ERR_PTR(xa_err(old));
	}
	refcount_inc(&node->refs); /* xarray holds one ref */
	return node;
}

void vtxfs_delayed_node_put(struct vtxfs_delayed_node *node)
{
	VTXFS_TRACE_ENTER();

	if (refcount_dec_and_test(&node->refs))
		kfree(node);
}

int vtxfs_delayed_insert_dir_item(struct vtxfs_delayed_node *node,
				  const struct vtx_key *key,
				  const void *data, u32 len)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_delayed_item *it;
	int ret;

	it = item_alloc(key, VTXFS_DELAYED_INSERT, data, len);
	if (!it)
		return -ENOMEM;
	mutex_lock(&node->mutex);
	ret = tree_insert(&node->insertions, it);
	if (!ret)
		node->insert_count++;
	mutex_unlock(&node->mutex);
	if (ret)
		kfree(it);
	return ret;
}

int vtxfs_delayed_delete_dir_item(struct vtxfs_delayed_node *node,
				  const struct vtx_key *key)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_delayed_item *it;
	int ret;

	it = item_alloc(key, VTXFS_DELAYED_DELETE, NULL, 0);
	if (!it)
		return -ENOMEM;
	mutex_lock(&node->mutex);
	ret = tree_insert(&node->deletions, it);
	if (!ret)
		node->delete_count++;
	mutex_unlock(&node->mutex);
	if (ret)
		kfree(it);
	return ret;
}

/* Apply all queued mutations of one node into the tree via ctree ops. */
int vtxfs_delayed_flush_node(struct vtxfs_delayed_node *node)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_root *root = node->root;
	struct vtxfs_path *path;
	struct rb_node *n;
	int ret = 0;

	path = vtxfs_alloc_path();
	if (!path)
		return -ENOMEM;

	mutex_lock(&node->mutex);
	/* deletions first, then insertions to keep ordering stable */
	while ((n = rb_first(&node->deletions)) != NULL) {
		struct vtxfs_delayed_item *it =
			rb_entry(n, struct vtxfs_delayed_item, rb);

		rb_erase(&it->rb, &node->deletions);
		mutex_unlock(&node->mutex);
		ret = vtxfs_search_slot(root, path, &it->key, 0, 1);
		if (ret == 0)
			ret = vtxfs_del_item(root, path);
		vtxfs_release_path(path);
		kfree(it);
		if (ret && ret != -ENOENT)
			goto out;
		mutex_lock(&node->mutex);
	}
	while ((n = rb_first(&node->insertions)) != NULL) {
		struct vtxfs_delayed_item *it =
			rb_entry(n, struct vtxfs_delayed_item, rb);
		void *w = NULL;

		rb_erase(&it->rb, &node->insertions);
		mutex_unlock(&node->mutex);
		ret = vtxfs_search_slot(root, path, &it->key,
					(int)it->data_len, 1);
		if (ret == 1) {
			ret = vtxfs_insert_item(root, path, &it->key,
						it->data_len, &w);
			if (!ret && it->data_len && w)
				memcpy(w, it->data, it->data_len);
		} else if (ret == 0) {
			ret = -EEXIST;
		}
		vtxfs_release_path(path);
		kfree(it);
		if (ret)
			goto out;
		mutex_lock(&node->mutex);
	}
	node->insert_count = node->delete_count = 0;
	mutex_unlock(&node->mutex);
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_delayed_flush_all(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_delayed_node *node;
	unsigned long ino;
	int ret = 0;

	xa_for_each(&root->delayed_nodes, ino, node) {
		ret = vtxfs_delayed_flush_node(node);
		if (ret)
			break;
	}
	return ret;
}
