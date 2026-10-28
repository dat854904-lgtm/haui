// SPDX-License-Identifier: GPL-2.0-only
/*
 * uuid-tree.c — UUID -> subvolume mapping (VTX_TREE_UUID, vtx_uuid_map_item).
 */
#include <linux/string.h>

#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

static void mk_uuid_key(struct vtx_key *k, u64 subvol_id)
{
	VTXFS_TRACE_ENTER();

	vtxfs_mk_key(k, subvol_id, VTX_TYPE_UUID_MAP, subvol_id);
}

int vtxfs_uuid_insert(struct vtxfs_fs_info *fs, const u8 uuid[16],
		      u64 subvol_id, u32 kind)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	struct vtx_uuid_map_item item;
	void *w = NULL;
	int ret;

	if (!path)
		return -ENOMEM;
	mk_uuid_key(&key, subvol_id);
	memset(&item, 0, sizeof(item));
	memcpy(item.uuid, uuid, 16);
	item.subvol_id = cpu_to_le64(subvol_id);
	item.kind = cpu_to_le32(kind);

	ret = vtxfs_search_slot(fs->uuid_root, path, &key, sizeof(item), 1);
	if (ret != 1) {
		ret = ret ? ret : -EEXIST;
		goto out;
	}
	ret = vtxfs_insert_item(fs->uuid_root, path, &key, sizeof(item), &w);
	if (!ret && w)
		memcpy(w, &item, sizeof(item));
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_uuid_lookup(struct vtxfs_fs_info *fs, const u8 uuid[16],
		      u64 *subvol_id)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	struct vtx_uuid_map_item item;
	int ret;

	if (!path)
		return -ENOMEM;
	/* Scan the UUID tree linearly (small in P0) for the UUID. */
	key.oid_and_type = cpu_to_le64(VTX_MAKE_OID_TYPE(1, VTX_TYPE_UUID_MAP));
	key.offset = 0;
	ret = vtxfs_search_slot(fs->uuid_root, path, &key, 0, 0);
	while (ret >= 0) {
		struct vtxfs_extent_buffer *leaf = path->nodes[0];
		const struct vtx_slot *sl;

		if (path->slots[0] >= vtx_cpu16(leaf->hdr->nitems)) {
			ret = vtxfs_next_key(fs->uuid_root, path);
			if (ret)
				break;
			leaf = path->nodes[0];
		}
		sl = &leaf->hdr->entries[path->slots[0]].slot;
		if (vtx_cpu16(sl->val_len) == sizeof(item)) {
			memcpy(&item, (u8 *)leaf->hdr +
			       vtx_cpu16(sl->val_off8) * 8U, sizeof(item));
			if (!memcmp(item.uuid, uuid, 16)) {
				*subvol_id = vtx_cpu64(item.subvol_id);
				ret = 0;
				goto out;
			}
		}
		ret = vtxfs_next_key(fs->uuid_root, path);
	}
	ret = -ENOENT;
out:
	vtxfs_free_path(path);
	return ret;
}

int vtxfs_uuid_delete(struct vtxfs_fs_info *fs, const u8 uuid[16],
		      u64 subvol_id)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_path *path = vtxfs_alloc_path();
	struct vtx_key key;
	int ret;

	(void)uuid;
	if (!path)
		return -ENOMEM;
	mk_uuid_key(&key, subvol_id);
	ret = vtxfs_search_slot(fs->uuid_root, path, &key, 0, 1);
	if (ret == 0)
		ret = vtxfs_del_item(fs->uuid_root, path);
	vtxfs_free_path(path);
	return ret == 0 ? 0 : (ret > 0 ? -ENOENT : ret);
}
