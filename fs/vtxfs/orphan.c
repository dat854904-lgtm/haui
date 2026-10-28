// SPDX-License-Identifier: GPL-2.0-only
/*
 * orphan.c — orphan inode tracking & cleanup. An inode is marked orphan
 * when its last link is removed but extents may still be reclaimed; the
 * reaper / this path finishes the drop. Key: (ORPHAN_DIR, DIR_INDEX, ino).
 */
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

static void mk_orphan_key(struct vtx_key *k, u64 ino)
{
	VTXFS_TRACE_ENTER();

	vtxfs_mk_key(k, VTXFS_INO_ORPHAN_DIR, VTX_TYPE_DIR_INDEX, ino);
}

int vtxfs_orphan_add(struct vtxfs_root *root, u64 ino)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	struct vtx_dir_index_val val;
	void *w = NULL;
	int ret;

	if (!path)
		return -ENOMEM;
	mk_orphan_key(&key, ino);
	memset(&val, 0, sizeof(val));
	val.target_ino = cpu_to_le64(ino);

	ret = vtxfs_search_slot(root, path, &key, sizeof(val), 1);
	if (ret == 1) {
		ret = vtxfs_insert_item(root, path, &key, sizeof(val), &w);
		if (!ret && w)
			memcpy(w, &val, sizeof(val));
	} else if (ret == 0) {
		ret = 0; /* already orphaned */
	}
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_orphan_del(struct vtxfs_root *root, u64 ino)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	int ret;

	if (!path)
		return -ENOMEM;
	mk_orphan_key(&key, ino);
	ret = vtxfs_search_slot(root, path, &key, 0, 1);
	if (ret == 0)
		ret = vtxfs_del_item(root, path);
	vtxfs_free_path(path);
	return ret == 0 ? 0 : (ret > 0 ? -ENOENT : ret);
}

/* Walk all orphan markers and drop their inode items. */
int vtxfs_orphan_cleanup(struct vtxfs_root *root)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	int ret;

	if (!path)
		return -ENOMEM;
	mk_orphan_key(&key, 0);
	ret = vtxfs_search_slot(root, path, &key, 0, 1);
	while (ret >= 0) {
		struct vtxfs_extent_buffer *leaf = path->nodes[0];
		const struct vtx_key *found_k;
		u64 ino;

		if (path->slots[0] >= vtx_cpu16(leaf->hdr->nitems)) {
			ret = vtxfs_next_key(root, path);
			if (ret)
				break;
			leaf = path->nodes[0];
		}
		found_k = &leaf->hdr->entries[path->slots[0]].key;
		if (vtxfs_key_oid(found_k) != VTXFS_INO_ORPHAN_DIR ||
		    vtxfs_key_type(found_k) != VTX_TYPE_DIR_INDEX) {
			ret = 1;
			break;
		}
		ino = vtxfs_key_off(found_k);

		vtxfs_release_path(path);
		/* drop extent items + inode item, then clear marker */
		if (root->fs_info && root->fs_info->sb) {
			struct vtxfs_sb_info *sbi = VTXFS_SB(root->fs_info->sb);
			if (sbi && sbi->fs && !sbi->fs->read_only)
				(void)vtx_orphan_purge(sbi->fs, sbi->subvol_id, ino);
		}
		ret = vtxfs_orphan_del(root, ino);
		if (ret && ret != -ENOENT)
			break;
		ret = vtxfs_search_slot(root, path, &key, 0, 1);
	}
	vtxfs_free_path(path);
	return ret == 1 ? 0 : ret; /* 1 == past last key */
}
