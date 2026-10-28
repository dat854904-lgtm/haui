// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "common.h"

#define DUMP_MAX_LEVEL 8U

/* ---------- helper in khoá & tên kiểu ---------- */
static const char *type_name(__u8 t)
{
	switch (t) {
	case VTX_TYPE_INODE: return "INODE";
	case VTX_TYPE_INODE_REF: return "INODE_REF";
	case VTX_TYPE_XATTR: return "XATTR";
	case VTX_TYPE_DIR_ITEM: return "DIR_ITEM";
	case VTX_TYPE_DIR_INDEX: return "DIR_INDEX";
	case VTX_TYPE_EXTENT: return "EXTENT";
	case VTX_TYPE_SUBVOL_ROOT: return "SUBVOL_ROOT";
	case VTX_TYPE_OMAP_VAL: return "OMAP_VAL";
	case VTX_TYPE_REF: return "REF";
	case VTX_TYPE_SFQ: return "SFQ";
	case VTX_TYPE_REAPER_JOB: return "REAPER_JOB";
	case VTX_TYPE_UUID_MAP: return "UUID_MAP";
	case VTX_TYPE_ALLOC_BY_VBLK: return "ALLOC_BY_VBLK";
	default: return "UNKNOWN";
	}
}

static void print_key(const struct vtx_key *k)
{
	__u64 w = vtx_cpu64(k->oid_and_type);

	printf("(oid=%llu type=%u/%s off=%llu)",
	       (unsigned long long)VTX_KEY_GET_OID(w),
	       VTX_KEY_GET_TYPE(w), type_name(VTX_KEY_GET_TYPE(w)),
	       (unsigned long long)vtx_cpu64(k->offset));
}

/* ---------- in nội dung value theo kiểu item ---------- */
static void print_item_value(__u8 type, const void *val, __u16 len, int ind)
{
	switch (type) {
	case VTX_TYPE_INODE:
		if (len >= sizeof(struct vtx_inode_item)) {
			const struct vtx_inode_item *it = val;
			printf("%*ssize=%llu alloc=%llu mode=0%o nlink=%u "
			       "uid=%u gid=%u\n", ind, "",
			       (unsigned long long)vtx_cpu64(it->size_bytes),
			       (unsigned long long)vtx_cpu64(it->alloc_bytes),
			       vtx_cpu32(it->mode), vtx_cpu32(it->nlink),
			       vtx_cpu32(it->uid), vtx_cpu32(it->gid));
		}
		break;
	case VTX_TYPE_EXTENT:
		if (len >= sizeof(struct vtx_extent_item)) {
			const struct vtx_extent_item *e = val;
			__u32 fl = vtx_cpu32(e->flags);
			printf("%*svblk=0x%llx alloc_id=%llu phys=%u "
			       "logical=%u flags=0x%x%s%s csum=%u\n",
			       ind, "",
			       (unsigned long long)vtx_cpu64(e->vblk),
			       (unsigned long long)vtx_cpu64(e->allocation_id),
			       vtx_cpu32(e->physical_blocks),
			       vtx_cpu32(e->logical_bytes), fl,
			       (fl & VTX_EXTENT_INLINE) ? " INLINE" : "",
			       (fl & VTX_EXTENT_SHARED) ? " SHARED" : "",
			       vtx_cpu16(e->csum_count));
		}
		break;
	case VTX_TYPE_REF:
		if (len >= sizeof(struct vtx_ref_item)) {
			const struct vtx_ref_item *r = val;
			printf("%*sstart_vblk=0x%llx refcount=%llu blocks=%u "
			       "flags=0x%x\n", ind, "",
			       (unsigned long long)vtx_cpu64(r->start_vblk),
			       (unsigned long long)vtx_cpu64(r->refcount),
			       vtx_cpu32(r->block_count),
			       vtx_cpu32(r->flags));
		}
		break;
	case VTX_TYPE_SFQ:
		if (len >= sizeof(struct vtx_sfq_entry)) {
			const struct vtx_sfq_entry *s = val;
			printf("%*sstart_vblk=0x%llx blocks=%llu retired_xid=%llu\n",
			       ind, "",
			       (unsigned long long)vtx_cpu64(s->start_vblk),
			       (unsigned long long)vtx_cpu64(s->block_count),
			       (unsigned long long)vtx_cpu64(s->retired_xid));
		}
		break;
	case VTX_TYPE_DIR_ITEM:
		if (len >= sizeof(struct vtx_dir_item)) {
			const struct vtx_dir_item *d = val;
			__u16 nl = vtx_cpu16(d->name_len);
			printf("%*starget=%llu type=%s name='%.*s'\n", ind, "",
			       (unsigned long long)vtx_cpu64(d->target_ino),
			       vtxfs_cli_ftype_name(d->file_type),
			       (int)nl, d->name);
		}
		break;
	case VTX_TYPE_SUBVOL_ROOT:
		if (len >= sizeof(struct vtx_subvol_root_item)) {
			const struct vtx_subvol_root_item *s = val;
			printf("%*ssubvol=%llu parent=%llu root_vblk=0x%llx "
			       "flags=0x%llx inodes=%llu\n", ind, "",
			       (unsigned long long)vtx_cpu64(s->subvol_id),
			       (unsigned long long)vtx_cpu64(s->parent_subvol_id),
			       (unsigned long long)vtx_cpu64(s->root_node_vblk),
			       (unsigned long long)vtx_cpu64(s->flags),
			       (unsigned long long)vtx_cpu64(s->num_inodes));
		}
		break;
	case VTX_TYPE_OMAP_VAL:
		if (len >= sizeof(struct vtx_omap_val)) {
			const struct vtx_omap_val *o = val;
			printf("%*svblk=0x%llx node_size=%u flags=0x%x%s\n",
			       ind, "",
			       (unsigned long long)vtx_cpu64(o->vblk),
			       vtx_cpu32(o->node_size), vtx_cpu32(o->flags),
			       (vtx_cpu32(o->flags) & VTX_OMAP_VAL_TOMBSTONE)
			       ? " TOMBSTONE" : "");
		}
		break;
	case VTX_TYPE_UUID_MAP:
		if (len >= sizeof(struct vtx_uuid_map_item)) {
			const struct vtx_uuid_map_item *u = val;
			printf("%*ssubvol=%llu kind=%u uuid=", ind, "",
			       (unsigned long long)vtx_cpu64(u->subvol_id),
			       vtx_cpu32(u->kind));
			vtxfs_cli_print_uuid(u->uuid);
			printf("\n");
		}
		break;
	case VTX_TYPE_ALLOC_BY_VBLK:
		if (len >= sizeof(struct vtx_alloc_by_vblk_item)) {
			const struct vtx_alloc_by_vblk_item *a = val;
			printf("%*salloc_id=%llu blocks=%u\n", ind, "",
			       (unsigned long long)vtx_cpu64(a->allocation_id),
			       vtx_cpu32(a->block_count));
		}
		break;
	case VTX_TYPE_REAPER_JOB:
		if (len >= sizeof(struct vtx_reaper_job)) {
			const struct vtx_reaper_job *j = val;
			printf("%*sjob=%llu object=%llu phase=%u op=%u\n",
			       ind, "",
			       (unsigned long long)vtx_cpu64(j->job_id),
			       (unsigned long long)vtx_cpu64(j->object_id),
			       vtx_cpu32(j->phase), vtx_cpu32(j->operation));
		}
		break;
	case VTX_TYPE_XATTR:
		if (len >= sizeof(struct vtx_xattr_item)) {
			const struct vtx_xattr_item *x = val;
			__u16 nl = vtx_cpu16(x->name_len);
			__u16 vl = vtx_cpu16(x->value_len);
			printf("%*sname='%.*s' value_len=%u\n", ind, "",
			       (int)nl, x->data, vl);
		}
		break;
	default:
		printf("%*s[%u bytes]\n", ind, "", len);
		break;
	}
}

/* ---------- thống kê cây ---------- */
struct tree_stats {
	__u64 total_nodes;
	__u64 nodes_by_level[DUMP_MAX_LEVEL];
	__u64 total_items;
	__u64 total_item_bytes;
	__u64 total_node_used;
	__u64 total_node_capacity;
	int max_level;
};

/* ---------- walker đệ quy: dump hoặc stats ---------- */
static int walk_node(struct vtx_rw_tree *t, __u64 oid, __u64 vblk,
		     int level, int is_root, int indent, int verbose,
		     struct tree_stats *st)
{
	const void *node = NULL;
	const struct vtx_node_hdr *h;
	size_t bytes = 0;
	int verified = 0, ret;
	__u32 i, n;

	ret = t->ops->read_node(t->ctx, vblk, &node, &bytes, &verified);
	if (ret)
		return ret;
	if (!verified || bytes != t->node_bytes) {
		t->ops->release_node(t->ctx, node);
		return -EUCLEAN;
	}
	h = (const struct vtx_node_hdr *)node;
	n = vtx_cpu16(h->nitems);

	if (st) {
		__u32 head = (__u32)vtx_cpu16(h->free_head8) * 8U;
		__u32 tail = (__u32)vtx_cpu16(h->free_tail8) * 8U;

		st->total_nodes++;
		if (level < (int)DUMP_MAX_LEVEL)
			st->nodes_by_level[level]++;
		st->total_node_used += head + (t->node_bytes - tail);
		st->total_node_capacity += t->node_bytes;
		if (level > st->max_level)
			st->max_level = level;
	}
	if (verbose)
		printf("%*s%s vblk=0x%llx oid=%llu level=%d nitems=%u "
		       "flags=0x%x%s\n", indent, "",
		       level == 0 ? "LEAF" : "NODE",
		       (unsigned long long)vblk,
		       (unsigned long long)oid, level, n,
		       vtx_cpu16(h->flags), is_root ? " ROOT" : "");

	if (level == 0) {
		for (i = 0; i < n; i++) {
			const struct vtx_node_entry *e = &h->entries[i];
			const void *val = (const __u8 *)node +
				(__u32)vtx_cpu16(e->slot.val_off8) * 8U;
			__u16 len = vtx_cpu16(e->slot.val_len);
			__u8 ty = VTX_KEY_GET_TYPE(
					vtx_cpu64(e->key.oid_and_type));

			if (st) {
				st->total_items++;
				st->total_item_bytes += len;
			}
			if (verbose) {
				printf("%*s  ITEM ", indent, "");
				print_key(&e->key);
				printf(" len=%u\n", len);
				print_item_value(ty, val, len, indent + 4);
			}
		}
		t->ops->release_node(t->ctx, node);
		return 0;
	}

	/* node internal: sao chép rồi đệ quy xuống con */
	{
		__u8 *copy = malloc(t->node_bytes);

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
			       (__u32)vtx_cpu16(e->slot.val_off8) * 8U,
			       sizeof(ptr));
			c_oid = vtx_cpu64(ptr.child_oid);
			if (t->is_physical) {
				c_vblk = vtx_cpu64(ptr.hint_vblk);
			} else {
				ret = t->ops->omap_resolve(t->ctx, t->view_id,
							   c_oid, &c_vblk);
				if (ret) {
					free(copy);
					return ret;
				}
			}
			if (verbose) {
				printf("%*s  PTR ", indent, "");
				print_key(&e->key);
				printf(" -> child_oid=%llu vblk=0x%llx\n",
				       (unsigned long long)c_oid,
				       (unsigned long long)c_vblk);
			}
			ret = walk_node(t, c_oid, c_vblk, level - 1, 0,
					indent + 2, verbose, st);
			if (ret) {
				free(copy);
				return ret;
			}
		}
		free(copy);
	}
	return 0;
}

static void print_subvol_header(struct vtx_fs *fs, __u64 subvol)
{
	struct vtx_key k;
	struct vtx_subvol_root_item it;
	size_t bytes = sizeof(it);

	k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(subvol,
					VTX_TYPE_SUBVOL_ROOT));
	k.offset = 0;
	if (vtx_rw_lookup(&fs->root_tree, &k, &it, &bytes) != 0 ||
	    bytes != sizeof(it))
		return;

	printf("  subvol_id       : %llu\n",
	       (unsigned long long)subvol);
	printf("  parent_subvol   : %llu\n",
	       (unsigned long long)vtx_cpu64(it.parent_subvol_id));
	printf("  flags           : 0x%llx%s%s\n",
	       (unsigned long long)vtx_cpu64(it.flags),
	       (vtx_cpu64(it.flags) & VTX_SUBVOL_RDONLY) ? " RO" : "",
	       (vtx_cpu64(it.flags) & VTX_SUBVOL_REAPING) ? " REAPING" : "");
	printf("  num_inodes      : %llu\n",
	       (unsigned long long)vtx_cpu64(it.num_inodes));
	printf("  last_snap_xid   : %llu\n",
	       (unsigned long long)vtx_cpu64(it.last_snap_xid));
	printf("  fs_root_vblk    : 0x%llx\n",
	       (unsigned long long)vtx_cpu64(it.root_node_vblk));
	printf("  omap_view_vblk  : 0x%llx\n",
	       (unsigned long long)vtx_cpu64(it.omap_view_root_vblk));
	printf("  uuid            : ");
	vtxfs_cli_print_uuid(it.uuid);
	printf("\n");
}

static struct vtx_rw_tree *select_tree_ex(struct vtx_fs *fs,
					  const char *name, __u64 subvol,
					  int allow_reaping,
					  struct vtx_subvol_root_item *out)
{
	if (!strcmp(name, "root"))   return &fs->root_tree;
	if (!strcmp(name, "ref"))    return &fs->ref_tree;
	if (!strcmp(name, "sfq"))    return &fs->sfq_tree;
	if (!strcmp(name, "reaper")) return &fs->reaper_tree;
	if (!strcmp(name, "uuid"))   return &fs->uuid_tree;
	if (!strcmp(name, "omap") || !strcmp(name, "omap-meta"))
		return &fs->omap_meta_tree;

	if (!strcmp(name, "fs") || !strcmp(name, "view") ||
	    !strcmp(name, "omap-view")) {
		unsigned int sf = VTXFS_SELECT_VIEW;   /* KHÔNG flush */
		int ret;

		if (allow_reaping)
			sf |= VTXFS_SELECT_ALLOW_REAPING;
		ret = vtxfs_cli_select_subvol(fs, subvol, sf, out);
		if (ret)
			return NULL;
		return (!strcmp(name, "fs")) ? &fs->active_fs_tree
					     : &fs->active_omap_view;
	}
	return NULL;
}

/* ---------- dump-super ---------- */
static int cmd_dump_super(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct vtx_super_header *sb;
	int rc;

	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "usage: vtxfs inspect dump-super <image>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	sb = &h.fs->super_blk.hdr;
	printf("Superblock '%s':\n", argv[0]);
	printf("  magic           : %.8s\n", sb->magic);
	printf("  version         : %u.%u\n",
	       vtx_cpu16(sb->major), vtx_cpu16(sb->minor));
	printf("  fsid            : ");
	vtxfs_cli_print_uuid(sb->fsid_uuid);
	printf("\n  metadata_uuid   : ");
	vtxfs_cli_print_uuid(sb->metadata_uuid);
	printf("\n  super_generation: %llu\n",
	       (unsigned long long)vtx_cpu64(sb->super_generation));
	printf("  total_blocks    : %llu\n",
	       (unsigned long long)vtx_cpu64(sb->total_blocks));
	printf("  node_size       : %u\n", vtx_cpu32(sb->node_size));
	printf("  incompat        : 0x%llx\n",
	       (unsigned long long)vtx_cpu64(sb->incompat_flags));
	printf("  ro_compat       : 0x%llx\n",
	       (unsigned long long)vtx_cpu64(sb->ro_compat_flags));
	printf("  label           : %s\n", sb->label);
	printf("Committed CPD xid : %llu\n",
	       (unsigned long long)h.fs->committed_xid);
	vtxfs_cli_close(&h, 0);
	return 0;
}

/* ---------- dump-tree ---------- */
static int cmd_dump_tree(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct vtx_rw_tree *t;
	const char *a, *tname;
	__u64 subvol;
	int allow_reaping;
	struct vtx_subvol_root_item sitem;
	int rc;

	(void)cmd;
	if (argc < 1) {
		fprintf(stderr,
			"usage: vtxfs inspect dump-tree <image> --tree=<root|ref|sfq|reaper|uuid|omap|fs|view|omap-view> [--subvol=<id>] [--allow-reaping]\n");
		return 1;
	}
	tname = vtxfs_cli_opt(argc - 1, argv + 1, "tree");
	if (!tname)
		tname = "root";
	subvol = vtxfs_cli_opt_u64(argc - 1, argv + 1, "subvol",
				   VTXFS_OID_FIRST_USER_SUBVOL);
	allow_reaping = vtxfs_cli_flag(argc - 1, argv + 1, "allow-reaping");

	rc = vtxfs_cli_open(&h, argv[0], 0);	/* RO: không flush khi switch */
	if (rc)
		return 1;

	a = vtxfs_cli_opt(argc - 1, argv + 1, "subvol");
	if (a && strcmp(tname, "fs") && strcmp(tname, "view") &&
	    strcmp(tname, "omap-view")) {
		fprintf(stderr,
			"vtxfs: --subvol chỉ dùng với --tree=fs hoặc --tree=omap-view\n");
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	memset(&sitem, 0, sizeof(sitem));
	t = select_tree_ex(h.fs, tname, subvol, allow_reaping, &sitem);
	if (!t) {
		fprintf(stderr, "vtxfs: không chọn được cây '%s' (subvol=%llu)\n",
			tname, (unsigned long long)subvol);
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	printf("=== dump-tree '%s'", tname);
	if (!strcmp(tname, "fs") || !strcmp(tname, "view") ||
	    !strcmp(tname, "omap-view"))
		printf(" subvol=%llu", (unsigned long long)subvol);
	printf(" root_vblk=0x%llx level=%u %s ===\n",
	       (unsigned long long)t->root_vblk, t->root_level,
	       t->is_physical ? "PHYSICAL_VBLK" : "VIRTUAL_OID");
	if (!strcmp(tname, "fs") || !strcmp(tname, "view") ||
	    !strcmp(tname, "omap-view")) {
		print_subvol_header(h.fs, subvol);
		if (allow_reaping &&
		    (vtx_cpu64(sitem.flags) & VTX_SUBVOL_REAPING))
			printf("  NOTE: REAPING — đang đọc từ checkpoint đã "
			       "commit cuối; cây có thể bị tháo dở bởi Reaper\n");
	}

	rc = walk_node(t, t->root_oid, t->root_vblk, t->root_level,
		       1, 0, 1, NULL);
	if (rc)
		fprintf(stderr, "vtxfs: dump-tree lỗi: %s\n", strerror(-rc));
	vtxfs_cli_close(&h, 0);
	return rc ? 1 : 0;
}

/* ---------- tree-stats ---------- */
static int cmd_tree_stats(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct vtx_rw_tree *t;
	struct tree_stats st;
	const char *tname;
	__u64 subvol;
	int allow_reaping;
	struct vtx_subvol_root_item sitem;
	int i, rc;

	(void)cmd;
	if (argc < 1) {
		fprintf(stderr,
			"usage: vtxfs inspect tree-stats <image> --tree=<...> [--subvol=<id>] [--allow-reaping]\n");
		return 1;
	}
	tname = vtxfs_cli_opt(argc - 1, argv + 1, "tree");
	if (!tname)
		tname = "root";
	subvol = vtxfs_cli_opt_u64(argc - 1, argv + 1, "subvol",
				   VTXFS_OID_FIRST_USER_SUBVOL);
	allow_reaping = vtxfs_cli_flag(argc - 1, argv + 1, "allow-reaping");

	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;
	memset(&sitem, 0, sizeof(sitem));
	t = select_tree_ex(h.fs, tname, subvol, allow_reaping, &sitem);
	if (!t) {
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	memset(&st, 0, sizeof(st));
	rc = walk_node(t, t->root_oid, t->root_vblk, t->root_level,
		       1, 0, 0, &st);
	if (rc) {
		fprintf(stderr, "vtxfs: tree-stats lỗi: %s\n", strerror(-rc));
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	printf("Tree stats '%s'", tname);
	if (!strcmp(tname, "fs") || !strcmp(tname, "view") ||
	    !strcmp(tname, "omap-view"))
		printf(" subvol=%llu", (unsigned long long)subvol);
	printf(":\n");
	if (!strcmp(tname, "fs") || !strcmp(tname, "view") ||
	    !strcmp(tname, "omap-view")) {
		print_subvol_header(h.fs, subvol);
		if (allow_reaping &&
		    (vtx_cpu64(sitem.flags) & VTX_SUBVOL_REAPING))
			printf("  NOTE: REAPING — đang đọc từ checkpoint đã "
			       "commit cuối\n");
	}
	printf("  depth (max level) : %d\n", st.max_level);
	printf("  total_nodes       : %llu\n",
	       (unsigned long long)st.total_nodes);
	for (i = 0; i <= st.max_level && i < (int)DUMP_MAX_LEVEL; i++)
		printf("    level %d nodes   : %llu\n", i,
		       (unsigned long long)st.nodes_by_level[i]);
	printf("  total_items       : %llu\n",
	       (unsigned long long)st.total_items);
	printf("  total_item_bytes  : %llu\n",
	       (unsigned long long)st.total_item_bytes);
	printf("  node_used_bytes   : %llu / %llu (%.1f%%)\n",
	       (unsigned long long)st.total_node_used,
	       (unsigned long long)st.total_node_capacity,
	       st.total_node_capacity ?
	       100.0 * (double)st.total_node_used /
	       (double)st.total_node_capacity : 0.0);
	vtxfs_cli_close(&h, 0);
	return 0;
}

static const struct cmd_struct inspect_cmds[] = {
	{ "stat",       cmd_stat,          NULL, "<image>" },
	{ "file",       cmd_file,          NULL, "<file_path> | <image> <vtxfs_path>" },
	{ "sys-index",  cmd_sys_index,     NULL, "<image>" },
	{ "map",        cmd_map,           NULL, "<image> [--vag=N] [--compact]" },
	{ "block",      cmd_block,         NULL, "<image> <pblk> [--vblk]" },
	{ "dump-super", cmd_dump_super,    NULL, "<image>" },
	{ "dump-tree",  cmd_dump_tree,     NULL, "<image> --tree=<name> [--subvol=<id>] [--allow-reaping]" },
	{ "tree-stats", cmd_tree_stats,    NULL, "<image> --tree=<name> [--subvol=<id>] [--allow-reaping]" },
};

const struct cmd_group inspect_cmd_group = {
	.name = "inspect",
	.usage = "Công cụ debug/inspect ctree, superblock, block map và block data",
	.cmds = inspect_cmds,
	.ncmds = sizeof(inspect_cmds) / sizeof(inspect_cmds[0]),
};
