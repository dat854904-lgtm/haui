// SPDX-License-Identifier: GPL-2.0-only
/* ctree_compat.c — đảm bảo node kernel ghi ra khớp engine fsck. */
#include <linux/slab.h>
#include <linux/rbtree.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

/* Bật/tắt cờ ROOT (engine fsck đòi đúng 1 node ROOT mỗi gốc cây). */
void vtxfs_ctree_set_root_flag(struct vtxfs_extent_buffer *eb, int on)
{
	VTXFS_TRACE_ENTER();

	u16 f = vtx_cpu16(eb->hdr->flags);

	if (on)
		f |= VTX_NODE_FLAG_ROOT;
	else
		f &= (__u16)~VTX_NODE_FLAG_ROOT;
	eb->hdr->flags = cpu_to_le16(f);
	memset(eb->hdr->checksum, 0, sizeof(eb->hdr->checksum));
	vtxfs_mark_eb_dirty(eb);
}

/* Ghi delayed-ref vào transaction đang chạy (drain ở commit, tránh đệ quy
 * khi đang đột biến cây). +delta=1 khi cấp phát, -1 khi thu hồi. */
int vtxfs_ctree_delayed_ref(struct vtxfs_fs_info *fs, u64 alloc_id,
			    u64 vblk, u32 blocks, u32 flags, s64 delta)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_trans *trans = fs->running;
	struct vtxfs_delayed_ref *dr;
	struct rb_node **p, *parent = NULL;
	unsigned long fl;

	if (!trans)
		return -EINVAL;
	dr = kzalloc(sizeof(*dr), GFP_NOFS);
	if (!dr)
		return -ENOMEM;
	dr->allocation_id = alloc_id;
	dr->start_vblk = vblk;
	dr->blocks = blocks;
	dr->flags = flags;
	dr->delta = delta;

	spin_lock_irqsave(&trans->delayed_ref_lock, fl);
	p = &trans->delayed_refs.rb_node;
	while (*p) {
		struct vtxfs_delayed_ref *cur =
			rb_entry(*p, struct vtxfs_delayed_ref, rb);

		parent = *p;
		if (vblk < cur->start_vblk)
			p = &parent->rb_left;
		else if (vblk > cur->start_vblk)
			p = &parent->rb_right;
		else {
			cur->delta += delta;	/* gộp cùng vblk */
			spin_unlock_irqrestore(&trans->delayed_ref_lock, fl);
			kfree(dr);
			return 0;
		}
	}
	rb_link_node(&dr->rb, parent, p);
	rb_insert_color(&dr->rb, &trans->delayed_refs);
	spin_unlock_irqrestore(&trans->delayed_ref_lock, fl);
	return 0;
}
