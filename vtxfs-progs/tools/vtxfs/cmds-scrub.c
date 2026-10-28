// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#define _GNU_SOURCE
/*
 * cmds-scrub.c — quét xác minh: metadata (BLAKE3-128 qua read_node) + dữ liệu
 * (CRC32C từng block 4 KiB). Tiến trình được theo dõi bằng vtx_scrub_state
 * (last_checked_vblk, checkpoint_xid, verified_blocks, error_count, flags).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "commands.h"
#include "common.h"

struct scrub_ctx {
	struct vtx_fs *fs;
	struct vtx_scrub_state state;
	__u64 meta_nodes;
	__u64 data_extents;
};

static void state_inc_verified(struct vtx_scrub_state *s)
{
	s->verified_blocks = vtx_le64(vtx_cpu64(s->verified_blocks) + 1);
}

static void state_add_error(struct vtx_scrub_state *s)
{
	s->error_count = vtx_le32(vtx_cpu32(s->error_count) + 1);
}

/* Đệ quy đọc + xác minh mọi node của một cây (BLAKE3 được kiểm tra trong
 * read_node; sai -> -EUCLEAN). */
static int scrub_walk_node(struct scrub_ctx *sc, const struct vtx_rw_tree *t,
			   __u64 oid, __u64 vblk, int level, int is_root)
{
	const void *node = NULL;
	const struct vtx_node_hdr *h;
	size_t bytes = 0;
	int verified = 0, ret;
	__u32 i, n;

	(void)oid;
	(void)is_root;
	ret = t->ops->read_node(t->ctx, vblk, &node, &bytes, &verified);
	if (ret || !verified || bytes != t->node_bytes) {
		if (node)
			t->ops->release_node(t->ctx, node);
		state_add_error(&sc->state);
		return ret ? ret : -EUCLEAN;
	}
	h = (const struct vtx_node_hdr *)node;
	n = vtx_cpu16(h->nitems);
	state_inc_verified(&sc->state);
	sc->meta_nodes++;
	sc->state.last_checked_vblk = vtx_le64(vblk);

	if (level == 0) {
		t->ops->release_node(t->ctx, node);
		return 0;
	}
	{
		__u8 *copy = (__u8 *)malloc(t->node_bytes);

		if (!copy) {
			t->ops->release_node(t->ctx, node);
			return -ENOMEM;
		}
		memcpy(copy, node, t->node_bytes);
		t->ops->release_node(t->ctx, node);
		h = (const struct vtx_node_hdr *)copy;
		for (i = 0; i < n; i++) {
			const struct vtx_node_entry *e = &h->entries[i];
			struct vtx_key_ptr_val ptr;
			__u64 c_oid, c_vblk = 0;

			memcpy(&ptr, copy +
			       (size_t)vtx_cpu16(e->slot.val_off8) * 8U,
			       sizeof(ptr));
			c_oid = vtx_cpu64(ptr.child_oid);
			if (t->is_physical) {
				c_vblk = vtx_cpu64(ptr.hint_vblk);
			} else {
				ret = t->ops->omap_resolve(t->ctx, t->view_id,
							   c_oid, &c_vblk);
				if (ret) {
					free(copy);
					state_add_error(&sc->state);
					return ret;
				}
			}
			ret = scrub_walk_node(sc, t, c_oid, c_vblk,
					      level - 1, 0);
			if (ret) {
				free(copy);
				return ret;
			}
		}
		free(copy);
	}
	return 0;
}

/* Xác minh CRC32C mọi block dữ liệu của subvolume đang active. */
static int scrub_data_subvol(struct scrub_ctx *sc)
{
	struct vtx_fs *fs = sc->fs;
	struct vtx_key cur = {0}, found;
	__u8 *ebuf, *blk;
	size_t bytes;
	int first = 1, ret;

	ebuf = (__u8 *)malloc(VTXFS_BLOCK_SIZE);
	blk = (__u8 *)malloc(VTXFS_BLOCK_SIZE);
	if (!ebuf || !blk) {
		free(ebuf);
		free(blk);
		return -ENOMEM;
	}
	cur.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(1, 0));
	cur.offset = 0;
	for (;;) {
		const struct vtx_extent_item *ext;
		__u32 nb, b;

		bytes = VTXFS_BLOCK_SIZE;
		ret = vtx_rw_next(&fs->active_fs_tree, &cur, first,
				  &found, ebuf, &bytes);
		if (ret == -ENOENT) {
			ret = 0;
			break;
		}
		if (ret)
			break;
		first = 0;
		cur = found;
		if (VTX_KEY_GET_TYPE(vtx_cpu64(found.oid_and_type)) !=
		    VTX_TYPE_EXTENT)
			continue;
		ext = (const struct vtx_extent_item *)ebuf;
		if (!vtx_extent_p0_value_valid(ext, bytes)) {
			state_add_error(&sc->state);
			continue;
		}
		if (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)
			continue;
		nb = vtx_cpu32(ext->physical_blocks);
		sc->data_extents++;
		{
			const __le32 *stored = (const __le32 *)ext->tail;

			for (b = 0; b < nb; b++) {
				__u64 pblk;
				__u32 actual;

				ret = vtx_vag_vblk_to_pblk(&fs->vag,
						vtx_cpu64(ext->vblk) + b,
						&pblk);
				if (ret) {
					state_add_error(&sc->state);
					break;
				}
				ret = fs->bdev->ops->read_blocks(fs->bdev,
						pblk, 1, blk);
				if (ret) {
					state_add_error(&sc->state);
					break;
				}
				actual = vtx_crc32c(blk, VTXFS_BLOCK_SIZE);
				if (actual != vtx_cpu32(stored[b]))
					state_add_error(&sc->state);
				else
					state_inc_verified(&sc->state);
				sc->state.last_checked_vblk =
					vtx_le64(vtx_cpu64(ext->vblk) + b);
			}
		}
	}
	free(ebuf);
	free(blk);
	return ret;
}

struct collect_ctx {
	__u64 *ids;
	int n, cap;
};

static int collect_subvol_cb(const struct vtx_subvol_root_item *it, __u64 id,
			     void *arg)
{
	struct collect_ctx *c = (struct collect_ctx *)arg;

	(void)it;
	if (c->n < c->cap)
		c->ids[c->n++] = id;
	return 0;
}

static int cmd_scrub_start(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct scrub_ctx sc;
	__u64 sub_ids[128];
	struct collect_ctx cc = { sub_ids, 0, 128 };
	const struct vtx_rw_tree *sys[6];
	int verify_only = 0, rc, i;

	const char *target_path = NULL;
	(void)c;
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--verify-only") == 0)
			verify_only = 1;
		else if (!target_path)
			target_path = argv[i];
	}
	if (!target_path) {
		fprintf(stderr,
			"usage: vtxfs scrub start <image|mountpoint> [--verify-only]\n");
		return 1;
	}

	{
		struct stat st;
		if (stat(target_path, &st) == 0 && S_ISDIR(st.st_mode)) {
			int dirfd = open(target_path, O_RDONLY | O_DIRECTORY);
			struct vtx_ioc_scrub_args scrub_req;
			if (dirfd < 0) {
				fprintf(stderr, "vtxfs scrub: failed to open '%s': %s\n",
					target_path, strerror(errno));
				return 1;
			}
			memset(&scrub_req, 0, sizeof(scrub_req));
			scrub_req.struct_size = sizeof(scrub_req);
			scrub_req.flags = verify_only ? VTX_SCRUB_VERIFY_ONLY : 0;
			if (ioctl(dirfd, VTX_IOC_SCRUB, &scrub_req) != 0) {
				fprintf(stderr, "vtxfs scrub: ioctl VTX_IOC_SCRUB failed: %s\n",
					strerror(errno));
				close(dirfd);
				return 1;
			}
			close(dirfd);
			printf("Scrub (live kernel ioctl) hoàn tất:\n");
			printf("  flags             : 0x%x%s\n",
			       scrub_req.flags, verify_only ? " (VERIFY_ONLY)" : "");
			printf("  data_blocks       : %llu\n",
			       (unsigned long long)scrub_req.data_blocks_scrubbed);
			printf("  meta_blocks       : %llu\n",
			       (unsigned long long)scrub_req.meta_blocks_scrubbed);
			printf("  checksum_errors   : %llu\n",
			       (unsigned long long)scrub_req.checksum_errors);
			printf("  repaired_blocks   : %llu\n",
			       (unsigned long long)scrub_req.repaired_meta_blocks);
			printf("  uncorrectable     : %llu\n",
			       (unsigned long long)scrub_req.uncorrectable_errors);
			return (scrub_req.checksum_errors == 0) ? 0 : 1;
		}
	}

	rc = vtxfs_cli_open(&h, target_path, 0);
	if (rc)
		return 1;

	memset(&sc, 0, sizeof(sc));
	sc.fs = h.fs;
	sc.state.checkpoint_xid = vtx_le64(h.fs->committed_xid);
	sc.state.flags = vtx_le32(verify_only ? VTX_SCRUB_VERIFY_ONLY : 0U);

	/* 1. Quét 6 cây hệ thống. */
	sys[0] = &h.fs->omap_meta_tree;
	sys[1] = &h.fs->root_tree;
	sys[2] = &h.fs->ref_tree;
	sys[3] = &h.fs->reaper_tree;
	sys[4] = &h.fs->sfq_tree;
	sys[5] = &h.fs->uuid_tree;
	for (i = 0; i < 6; i++) {
		rc = scrub_walk_node(&sc, sys[i], sys[i]->root_oid,
				     sys[i]->root_vblk, sys[i]->root_level, 1);
		if (rc)
			break;
	}

	/* 2. Quét từng subvolume: cây FS + OMAP view + dữ liệu. */
	if (rc == 0) {
		struct vtxfs_handle hh = h;

		rc = vtxfs_cli_for_each_subvol(&hh, collect_subvol_cb, &cc);
	}
	for (i = 0; rc == 0 && i < cc.n; i++) {
		rc = vtx_subvol_switch_view(h.fs, sub_ids[i]);
		if (rc)
			break;
		rc = scrub_walk_node(&sc, &h.fs->active_omap_view,
				     h.fs->active_omap_view.root_oid,
				     h.fs->active_omap_view.root_vblk,
				     h.fs->active_omap_view.root_level, 1);
		if (rc)
			break;
		rc = scrub_walk_node(&sc, &h.fs->active_fs_tree,
				     h.fs->active_fs_tree.root_oid,
				     h.fs->active_fs_tree.root_vblk,
				     h.fs->active_fs_tree.root_level, 1);
		if (rc)
			break;
		rc = scrub_data_subvol(&sc);
	}

	printf("Scrub %s:\n", rc == 0 ? "hoàn tất" : "gián đoạn");
	printf("  checkpoint_xid    : %llu\n",
	       (unsigned long long)vtx_cpu64(sc.state.checkpoint_xid));
	printf("  flags             : 0x%x%s\n",
	       vtx_cpu32(sc.state.flags),
	       verify_only ? " (VERIFY_ONLY)" : "");
	printf("  verified_blocks   : %llu\n",
	       (unsigned long long)vtx_cpu64(sc.state.verified_blocks));
	printf("  meta_nodes        : %llu\n",
	       (unsigned long long)sc.meta_nodes);
	printf("  data_extents      : %llu\n",
	       (unsigned long long)sc.data_extents);
	printf("  error_count       : %u\n",
	       vtx_cpu32(sc.state.error_count));
	printf("  last_checked_vblk : 0x%llx\n",
	       (unsigned long long)vtx_cpu64(sc.state.last_checked_vblk));
	vtxfs_cli_close(&h, 0);
	return (rc == 0 && vtx_cpu32(sc.state.error_count) == 0) ? 0 : 1;
}

static int cmd_scrub_status(const struct cmd_struct *c, int argc, char **argv)
{
	(void)c;
	(void)argc;
	(void)argv;
	printf("vtxfs scrub: P0 không có scrub nền bền vững "
	       "(VTX_TREE_SCRUB là DEFERRED) — không có tiến trình nào.\n");
	return 0;
}

static const struct cmd_struct scrub_cmds[] = {
	{ "start",  cmd_scrub_start,  NULL, "<image> [--verify-only]" },
	{ "status", cmd_scrub_status, NULL, "<image>" },
};

const struct cmd_group scrub_cmd_group = {
	.name = "scrub",
	.usage = "Quét xác minh metadata (BLAKE3) + dữ liệu (CRC32C)",
	.cmds = scrub_cmds,
	.ncmds = sizeof(scrub_cmds) / sizeof(scrub_cmds[0]),
};
