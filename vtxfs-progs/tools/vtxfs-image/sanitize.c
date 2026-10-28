// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <string.h>
#include "common.h"
#include "sanitize.h"

static const char obf_chars[] =
	"abcdefghijklmnopqrstuvwxyz0123456789";

static void obfuscate_name(__u8 *name, __u16 len, __u64 seed)
{
	__u16 i;

	for (i = 0; i < len; i++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		name[i] = (__u8)obf_chars[(seed >> 33) % (sizeof(obf_chars)-1)];
	}
}

int sanitize_node(struct md_ctx *c, void *node, __u32 node_size)
{
	struct vtx_node_hdr *h = (struct vtx_node_hdr *)node;
	__u32 i, n;
	__u16 flags;

	if (!vtx_node_slots_valid(h, node_size))
		return -EUCLEAN;
	flags = vtx_cpu16(h->flags);
	if (!(flags & VTX_NODE_FLAG_LEAF))
		return 0; /* node internal không mang dữ liệu nhạy cảm */
	n = vtx_cpu16(h->nitems);
	for (i = 0; i < n; i++) {
		const struct vtx_node_entry *e = &h->entries[i];
		__u8 type = VTX_KEY_GET_TYPE(vtx_cpu64(e->key.oid_and_type));
		__u32 off = (__u32)vtx_cpu16(e->slot.val_off8) * 8U;
		__u16 len = vtx_cpu16(e->slot.val_len);
		__u8 *val = (__u8 *)node + off;

		if (type == VTX_TYPE_DIR_ITEM && len >= sizeof(struct vtx_dir_item)) {
			struct vtx_dir_item *d = (struct vtx_dir_item *)val;
			__u16 nlen = vtx_cpu16(d->name_len);
			__u64 h48; __u32 l32;

			if (sizeof(*d) + nlen > len || nlen == 0)
				continue;
			obfuscate_name(d->name, nlen,
				       vtx_cpu64(d->dir_seq) + i);
			/* Cập nhật low32 để tự nhất quán (fsck không kiểm tra). */
			vtx_dir_name_hash(c->fs->super_blk.hdr.dir_hash_seed,
					  d->name, nlen, &h48, &l32);
			d->name_hash_low32 = vtx_le32(l32);
		} else if (type == VTX_TYPE_EXTENT &&
			   len >= sizeof(struct vtx_extent_item)) {
			struct vtx_extent_item *ext = (struct vtx_extent_item *)val;

			if (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE) {
				__u32 pay = vtx_cpu32(ext->payload_bytes);

				if (sizeof(*ext) + pay <= len)
					memset(ext->tail, 0, pay);
			}
		} else if (type == VTX_TYPE_XATTR &&
			   len >= sizeof(struct vtx_xattr_item)) {
			struct vtx_xattr_item *x = (struct vtx_xattr_item *)val;
			__u16 nlen = vtx_cpu16(x->name_len);
			__u16 vlen = vtx_cpu16(x->value_len);

			if (sizeof(*x) + nlen + vlen <= len && vlen)
				memset(x->data + nlen, 0, vlen);
		}
	}
	/* Tính lại BLAKE3-128 để node hợp lệ sau sanitize. */
	return vtx_node_checksum_compute(node, node_size,
					 c->fs->super_blk.hdr.metadata_uuid);
}
