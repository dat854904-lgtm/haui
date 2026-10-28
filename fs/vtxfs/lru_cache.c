// SPDX-License-Identifier: GPL-2.0-only
/* Generic intrusive LRU used by the disk-io metadata block cache. */
#include <linux/list.h>
#include "vtxfs_drv.h"
#include "vtxfs_trace.h"

void vtxfs_lru_init(struct vtxfs_lru_cache *c, unsigned long max_items,
		    void (*free_entry)(struct vtxfs_lru_cache *,
				       struct vtxfs_lru_entry *))
{
	VTXFS_TRACE_ENTER();

	INIT_LIST_HEAD(&c->lru_list);
	c->nr_items = 0;
	c->max_items = max_items;
	c->free_entry = free_entry;
}

void vtxfs_lru_add(struct vtxfs_lru_cache *c, struct vtxfs_lru_entry *e)
{
	VTXFS_TRACE_ENTER();

	list_add(&e->lru, &c->lru_list);
	c->nr_items++;
	while (c->nr_items > c->max_items && !list_empty(&c->lru_list)) {
		struct vtxfs_lru_entry *victim =
			list_last_entry(&c->lru_list,
					struct vtxfs_lru_entry, lru);
		list_del_init(&victim->lru);
		c->nr_items--;
		c->free_entry(c, victim);
	}
}

void vtxfs_lru_remove(struct vtxfs_lru_cache *c, struct vtxfs_lru_entry *e)
{
	VTXFS_TRACE_ENTER();

	if (!list_empty(&e->lru)) {
		list_del_init(&e->lru);
		c->nr_items--;
	}
}

void vtxfs_lru_prune_all(struct vtxfs_lru_cache *c)
{
	VTXFS_TRACE_ENTER();

	while (!list_empty(&c->lru_list)) {
		struct vtxfs_lru_entry *victim =
			list_last_entry(&c->lru_list,
					struct vtxfs_lru_entry, lru);
		list_del_init(&victim->lru);
		c->nr_items--;
		c->free_entry(c, victim);
	}
}
