// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "sanitize.h"

/* Ghi một block, nén LZSS nếu bật --compress. */
static int emit_block(struct md_ctx *c, __u64 pblk, const void *data)
{
	struct vtx_md_rec rec;
	const void *out = data;
	__u32 len = VTXFS_BLOCK_SIZE;
	__u8 *cbuf = NULL;

	if (c->compress) {
		size_t cap = VTXFS_BLOCK_SIZE + VTXFS_BLOCK_SIZE / 8U + 64U;
		long csz;

		cbuf = malloc(cap);
		if (!cbuf)
			return -ENOMEM;
		csz = vtxmd_lz_compress(data, VTXFS_BLOCK_SIZE, cbuf, cap);
		if (csz < 0) {
			free(cbuf);
			return -EIO;
		}
		out = cbuf;
		len = (__u32)csz;
	}
	rec.pblk = vtx_le64(pblk);
	rec.len = vtx_le32(len);
	rec.reserved = 0;
	if (fwrite(&rec, sizeof(rec), 1, c->out) != 1 ||
	    fwrite(out, 1, len, c->out) != len) {
		free(cbuf);
		return -EIO;
	}
	free(cbuf);
	c->block_count++;
	return 0;
}

/* Dump VAG directory của checkpoint đã commit (root + leaf + bitmaps). */
static int emit_vag_dir(struct md_ctx *c)
{
	struct vtx_fs *fs = c->fs;
	__u64 root_pblk = vtx_cpu64(fs->cp_blk.hdr.vag_dir_root.pblk);
	__u64 leaf_pblk = vtx_cpu64(fs->vag.root.entries[0].start_pblk);
	__u8 *blk = malloc(VTXFS_BLOCK_SIZE);
	__u32 i;
	int ret = 0;

	if (!blk)
		return -ENOMEM;
	ret = c->bdev.ops->read_blocks(&c->bdev, root_pblk, 1, blk);
	if (!ret)
		ret = emit_block(c, root_pblk, blk);
	for (i = 0; i < 16U && !ret; i++) {
		ret = c->bdev.ops->read_blocks(&c->bdev, leaf_pblk + i, 1, blk);
		if (!ret)
			ret = emit_block(c, leaf_pblk + i, blk);
	}
	for (i = 0; i < fs->vag.total_vags && !ret; i++) {
		__u64 bm = vtx_cpu64(fs->vag.descs[i].bitmap_pblk);

		ret = c->bdev.ops->read_blocks(&c->bdev, bm, 1, blk);
		if (!ret)
			ret = emit_block(c, bm, blk);
	}
	free(blk);
	return ret;
}

/* Dump toàn bộ bootstrap: superblock ring, dev table, CPD ring (primary+mirror). */
static int emit_bootstrap(struct md_ctx *c)
{
	__u8 *blk = malloc(VTXFS_BLOCK_SIZE);
	__u64 p;
	int ret = 0;

	if (!blk)
		return -ENOMEM;
	for (p = 0; p < VTXFS_SUPER_RING_SLOTS && !ret; p++) {
		__u64 a = VTXFS_SUPER_RING_PBLK + p;
		__u64 b = VTXFS_BOOT_MIRROR0_PBLK + a;

		ret = c->bdev.ops->read_blocks(&c->bdev, a, 1, blk);
		if (!ret) ret = emit_block(c, a, blk);
		if (!ret) ret = c->bdev.ops->read_blocks(&c->bdev, b, 1, blk);
		if (!ret) ret = emit_block(c, b, blk);
	}
	for (p = 0; p < VTXFS_DEV_TABLE_BLOCKS && !ret; p++) {
		__u64 a = VTXFS_DEV_TABLE_PBLK + p;
		__u64 b = VTXFS_BOOT_MIRROR0_PBLK + a;

		ret = c->bdev.ops->read_blocks(&c->bdev, a, 1, blk);
		if (!ret) ret = emit_block(c, a, blk);
		if (!ret) ret = c->bdev.ops->read_blocks(&c->bdev, b, 1, blk);
		if (!ret) ret = emit_block(c, b, blk);
	}
	for (p = 0; p < VTXFS_CPD_RING_SLOTS && !ret; p++) {
		__u64 a = VTXFS_CPD_RING_PBLK + p;
		__u64 b = VTXFS_BOOT_MIRROR0_PBLK + a;

		ret = c->bdev.ops->read_blocks(&c->bdev, a, 1, blk);
		if (!ret) ret = emit_block(c, a, blk);
		if (!ret) ret = c->bdev.ops->read_blocks(&c->bdev, b, 1, blk);
		if (!ret) ret = emit_block(c, b, blk);
	}
	if (!ret)
		ret = emit_vag_dir(c);
	free(blk);
	return ret;
}

/* Đệ quy dump mọi node của một cây theo pblk. */
static int dump_tree_nodes(struct md_ctx *c, const struct vtx_rw_tree *t,
			   __u64 oid, __u64 vblk, int level)
{
	const void *node = NULL;
	size_t bytes = 0;
	int verified = 0, ret;
	__u64 pblk;

	(void)oid;
	ret = t->ops->read_node(t->ctx, vblk, &node, &bytes, &verified);
	if (ret)
		return ret;
	if (!verified || bytes != t->node_bytes) {
		t->ops->release_node(t->ctx, node);
		return -EUCLEAN;
	}
	ret = vtx_vag_vblk_to_pblk(&c->fs->vag, vblk, &pblk);
	if (!ret) {
		if (c->sanitize) {
			__u8 *tmp = malloc(t->node_bytes);

			if (!tmp)
				ret = -ENOMEM;
			else {
				memcpy(tmp, node, t->node_bytes);
				ret = sanitize_node(c, tmp, t->node_bytes);
				if (!ret)
					ret = emit_block(c, pblk, tmp);
				free(tmp);
			}
		} else {
			ret = emit_block(c, pblk, node);
		}
	}
	if (ret || level == 0) {
		t->ops->release_node(t->ctx, node);
		return ret;
	}
	{
		__u8 *copy = malloc(t->node_bytes);
		const struct vtx_node_hdr *h;
		__u32 n, i;

		if (!copy) {
			t->ops->release_node(t->ctx, node);
			return -ENOMEM;
		}
		memcpy(copy, node, t->node_bytes);
		t->ops->release_node(t->ctx, node);
		h = (const struct vtx_node_hdr *)copy;
		n = vtx_cpu16(h->nitems);
		for (i = 0; i < n && !ret; i++) {
			const struct vtx_node_entry *e = &h->entries[i];
			struct vtx_key_ptr_val ptr;
			__u64 c_oid, c_vblk = 0;

			memcpy(&ptr, copy +
			       (__u32)vtx_cpu16(e->slot.val_off8) * 8U,
			       sizeof(ptr));
			c_oid = vtx_cpu64(ptr.child_oid);
			if (t->is_physical) {
				c_vblk = vtx_cpu64(ptr.hint_vblk);
			} else {
				ret = t->ops->omap_resolve(t->ctx, t->view_id,
							   c_oid, &c_vblk);
				if (ret)
					break;
			}
			ret = dump_tree_nodes(c, t, c_oid, c_vblk, level - 1);
		}
		free(copy);
	}
	return ret;
}

/* Dump mọi block dữ liệu thông qua VTX_TREE_REF (khử trùng lặp tự nhiên). */
static int dump_data_blocks(struct md_ctx *c)
{
	struct vtx_fs *fs = c->fs;
	struct vtx_key cur = {0}, found;
	__u8 val[64], *blk;
	size_t bytes;
	int first = 1, ret = 0;

	blk = malloc(VTXFS_BLOCK_SIZE);
	if (!blk)
		return -ENOMEM;
	for (;;) {
		const struct vtx_ref_item *r;
		__u32 bc, b;
		__u64 sv;

		bytes = sizeof(val);
		ret = vtx_rw_next(&fs->ref_tree, &cur, first, &found,
				  val, &bytes);
		if (ret == -ENOENT) {
			ret = 0;
			break;
		}
		if (ret)
			break;
		first = 0;
		cur = found;
		if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_REF || bytes != sizeof(*r))
			continue;
		r = (const struct vtx_ref_item *)val;
		if (!(vtx_cpu32(r->flags) & VTX_REF_FLAG_DATA))
			continue;
		sv = vtx_cpu64(r->start_vblk);
		bc = vtx_cpu32(r->block_count);
		for (b = 0; b < bc && !ret; b++) {
			__u64 pblk;

			ret = vtx_vag_vblk_to_pblk(&fs->vag, sv + b, &pblk);
			if (!ret)
				ret = c->bdev.ops->read_blocks(&c->bdev,
							       pblk, 1, blk);
			if (!ret)
				ret = emit_block(c, pblk, blk);
		}
		if (ret)
			break;
	}
	free(blk);
	return ret;
}

int image_create_main(int argc, char **argv)
{
	struct md_ctx c;
	struct vtx_metadump_hdr hdr;
	const char *img = NULL, *out = NULL;
	__u64 sub_ids[128];
	__u32 nsub = 0;
	int i, rc;

	memset(&c, 0, sizeof(c));
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 ||
		    strcmp(argv[i], "--sanitize") == 0)
			c.sanitize = 1;
		else if (strcmp(argv[i], "-c") == 0 ||
			 strcmp(argv[i], "--compress") == 0)
			c.compress = 1;
		else if (strcmp(argv[i], "-f") == 0 ||
			 strcmp(argv[i], "--full") == 0)
			c.full = 1;
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			out = argv[++i];
		else if (!img)
			img = argv[i];
	}
	if (!img || !out) {
		fprintf(stderr,
			"usage: vtxfs-image create <image> -o <dump> [--sanitize] [--compress] [--full]\n");
		return 1;
	}
	rc = md_open_fs(&c, img, 0);
	if (rc) {
		fprintf(stderr, "vtxfs-image: không mở được '%s': %s\n",
			img, strerror(-rc));
		return 1;
	}
	c.out = fopen(out, "wb");
	if (!c.out) {
		fprintf(stderr, "vtxfs-image: không tạo được '%s'\n", out);
		md_close_fs(&c);
		return 1;
	}

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, VTXFS_MD_MAGIC, VTXFS_MD_MAGIC_LEN);
	hdr.version = vtx_le32(VTXFS_MD_VERSION);
	{
		__u32 flags = 0;
		if (c.sanitize) flags |= VTXFS_MD_FLAG_SANITIZED;
		if (c.compress) flags |= VTXFS_MD_FLAG_COMPRESSED;
		if (c.full)     flags |= VTXFS_MD_FLAG_WITH_DATA;
		hdr.flags = vtx_le32(flags);
	}
	memcpy(hdr.fsid, c.fs->super_blk.hdr.fsid_uuid, VTXFS_UUID_SIZE);
	hdr.node_size = vtx_le32(VTXFS_BLOCK_SIZE);
	hdr.total_blocks = c.fs->super_blk.hdr.total_blocks;
	hdr.committed_xid = vtx_le64(c.fs->committed_xid);
	hdr.block_count = 0; /* cập nhật sau */
	if (fwrite(&hdr, sizeof(hdr), 1, c.out) != 1) {
		rc = -EIO;
		goto out;
	}

	rc = emit_bootstrap(&c);
	if (rc)
		goto out;

	/* 6 cây hệ thống. */
	{
		const struct vtx_rw_tree *sys[6] = {
			&c.fs->omap_meta_tree, &c.fs->root_tree,
			&c.fs->ref_tree, &c.fs->reaper_tree,
			&c.fs->sfq_tree, &c.fs->uuid_tree
		};
		for (i = 0; i < 6 && !rc; i++)
			rc = dump_tree_nodes(&c, sys[i], sys[i]->root_oid,
					     sys[i]->root_vblk, sys[i]->root_level);
	}
	if (rc)
		goto out;

	/* Liệt kê subvolume rồi dump cây FS + OMAP_VIEW của từng cái. */
	{
		struct vtx_key cur = {0}, found;
		struct vtx_subvol_root_item item;
		size_t bytes;
		int first = 1;

		for (;;) {
			bytes = sizeof(item);
			rc = vtx_rw_next(&c.fs->root_tree, &cur, first,
					 &found, &item, &bytes);
			if (rc == -ENOENT) { rc = 0; break; }
			if (rc) break;
			first = 0;
			cur = found;
			if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) ==
			    VTX_TYPE_SUBVOL_ROOT && nsub < 128)
				sub_ids[nsub++] =
					VTX_KEY_GET_OID(vtx_cpu64(found.oid_and_type));
		}
	}
	for (i = 0; (__u32)i < nsub && !rc; i++) {
		rc = vtx_subvol_switch_view(c.fs, sub_ids[i]);
		if (rc)
			break;
		rc = dump_tree_nodes(&c, &c.fs->active_omap_view,
				     c.fs->active_omap_view.root_oid,
				     c.fs->active_omap_view.root_vblk,
				     c.fs->active_omap_view.root_level);
		if (rc)
			break;
		rc = dump_tree_nodes(&c, &c.fs->active_fs_tree,
				     c.fs->active_fs_tree.root_oid,
				     c.fs->active_fs_tree.root_vblk,
				     c.fs->active_fs_tree.root_level);
	}
	if (rc)
		goto out;

	/* Nếu có --full: dump toàn bộ data blocks */
	if (c.full) {
		rc = dump_data_blocks(&c);
		if (rc)
			goto out;
	}

	/* Cập nhật block_count trong header. */
	hdr.block_count = vtx_le64(c.block_count);
	fseek(c.out, 0, SEEK_SET);
	if (fwrite(&hdr, sizeof(hdr), 1, c.out) != 1)
		rc = -EIO;

out:
	printf("vtxfs-image: create %s -> %llu block%s%s%s\n",
	       rc ? "THẤT BẠI" : "OK",
	       (unsigned long long)c.block_count,
	       c.sanitize ? " (đã sanitize)" : "",
	       c.compress ? " (đã nén LZSS)" : "",
	       c.full ? " (kèm data)" : "");
	md_close_fs(&c);
	return rc ? 1 : 0;
}
