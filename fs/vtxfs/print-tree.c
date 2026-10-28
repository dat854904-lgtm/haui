// SPDX-License-Identifier: GPL-2.0-only
/*
 * print-tree.c — debugging dump of tree nodes (ratelimited printk).
 */
#include <linux/ratelimit.h>
#include <linux/rcupdate.h>

#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

static const char *type_name(u8 t)
{
	VTXFS_TRACE_ENTER();

	switch (t) {
	case VTX_TYPE_INODE:       return "INODE";
	case VTX_TYPE_INODE_REF:   return "INODE_REF";
	case VTX_TYPE_XATTR:       return "XATTR";
	case VTX_TYPE_DIR_ITEM:    return "DIR_ITEM";
	case VTX_TYPE_DIR_INDEX:   return "DIR_INDEX";
	case VTX_TYPE_EXTENT:      return "EXTENT";
	case VTX_TYPE_SUBVOL_ROOT: return "SUBVOL_ROOT";
	case VTX_TYPE_OMAP_VAL:    return "OMAP_VAL";
	case VTX_TYPE_REF:         return "REF";
	case VTX_TYPE_SFQ:         return "SFQ";
	case VTX_TYPE_REAPER_JOB:  return "REAPER_JOB";
	case VTX_TYPE_UUID_MAP:    return "UUID_MAP";
	default:                   return "UNKNOWN";
	}
}

void vtxfs_print_leaf(struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h = eb->hdr;
	u32 i, n = vtx_cpu16(h->nitems);

	pr_info_ratelimited("leaf oid=%llu xid=%llu vblk=%llu nitems=%u\n",
		vtx_cpu64(h->oid), vtx_cpu64(h->xid),
		vtx_cpu64(h->self_vblk), n);
	for (i = 0; i < n; i++) {
		const struct vtx_node_entry *e = &h->entries[i];
		u64 ot = vtx_cpu64(e->key.oid_and_type);

		pr_info_ratelimited("  [%u] oid=%llu type=%s off=%llu len=%u\n",
			i, VTX_KEY_GET_OID(ot),
			type_name(VTX_KEY_GET_TYPE(ot)),
			vtx_cpu64(e->key.offset),
			vtx_cpu16(e->slot.val_len));
	}
}

void vtxfs_print_node(struct vtxfs_extent_buffer *eb)
{
	VTXFS_TRACE_ENTER();

	const struct vtx_node_hdr *h = eb->hdr;
	u32 i, n = vtx_cpu16(h->nitems);

	if (h->level == 0) {
		vtxfs_print_leaf(eb);
		return;
	}
	pr_info_ratelimited("node oid=%llu lvl=%u xid=%llu nitems=%u\n",
		vtx_cpu64(h->oid), h->level, vtx_cpu64(h->xid), n);
	for (i = 0; i < n; i++) {
		const struct vtx_node_entry *e = &h->entries[i];
		struct vtx_key_ptr_val ptr;

		memcpy(&ptr, (u8 *)h + vtx_cpu16(e->slot.val_off8) * 8U,
		       sizeof(ptr));
		pr_info_ratelimited("  [%u] sep_oid=%llu child_oid=%llu hint_vblk=%llu\n",
			i, vtxfs_key_oid(&e->key),
			vtx_cpu64(ptr.child_oid), vtx_cpu64(ptr.hint_vblk));
	}
}

void vtxfs_print_root(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_extent_buffer *node;

	rcu_read_lock();
	node = rcu_dereference(root->node);
	if (node)
		vtxfs_print_node(node);
	rcu_read_unlock();
}
