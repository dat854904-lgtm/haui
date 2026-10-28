/* SPDX-License-Identifier: GPL-2.0-only */
#include "vtxfs_engine.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sched/signal.h>
static inline void *vtx_zalloc(size_t size) { return vzalloc(size); }
static inline void vtx_free(void *ptr) { vfree(ptr); }
#else
#include <errno.h>
#include <string.h>
#include <stdlib.h>
static inline void *vtx_zalloc(size_t size) { return calloc(1, size); }
static inline void vtx_free(void *ptr) { free(ptr); }
#endif

static int enter_subvol(struct vtx_fs *fs, __u64 subvol_id, int for_write)
{
    int ret;
    if (!fs || !fs->mounted)
        return -EINVAL;
    if (for_write && (fs->read_only || fs->view_only))
        return -EROFS;
    if (fs->active_subvol_id == subvol_id)
        goto check_subvol;
    ret = fs->view_only ? vtx_subvol_switch_view(fs, subvol_id)
                        : vtx_subvol_switch(fs, subvol_id);
    if (ret)
        return ret;
check_subvol:
    if (for_write) {
        __u64 flags = vtx_cpu64(fs->active_subvol.flags);
        if (flags & (VTX_SUBVOL_RDONLY | VTX_SUBVOL_REAPING))
            return -EROFS;
        fs->active_subvol.change_xid = vtx_le64(fs->current_xid);
    }
    return 0;
}

static int load_inode(struct vtx_fs *fs, __u64 ino,
                      struct vtx_inode_item *out)
{
    struct vtx_key k;
    size_t bytes = sizeof(*out);
    int ret;

    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_INODE));
    k.offset = 0;
    ret = vtx_rw_lookup(&fs->active_fs_tree, &k, out, &bytes);
    if (ret)
        return ret;
    return (bytes == sizeof(*out)) ? 0 : -EUCLEAN;
}

static int save_inode(struct vtx_fs *fs, __u64 ino,
                      struct vtx_inode_item *in)
{
    struct vtx_key k;
    in->update_xid = vtx_le64(fs->current_xid);
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_INODE));
    k.offset = 0;
    return vtx_rw_upsert(&fs->active_fs_tree, &k, in, sizeof(*in));
}

static int dir_add_entry(struct vtx_fs *fs, __u64 dir_ino,
                         __u64 target_ino, const char *name,
                         __u8 ftype);

/* -------------------------------------------------------------------------
 * 1. Subvolume & Snapshot Management
 * ------------------------------------------------------------------------- */

int vtx_subvol_create(struct vtx_fs *fs, __u64 parent_subvol_id,
                      const char *name, __u64 *new_subvol_id_out)
{
    __u64 sub_id, view_oid = 0, view_vblk = 0, fs_oid = 0, fs_vblk = 0;
    void *view_buf = NULL, *fs_buf = NULL;
    struct vtx_node_hdr *h;
    struct vtx_key k;
    struct vtx_inode_item root_ino;
    struct vtx_inode_ref root_ref;
    struct vtx_uuid_map_item uuid_it;
    __u32 i;
    int ret;

    if (!fs || !fs->mounted || !new_subvol_id_out)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;
    /* A named child is recorded in the parent's namespace. Never write
     * that directory when the parent is an immutable RO snapshot.
     * Unnamed snapshot clones still may READ from a RO parent. */
    if (name && name[0]) {
        ret = vtx_subvol_switch(fs, parent_subvol_id);
        if (ret)
            return ret;
        if (vtx_cpu64(fs->active_subvol.flags) &
            (VTX_SUBVOL_RDONLY | VTX_SUBVOL_REAPING))
            return -EROFS;
    }

    if (fs->active_subvol_id) {
        ret = vtx_subvol_switch(fs, fs->active_subvol_id);
        if (ret)
            return ret;
    }

    sub_id = fs->next_oid++;
    ret = fs->root_tree.ops->alloc_node(fs, VTX_TREE_OMAP_VIEW, 0,
                                        &view_oid, &view_vblk, &view_buf);
    if (ret)
        return ret;
    ret = vtx_leaf_init(view_buf, VTXFS_BLOCK_SIZE, view_oid, fs->current_xid,
                        view_vblk, VTX_TREE_OMAP_VIEW,
                        VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT);
    if (ret)
        return ret;
    h = (struct vtx_node_hdr *)view_buf;
    h->owner_id = vtx_le64(sub_id);

    ret = fs->root_tree.ops->alloc_node(fs, VTX_TREE_FS, 0,
                                        &fs_oid, &fs_vblk, &fs_buf);
    if (ret)
        return ret;
    ret = vtx_leaf_init(fs_buf, VTXFS_BLOCK_SIZE, fs_oid, fs->current_xid,
                        fs_vblk, VTX_TREE_FS,
                        VTX_NODE_FLAG_LEAF | VTX_NODE_FLAG_ROOT);
    if (ret)
        return ret;
    h = (struct vtx_node_hdr *)fs_buf;
    h->owner_id = vtx_le64(sub_id);

    memset(&fs->active_subvol, 0, sizeof(fs->active_subvol));
    fs->active_subvol.subvol_id = vtx_le64(sub_id);
    fs->active_subvol.parent_subvol_id = vtx_le64(parent_subvol_id);
    fs->active_subvol.root_node_oid = vtx_le64(fs_oid);
    fs->active_subvol.root_node_vblk = vtx_le64(fs_vblk);
    fs->active_subvol.omap_view_root_vblk = vtx_le64(view_vblk);
    fs->active_subvol.create_xid = vtx_le64(fs->current_xid);
    fs->active_subvol.change_xid = vtx_le64(fs->current_xid);
    fs->active_subvol.num_inodes = vtx_le64(1);
    for (i = 0; i < VTXFS_UUID_SIZE; i++)
        fs->active_subvol.uuid[i] =
            (__u8)(fs->super_blk.hdr.fsid_uuid[i] ^ (__u8)(sub_id + i));

    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(sub_id, VTX_TYPE_SUBVOL_ROOT));
    k.offset = 0;
    ret = vtx_rw_insert(&fs->root_tree, &k, &fs->active_subvol,
                        sizeof(fs->active_subvol));
    if (ret)
        return ret;

    fs->active_subvol_id = 0;
    ret = vtx_subvol_switch(fs, sub_id);
    if (ret)
        return ret;

    ret = fs->active_fs_tree.ops->omap_set(fs, sub_id, fs_oid, fs_vblk);
    if (ret)
        return ret;

    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.parent_ino = vtx_le64(VTXFS_INO_ROOT_DIR);
    root_ino.update_xid = vtx_le64(fs->current_xid);
    root_ino.mode = vtx_le32(0040755U);
    root_ino.nlink = vtx_le32(1);
    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_INO_ROOT_DIR, VTX_TYPE_INODE));
    k.offset = 0;
    ret = vtx_rw_insert(&fs->active_fs_tree, &k, &root_ino, sizeof(root_ino));
    if (ret)
        return ret;

    memset(&root_ref, 0, sizeof(root_ref));
    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_INO_ROOT_DIR, VTX_TYPE_INODE_REF));
    k.offset = vtx_le64(VTXFS_INO_ROOT_DIR);
    ret = vtx_rw_insert(&fs->active_fs_tree, &k, &root_ref, sizeof(root_ref));
    if (ret)
        return ret;

    memset(&uuid_it, 0, sizeof(uuid_it));
    memcpy(uuid_it.uuid, fs->active_subvol.uuid, VTXFS_UUID_SIZE);
    uuid_it.subvol_id = vtx_le64(sub_id);
    uuid_it.kind = vtx_le32(1);
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(sub_id, VTX_TYPE_UUID_MAP));
    k.offset = vtx_le64(sub_id);
    ret = vtx_rw_insert(&fs->uuid_tree, &k, &uuid_it, sizeof(uuid_it));
    if (ret)
        return ret;

    /* Liên kết namespace: lưu tên subvolume như một directory item
     * (file_type = VTX_FT_SUBVOL, target_ino = subvol_id) trong thư mục
     * gốc của subvolume cha, theo đúng on-disk contract. */
    if (name && name[0] != '\0') {
        __u64 saved_subvol = fs->active_subvol_id;
        size_t nlen = strlen(name);

        if (nlen > VTXFS_NAME_MAX)
            return -ENAMETOOLONG;
        ret = vtx_subvol_switch(fs, parent_subvol_id);
        if (ret)
            return ret;
        ret = dir_add_entry(fs, VTXFS_INO_ROOT_DIR, sub_id, name,
                            VTX_FT_SUBVOL);
        if (ret)
            return ret;
        if (saved_subvol) {
            ret = vtx_subvol_switch(fs, saved_subvol);
            if (ret)
                return ret;
        }
    }

    *new_subvol_id_out = sub_id;
    return 0;
}

int vtx_snapshot_create(struct vtx_fs *fs, __u64 src_subvol_id,
                        const char *name, int read_only,
                        __u64 *snap_subvol_id_out)
{
    __u64 snap_id = 0;
    struct vtx_key cur_k = {0}, found_k;
    struct vtx_subvol_root_item src_sub;
    __u8 *item_buf = NULL;
    size_t bytes;
    int first = 1, ret;

    if (!fs || !fs->mounted || !snap_subvol_id_out)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;

    ret = vtx_subvol_switch(fs, src_subvol_id);
    if (ret)
        return ret;
    src_sub = fs->active_subvol;
    if (vtx_cpu64(src_sub.flags) & VTX_SUBVOL_REAPING)
        return -EBUSY;

    ret = vtx_subvol_create(fs, src_subvol_id, name, &snap_id);
    if (ret)
        return ret;

    /* Remove the default empty root dir items in snap_id before cloning. */
    {
        struct vtx_key dk;
        dk.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_INO_ROOT_DIR, VTX_TYPE_INODE));
        dk.offset = 0;
        (void)vtx_rw_delete(&fs->active_fs_tree, &dk);
        dk.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(VTXFS_INO_ROOT_DIR, VTX_TYPE_INODE_REF));
        dk.offset = vtx_le64(VTXFS_INO_ROOT_DIR);
        (void)vtx_rw_delete(&fs->active_fs_tree, &dk);
    }

    item_buf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!item_buf)
        return -ENOMEM;

    for (;;) {
        ret = vtx_subvol_switch(fs, src_subvol_id);
        if (ret)
            break;
        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first,
                          &found_k, item_buf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur_k = found_k;

        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) ==
            VTX_TYPE_EXTENT) {
            struct vtx_extent_item *ext = (struct vtx_extent_item *)item_buf;
            __u32 eflags = vtx_cpu32(ext->flags);
            if (!(eflags & VTX_EXTENT_INLINE)) {
                /* The source may be a read-only snapshot: never write it.
                 * REF is authoritative; SHARED is only an advisory bit
                 * on the newly cloned destination item. */
                ret = vtx_enqueue_delayed_ref(
                    fs, vtx_cpu64(ext->allocation_id),
                    vtx_cpu64(ext->vblk),
                    vtx_cpu32(ext->physical_blocks),
                    VTX_REF_FLAG_DATA, (__u32)snap_id, 1);
                if (ret)
                    break;
                ext->flags = vtx_le32(eflags | VTX_EXTENT_SHARED);
            }
        }

        ret = vtx_subvol_switch(fs, snap_id);
        if (ret)
            break;
        ret = vtx_rw_insert(&fs->active_fs_tree, &found_k,
                            item_buf, (__u16)bytes);
        if (ret)
            break;
    }

    vtx_free(item_buf);
    if (ret) {
        fs->read_only = 1; /* never commit a partially cloned snapshot */
        return ret;
    }

    ret = vtx_subvol_switch(fs, snap_id);
    if (ret)
        return ret;
    fs->active_subvol.num_inodes = src_sub.num_inodes;
    fs->active_subvol.directory_bytes = src_sub.directory_bytes;
    memcpy(fs->active_subvol.parent_uuid, src_sub.uuid, VTXFS_UUID_SIZE);
    fs->active_subvol.last_snap_xid = vtx_le64(fs->current_xid);
    if (read_only)
        fs->active_subvol.flags = vtx_le64(VTX_SUBVOL_RDONLY);

    *snap_subvol_id_out = snap_id;
    return vtx_subvol_switch(fs, src_subvol_id);
}

int vtx_subvol_destroy(struct vtx_fs *fs, __u64 subvol_id)
{
    struct vtx_key k;
    struct vtx_subvol_root_item sub;
    struct vtx_reaper_job job;
    size_t bytes = sizeof(sub);
    __u64 job_id;
    int ret;

    if (!fs || !fs->mounted || subvol_id <= VTXFS_OID_FIRST_USER_SUBVOL)
        return -EINVAL;
    if (fs->read_only || fs->view_only)
        return -EROFS;
    if (fs->active_subvol_id == subvol_id) {
        ret = vtx_subvol_switch(fs, VTXFS_OID_FIRST_USER_SUBVOL);
        if (ret)
            return ret;
    }

    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(subvol_id, VTX_TYPE_SUBVOL_ROOT));
    k.offset = 0;
    ret = vtx_rw_lookup(&fs->root_tree, &k, &sub, &bytes);
    if (ret)
        return ret;
    sub.flags = vtx_le64(vtx_cpu64(sub.flags) | VTX_SUBVOL_REAPING);
    ret = vtx_rw_replace(&fs->root_tree, &k, &sub, sizeof(sub));
    if (ret)
        return ret;

    job_id = fs->next_oid++;
    memset(&job, 0, sizeof(job));
    job.job_id = vtx_le64(job_id);
    job.object_id = vtx_le64(subvol_id);
    job.owning_root_id = vtx_le64(0);
    job.phase = vtx_le32(VTX_REAPER_QUEUED);
    job.operation = vtx_le32(VTX_REAPER_OP_DROP_SUBVOL);

    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(job_id, VTX_TYPE_REAPER_JOB));
    k.offset = 0;
    ret = vtx_rw_insert(&fs->reaper_tree, &k, &job, sizeof(job));
    if (ret)
        return ret;
    fs->cp_blk.hdr.cp_flags |= vtx_le32(VTXFS_CP_FLAG_REAPER_PENDING);
    return 0;
}

/* -------------------------------------------------------------------------
 * 2. Directory Helpers, Mkdir, Create, Symlink, Lookup, Readdir
 * ------------------------------------------------------------------------- */

static int dir_find_entry(struct vtx_fs *fs, __u64 dir_ino,
                          const char *name, size_t nlen,
                          struct vtx_key *dir_key_out,
                          struct vtx_dir_item *hdr_out)
{
    __u64 hash48 = 0;
    __u32 low32 = 0;
    struct vtx_key start_k, found_k;
    __u8 buf[sizeof(struct vtx_dir_item) + VTXFS_NAME_MAX];
    size_t bytes;
    int first = 1, ret;

    if (!name || !nlen || nlen > VTXFS_NAME_MAX)
        return -EINVAL;
    vtx_dir_name_hash(fs->super_blk.hdr.dir_hash_seed, name, nlen,
                      &hash48, &low32);
    start_k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(dir_ino, VTX_TYPE_DIR_ITEM));
    start_k.offset = vtx_le64(VTX_DIR_MAKE_OFFSET(hash48, 0));

    for (;;) {
        const struct vtx_dir_item *d;
        __u64 off;
        bytes = sizeof(buf);
        ret = vtx_rw_next(&fs->active_fs_tree, &start_k, first,
                          &found_k, buf, &bytes);
        if (ret == -ENOENT)
            return -ENOENT;
        if (ret == 0 || ret == -ENOBUFS) {
            if (VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != dir_ino ||
                VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) !=
                    VTX_TYPE_DIR_ITEM)
                return -ENOENT;
        }
        if (ret)
            return ret;
        first = 0;
        start_k = found_k;
        off = vtx_cpu64(found_k.offset);
        if ((off >> 16) != hash48)
            return -ENOENT;
        d = (const struct vtx_dir_item *)buf;
        if (!vtx_dir_item_valid(d, bytes))
            return -EUCLEAN;
        if (vtx_cpu16(d->name_len) == nlen &&
            memcmp(d->name, name, nlen) == 0) {
            if (dir_key_out)
                *dir_key_out = found_k;
            if (hdr_out)
                *hdr_out = *d;
            return 0;
        }
    }
}

static int dir_add_entry(struct vtx_fs *fs, __u64 dir_ino,
                         __u64 target_ino, const char *name,
                         __u8 ftype)
{
    size_t nlen = name ? strlen(name) : 0;
    __u8 d_buf[sizeof(struct vtx_dir_item) + VTXFS_NAME_MAX];
    __u8 ref_buf[sizeof(struct vtx_inode_ref) + VTXFS_NAME_MAX];
    struct vtx_dir_item *d = (struct vtx_dir_item *)d_buf;
    struct vtx_inode_ref *ir = (struct vtx_inode_ref *)ref_buf;
    struct vtx_dir_index_val idx;
    struct vtx_inode_item dir_it;
    struct vtx_key k;
    __u64 hash48 = 0, dir_seq, hash_off = 0;
    __u32 low32 = 0, slot;
    int ret;

    if (!name || !nlen || nlen > VTXFS_NAME_MAX)
        return -EINVAL;
    ret = load_inode(fs, dir_ino, &dir_it);
    if (ret)
        return ret;
    if ((vtx_cpu32(dir_it.mode) & 0170000U) != 0040000U)
        return -ENOTDIR;

    if (dir_find_entry(fs, dir_ino, name, nlen, NULL, NULL) == 0)
        return -EEXIST;

    vtx_dir_name_hash(fs->super_blk.hdr.dir_hash_seed, name, nlen,
                      &hash48, &low32);
    dir_seq = fs->next_oid++;

    memset(d_buf, 0, sizeof(d_buf));
    d->target_ino = vtx_le64(target_ino);
    d->dir_seq = vtx_le64(dir_seq);
    d->name_len = vtx_le16((__u16)nlen);
    d->file_type = ftype;
    d->flags = 0;
    d->name_hash_low32 = vtx_le32(low32);
    memcpy(d->name, name, nlen);
    if (!vtx_dir_item_valid(d, sizeof(*d) + nlen))
        return -EINVAL;

    for (slot = 0; slot < 65536U; slot++) {
        size_t dummy = 0;
        hash_off = VTX_DIR_MAKE_OFFSET(hash48, slot);
        k.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(dir_ino, VTX_TYPE_DIR_ITEM));
        k.offset = vtx_le64(hash_off);
        ret = vtx_rw_lookup(&fs->active_fs_tree, &k, NULL, &dummy);
        if (ret == -ENOENT)
            break;
    }
    if (slot == 65536U)
        return -ENOSPC;

    ret = vtx_rw_insert(&fs->active_fs_tree, &k, d_buf,
                        (__u16)(sizeof(*d) + nlen));
    if (ret)
        return ret;

    memset(&idx, 0, sizeof(idx));
    idx.target_ino = vtx_le64(target_ino);
    idx.dir_hash_offset = vtx_le64(hash_off);
    idx.name_len = vtx_le16((__u16)nlen);
    idx.file_type = ftype;
    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(dir_ino, VTX_TYPE_DIR_INDEX));
    k.offset = vtx_le64(dir_seq);
    ret = vtx_rw_insert(&fs->active_fs_tree, &k, &idx, sizeof(idx));
    if (ret)
        return ret;

    memset(ref_buf, 0, sizeof(ref_buf));
    ir->dir_seq = vtx_le64(dir_seq);
    ir->name_len = vtx_le16((__u16)nlen);
    memcpy(ir->name, name, nlen);
    k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(target_ino, VTX_TYPE_INODE_REF));
    k.offset = vtx_le64(dir_ino);
    ret = vtx_rw_upsert(&fs->active_fs_tree, &k, ref_buf,
                        (__u16)(sizeof(*ir) + nlen));
    if (ret)
        return ret;

    dir_it.u.dir_total_bytes =
        vtx_le64(vtx_cpu64(dir_it.u.dir_total_bytes) + nlen);
    {
        __u64 now = vtx_time_now_ns();
        dir_it.mtime_ns = vtx_le64(now);
        dir_it.ctime_ns = vtx_le64(now);
    }
    return save_inode(fs, dir_ino, &dir_it);
}

static int create_common(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
                         const char *name, __u32 mode, __u8 ftype,
                         __u64 *ino_out)
{
    __u64 ino;
    struct vtx_inode_item it;
    int ret;

    ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;

    ino = fs->next_oid++;
    if (ino < VTXFS_INO_FIRST_USER) {
        ino = VTXFS_INO_FIRST_USER;
        fs->next_oid = ino + 1ULL;
    }

    memset(&it, 0, sizeof(it));
    it.parent_ino = vtx_le64(parent_ino);
    it.update_xid = vtx_le64(fs->current_xid);
    it.mode = vtx_le32(mode);
    it.nlink = vtx_le32(1);
    {
        __u64 now = vtx_time_now_ns();
        it.atime_ns = vtx_le64(now);
        it.mtime_ns = vtx_le64(now);
        it.ctime_ns = vtx_le64(now);
        it.btime_ns = vtx_le64(now);
    }

    ret = save_inode(fs, ino, &it);
    if (ret)
        return ret;
    ret = dir_add_entry(fs, parent_ino, ino, name, ftype);
    if (ret) {
        struct vtx_key dk;
        dk.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_INODE));
        dk.offset = 0;
        (void)vtx_rw_delete(&fs->active_fs_tree, &dk);
        return ret;
    }

    fs->active_subvol.num_inodes =
        vtx_le64(vtx_cpu64(fs->active_subvol.num_inodes) + 1ULL);
    if (ino_out)
        *ino_out = ino;
    return 0;
}

int vtx_mkdir(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
              const char *name, __u32 mode, __u64 *ino_out)
{
    return create_common(fs, subvol_id, parent_ino, name,
                         (mode & 07777U) | 0040000U, VTX_FT_DIR, ino_out);
}

int vtx_create(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
               const char *name, __u32 mode, __u64 *ino_out)
{
    return create_common(fs, subvol_id, parent_ino, name,
                         (mode & 07777U) | 0100000U, VTX_FT_REG, ino_out);
}

int vtx_symlink(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
                const char *name, const char *target, __u64 *ino_out)
{
    __u64 ino = 0;
    size_t tlen = target ? strlen(target) : 0;
    int ret;

    if (!target || !tlen || tlen > VTXFS_MAX_INLINE_DATA)
        return -EINVAL;
    ret = create_common(fs, subvol_id, parent_ino, name,
                        0120777U, VTX_FT_SYMLINK, &ino);
    if (ret)
        return ret;
    ret = vtx_write(fs, subvol_id, ino, 0, target, tlen);
    if (ret)
        return ret;
    if (ino_out)
        *ino_out = ino;
    return 0;
}

int vtx_readlink(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 char *buf, size_t buf_size, size_t *len_out)
{
    struct vtx_inode_item it;
    size_t got = 0;
    int ret = enter_subvol(fs, subvol_id, 0);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;
    if ((vtx_cpu32(it.mode) & 0170000U) != 0120000U)
        return -EINVAL;
    if (!buf || buf_size < vtx_cpu64(it.size_bytes) + 1U)
        return -ENOBUFS;
    ret = vtx_read(fs, subvol_id, ino, 0, buf, vtx_cpu64(it.size_bytes), &got);
    if (ret)
        return ret;
    buf[got] = '\0';
    if (len_out)
        *len_out = got;
    return 0;
}

int vtx_lookup(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
               const char *name, __u64 *ino_out, __u8 *ftype_out)
{
    struct vtx_dir_item d;
    int ret = enter_subvol(fs, subvol_id, 0);
    if (ret)
        return ret;
    if (!name)
        return -EINVAL;
    ret = dir_find_entry(fs, parent_ino, name, strlen(name), NULL, &d);
    if (ret)
        return ret;
    if (ino_out)
        *ino_out = vtx_cpu64(d.target_ino);
    if (ftype_out)
        *ftype_out = d.file_type;
    return 0;
}

int vtx_lookup_path(struct vtx_fs *fs, __u64 subvol_id, const char *path,
                    __u64 *ino_out, __u8 *ftype_out)
{
    __u64 cur_ino = VTXFS_INO_ROOT_DIR;
    __u8 cur_ft = VTX_FT_DIR;
    const char *p = path;
    char comp[VTXFS_NAME_MAX + 1];
    int ret = enter_subvol(fs, subvol_id, 0);

    if (ret)
        return ret;
    if (!path || !*path)
        return -EINVAL;
    while (*p == '/')
        p++;
    while (*p) {
        size_t len = 0;
        while (p[len] && p[len] != '/') {
            if (len >= VTXFS_NAME_MAX)
                return -ENAMETOOLONG;
            comp[len] = p[len];
            len++;
        }
        comp[len] = '\0';
        ret = vtx_lookup(fs, subvol_id, cur_ino, comp, &cur_ino, &cur_ft);
        if (ret)
            return ret;
        p += len;
        while (*p == '/')
            p++;
    }
    if (ino_out)
        *ino_out = cur_ino;
    if (ftype_out)
        *ftype_out = cur_ft;
    return 0;
}

int vtx_stat(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
             struct vtx_inode_item *inode_out)
{
    int ret = enter_subvol(fs, subvol_id, 0);
    if (ret)
        return ret;
    if (!inode_out)
        return -EINVAL;
    return load_inode(fs, ino, inode_out);
}

int vtx_chmod(struct vtx_fs *fs, __u64 subvol_id, __u64 ino, __u32 mode)
{
    struct vtx_inode_item it;
    int ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;
    it.mode = vtx_le32((vtx_cpu32(it.mode) & 0170000U) | (mode & 07777U));
    return save_inode(fs, ino, &it);
}

int vtx_chown(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
              __u32 uid, __u32 gid)
{
    struct vtx_inode_item it;
    int ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;
    it.uid = vtx_le32(uid);
    it.gid = vtx_le32(gid);
    return save_inode(fs, ino, &it);
}

int vtx_utimes(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
               __u64 atime_ns, __u64 mtime_ns)
{
    struct vtx_inode_item it;
    int ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;
    it.atime_ns = vtx_le64(atime_ns);
    it.mtime_ns = vtx_le64(mtime_ns);
    return save_inode(fs, ino, &it);
}

int vtx_readdir(struct vtx_fs *fs, __u64 subvol_id, __u64 dir_ino,
                __u64 *cookie_inout, struct vtx_dirent *entries,
                size_t max_entries, size_t *count_out)
{
    struct vtx_key cur_k, found_k, d_k;
    struct vtx_dir_index_val idx;
    __u8 d_buf[sizeof(struct vtx_dir_item) + VTXFS_NAME_MAX];
    size_t bytes, count = 0;
    int inclusive, ret;

    if (!cookie_inout || !count_out || (max_entries && !entries))
        return -EINVAL;
    ret = enter_subvol(fs, subvol_id, 0);
    if (ret)
        return ret;

    cur_k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(dir_ino, VTX_TYPE_DIR_INDEX));
    cur_k.offset = vtx_le64(*cookie_inout);
    inclusive = (*cookie_inout == 0) ? 1 : 0;

    while (count < max_entries) {
        const struct vtx_dir_item *d;
        __u16 nlen;
        bytes = sizeof(idx);
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, inclusive,
                          &found_k, &idx, &bytes);
        if (ret == -ENOENT)
            break;
        if (ret == 0 || ret == -ENOBUFS) {
            if (VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != dir_ino ||
                VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) !=
                    VTX_TYPE_DIR_INDEX)
                break;
        }
        if (ret)
            return ret;
        inclusive = 0;
        cur_k = found_k;
        if (bytes != sizeof(idx))
            return -EUCLEAN;

        d_k.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(dir_ino, VTX_TYPE_DIR_ITEM));
        d_k.offset = idx.dir_hash_offset;
        bytes = sizeof(d_buf);
        ret = vtx_rw_lookup(&fs->active_fs_tree, &d_k, d_buf, &bytes);
        if (ret)
            return ret;
        d = (const struct vtx_dir_item *)d_buf;
        if (!vtx_dir_item_valid(d, bytes))
            return -EUCLEAN;

        nlen = vtx_cpu16(d->name_len);
        entries[count].ino = vtx_cpu64(d->target_ino);
        entries[count].dir_seq = vtx_cpu64(found_k.offset);
        entries[count].file_type = d->file_type;
        memcpy(entries[count].name, d->name, nlen);
        entries[count].name[nlen] = '\0';
        *cookie_inout = entries[count].dir_seq;
        count++;
    }
    *count_out = count;
    return 0;
}

/* Tìm tên của một subvolume bằng cách quét dirent VTX_FT_SUBVOL trong
 * thư mục gốc của subvolume cha. Trả -ENOENT với subvolume mặc định
 * (không có dirent) hoặc subvolume tạo trước bản vá này. */
int vtx_subvol_get_name(struct vtx_fs *fs, __u64 subvol_id,
			char *name_out, size_t name_buf_size)
{
	struct vtx_key k;
	struct vtx_subvol_root_item sub;
	size_t bytes = sizeof(sub);
	__u64 parent_id, saved_subvol;
	struct vtx_dirent *ents;
	__u64 cookie = 0;
	size_t count = 0, i;
	int ret;

	if (!fs || !fs->mounted || !name_out || name_buf_size == 0)
		return -EINVAL;
	k.oid_and_type =
		vtx_le64(VTX_MAKE_OID_TYPE(subvol_id, VTX_TYPE_SUBVOL_ROOT));
	k.offset = 0;
	ret = vtx_rw_lookup(&fs->root_tree, &k, &sub, &bytes);
	if (ret)
		return ret;
	parent_id = vtx_cpu64(sub.parent_subvol_id);
	if (parent_id == 0)
		return -ENOENT; /* subvolume mặc định */

	ents = (struct vtx_dirent *)vtx_zalloc(sizeof(*ents) * 64);
	if (!ents)
		return -ENOMEM;

	saved_subvol = fs->active_subvol_id;
	ret = vtx_subvol_switch(fs, parent_id);
	if (ret) {
		vtx_free(ents);
		return ret;
	}
	for (;;) {
		ret = vtx_readdir(fs, parent_id, VTXFS_INO_ROOT_DIR,
				  &cookie, ents, 64, &count);
		if (ret)
			break;
		for (i = 0; i < count; i++) {
			if (ents[i].file_type == VTX_FT_SUBVOL &&
			    ents[i].ino == subvol_id) {
				size_t nlen = strlen(ents[i].name);

				if (nlen >= name_buf_size)
					nlen = name_buf_size - 1U;
				memcpy(name_out, ents[i].name, nlen);
				name_out[nlen] = '\0';
				if (saved_subvol)
					vtx_subvol_switch(fs, saved_subvol);
				vtx_free(ents);
				return 0;
			}
		}
		if (count < 64)
			break;
	}
	if (saved_subvol)
		vtx_subvol_switch(fs, saved_subvol);
	vtx_free(ents);
	return -ENOENT;
}

/* -------------------------------------------------------------------------
 * 3. File Extents: Read, Write, Extent Split/Drop, Truncate, Reflink
 * ------------------------------------------------------------------------- */

static int read_single_data_block(struct vtx_fs *fs, __u64 vblk, void *out_buf)
{
    __u32 i;
    __u64 pblk = 0;
    int ret;

    /* Check uncommitted pending data writes in reverse order. */
    for (i = fs->num_pending_data; i > 0; i--) {
        const struct vtx_pending_data_write *pw = &fs->pending_data[i - 1U];
        if (vblk >= pw->vblk && vblk < pw->vblk + pw->block_count) {
            size_t off = (size_t)(vblk - pw->vblk) * VTXFS_BLOCK_SIZE;
            memcpy(out_buf, pw->data + off, VTXFS_BLOCK_SIZE);
            return 0;
        }
    }
    ret = vtx_vag_vblk_to_pblk(&fs->vag, vblk, &pblk);
    if (ret)
        return ret;
    return fs->bdev->ops->read_blocks(fs->bdev, pblk, 1, out_buf);
}

int vtx_read(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
             __u64 file_offset, void *buf, size_t len, size_t *bytes_read_out)
{
    struct vtx_inode_item it;
    __u64 file_size, read_end;
    struct vtx_key cur_k, found_k;
    __u8 *ebuf = NULL, *blk_buf = NULL;
    size_t bytes;
    int first = 1, ret;

    if (!buf && len)
        return -EINVAL;
    ret = enter_subvol(fs, subvol_id, 0);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;

    file_size = vtx_cpu64(it.size_bytes);
    if (file_offset >= file_size || len == 0) {
        if (bytes_read_out)
            *bytes_read_out = 0;
        return 0;
    }
    if (len > file_size - file_offset)
        len = (size_t)(file_size - file_offset);

    memset(buf, 0, len);
    read_end = file_offset + len;

    ebuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    blk_buf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!ebuf || !blk_buf) {
        vtx_free(ebuf);
        vtx_free(blk_buf);
        return -ENOMEM;
    }

    cur_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
    cur_k.offset = vtx_le64(file_offset > VTXFS_EXTENT_LOOKBACK_BYTES ?
                            file_offset - VTXFS_EXTENT_LOOKBACK_BYTES : 0ULL);

    for (;;) {
        const struct vtx_extent_item *ext;
        __u64 ext_start, ext_end;
        __u32 flags, phys_blks, b;

        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first,
                          &found_k, ebuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != ino ||
            VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) !=
                VTX_TYPE_EXTENT)
            break;

        ext = (const struct vtx_extent_item *)ebuf;
        if (!vtx_extent_p0_value_valid(ext, bytes)) {
            ret = -EUCLEAN;
            break;
        }
        ext_start = vtx_cpu64(found_k.offset);
        flags = vtx_cpu32(ext->flags);
        if (flags & VTX_EXTENT_INLINE) {
            __u32 pay_len = vtx_cpu32(ext->payload_bytes);
            ext_end = ext_start + pay_len;
            if (ext_end > file_offset && ext_start < read_end) {
                __u64 ov_s = ext_start > file_offset ? ext_start : file_offset;
                __u64 ov_e = ext_end < read_end ? ext_end : read_end;
                memcpy((__u8 *)buf + (ov_s - file_offset),
                       ext->tail + (ov_s - ext_start),
                       (size_t)(ov_e - ov_s));
            }
            continue;
        }

        if (flags & VTX_EXTENT_COMPRESSED) {
            __u32 phys_blks = vtx_cpu32(ext->physical_blocks);
            __u32 logical_bytes = vtx_cpu32(ext->logical_bytes);
            size_t comp_buf_sz = (size_t)phys_blks * VTXFS_BLOCK_SIZE;
            __u8 *raw_comp;
            __u8 *decomp = NULL;
            __u32 decomp_len = 0;

            ext_end = ext_start + logical_bytes;
            if (ext_end <= file_offset)
                continue;
            if (ext_start >= read_end)
                break;

            raw_comp = (__u8 *)vtx_zalloc(comp_buf_sz);
            if (!raw_comp) {
                ret = -ENOMEM;
                break;
            }
            for (b = 0; b < phys_blks; b++) {
                __le32 stored_le;
                ret = read_single_data_block(fs, vtx_cpu64(ext->vblk) + b,
                                             raw_comp + (size_t)b * VTXFS_BLOCK_SIZE);
                if (ret)
                    break;
                memcpy(&stored_le, ext->tail + b * sizeof(__le32), sizeof(stored_le));
                if (vtx_crc32c(raw_comp + (size_t)b * VTXFS_BLOCK_SIZE, VTXFS_BLOCK_SIZE) !=
                    vtx_cpu32(stored_le)) {
                    ret = -EUCLEAN;
                    break;
                }
            }
            if (ret) {
                vtx_free(raw_comp);
                break;
            }

            ret = vtx_decompress_extent(raw_comp, (__u32)comp_buf_sz, &decomp, &decomp_len);
            vtx_free(raw_comp);
            if (ret)
                break;

            __u64 ov_s = ext_start > file_offset ? ext_start : file_offset;
            __u64 ov_e = ext_end < read_end ? ext_end : read_end;

            if (decomp_len < logical_bytes ||
                (ov_e > ov_s && (ov_s - ext_start) + (ov_e - ov_s) > decomp_len)) {
                vtx_buffer_free(decomp);
                ret = -EUCLEAN;
                break;
            }

            if (ov_e > ov_s) {
                memcpy((__u8 *)buf + (ov_s - file_offset),
                       decomp + (ov_s - ext_start),
                       (size_t)(ov_e - ov_s));
            }
            vtx_buffer_free(decomp);
            continue;
        }

        phys_blks = vtx_cpu32(ext->physical_blocks);
        ext_end = ext_start + (__u64)phys_blks * VTXFS_BLOCK_SIZE;
        if (ext_end <= file_offset)
            continue;
        if (ext_start >= read_end)
            break;

        for (b = 0; b < phys_blks; b++) {
            __u64 b_start = ext_start + (__u64)b * VTXFS_BLOCK_SIZE;
            __u64 b_end = b_start + VTXFS_BLOCK_SIZE;
            __le32 stored_le;
            __u32 expected_crc, actual_crc;

            if (b_end <= file_offset || b_start >= read_end)
                continue;
            ret = read_single_data_block(fs, vtx_cpu64(ext->vblk) + b, blk_buf);
            if (ret)
                break;
            memcpy(&stored_le, ext->tail + b * sizeof(__le32),
                   sizeof(stored_le));
            expected_crc = vtx_cpu32(stored_le);
            actual_crc = vtx_crc32c(blk_buf, VTXFS_BLOCK_SIZE);
            if (actual_crc != expected_crc) {
                ret = -EUCLEAN;
                break;
            }
            {
                __u64 ov_s = b_start > file_offset ? b_start : file_offset;
                __u64 ov_e = b_end < read_end ? b_end : read_end;
                memcpy((__u8 *)buf + (ov_s - file_offset),
                       blk_buf + (ov_s - b_start),
                       (size_t)(ov_e - ov_s));
            }
        }
        if (ret)
            break;
    }

    vtx_free(ebuf);
    vtx_free(blk_buf);
    if (ret == 0 && bytes_read_out)
        *bytes_read_out = len;
    return ret;
}

static int build_and_insert_block_extent_ex(struct vtx_fs *fs, __u64 ino,
                                            __u64 file_off, __u64 vblk,
                                            __u64 alloc_id, __u32 phys_blks,
                                            __u32 logical_bytes,
                                            __u32 flags, __u8 compression,
                                            const __le32 *csums)
{
    struct vtx_extent_item *e;
    struct vtx_key k;
    size_t total_len = sizeof(*e) + (size_t)phys_blks * sizeof(__le32);
    __u8 *ebuf;
    int ret;

    if (!phys_blks || phys_blks > 256U)
        return -EINVAL;
    ebuf = (__u8 *)vtx_zalloc(total_len);
    if (!ebuf)
        return -ENOMEM;
    e = (struct vtx_extent_item *)ebuf;
    e->vblk = vtx_le64(vblk);
    e->allocation_id = vtx_le64(alloc_id);
    e->birth_xid = vtx_le64(fs->current_xid);
    e->owner_subvol_id = vtx_le64(fs->active_subvol_id);
    e->physical_blocks = vtx_le32(phys_blks);
    e->logical_bytes = vtx_le32(logical_bytes ? logical_bytes : (phys_blks * VTXFS_BLOCK_SIZE));
    e->flags = vtx_le32(flags);
    e->csum_type = vtx_le16(VTX_CSUM_CRC32C);
    e->csum_count = vtx_le16((__u16)phys_blks);
    e->payload_bytes = vtx_le32(0);
    e->compression = compression;
    e->encryption = VTX_ENC_NONE;
    e->header_bytes = vtx_le16((__u16)sizeof(*e));
    e->crypto_identity = vtx_le64(0);
    memcpy(e->tail, csums, (size_t)phys_blks * sizeof(__le32));

    if (!vtx_extent_p0_value_valid(e, total_len)) {
        vtx_free(ebuf);
        return -EUCLEAN;
    }

    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
    k.offset = vtx_le64(file_off);
    ret = vtx_rw_upsert(&fs->active_fs_tree, &k, ebuf, (__u16)total_len);
    vtx_free(ebuf);
    return ret;
}

static int build_and_insert_block_extent(struct vtx_fs *fs, __u64 ino,
                                         __u64 file_off, __u64 vblk,
                                         __u64 alloc_id, __u32 phys_blks,
                                         __u32 flags,
                                         const __le32 *csums)
{
    return build_and_insert_block_extent_ex(fs, ino, file_off, vblk, alloc_id,
                                            phys_blks, phys_blks * VTXFS_BLOCK_SIZE,
                                            flags, VTX_COMPR_NONE, csums);
}

/* Drop or split extents overlapping [drop_start, drop_end) (4K aligned). */
static int drop_extent_range(struct vtx_fs *fs, __u64 ino,
                             __u64 drop_start, __u64 drop_end)
{
    struct vtx_key cur_k, found_k;
    __u8 *ebuf;
    size_t bytes;
    int first = 1, ret = 0;

    if (drop_start >= drop_end)
        return 0;
    ebuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!ebuf)
        return -ENOMEM;

    cur_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
    cur_k.offset = vtx_le64(drop_start > VTXFS_EXTENT_LOOKBACK_BYTES ?
                            drop_start - VTXFS_EXTENT_LOOKBACK_BYTES : 0ULL);

    for (;;) {
        const struct vtx_extent_item *ext;
        __u64 ext_start, ext_end, vblk, alloc_id;
        __u32 flags, phys_blks;
        const __le32 *csums;

        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first,
                          &found_k, ebuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != ino ||
            VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) !=
                VTX_TYPE_EXTENT)
            break;

        ext = (const struct vtx_extent_item *)ebuf;
        ext_start = vtx_cpu64(found_k.offset);
        flags = vtx_cpu32(ext->flags);
        if (flags & VTX_EXTENT_INLINE) {
            if (drop_start == 0) {
                ret = vtx_rw_delete(&fs->active_fs_tree, &found_k);
                if (ret)
                    break;
            }
            continue;
        }

        phys_blks = vtx_cpu32(ext->physical_blocks);
        ext_end = ext_start + ((flags & VTX_EXTENT_COMPRESSED) ?
                               (__u64)vtx_cpu32(ext->logical_bytes) :
                               ((__u64)phys_blks * VTXFS_BLOCK_SIZE));
        if (ext_end <= drop_start)
            continue;
        if (ext_start >= drop_end)
            break;

        vblk = vtx_cpu64(ext->vblk);
        alloc_id = vtx_cpu64(ext->allocation_id);
        csums = (const __le32 *)ext->tail;

        if (ext_start >= drop_start && ext_end <= drop_end) {
            /* Entire extent covered: delete and decrement refcount. */
            ret = vtx_rw_delete(&fs->active_fs_tree, &found_k);
            if (ret)
                break;
            ret = vtx_enqueue_delayed_ref(fs, alloc_id, vblk, phys_blks,
                                          VTX_REF_FLAG_DATA,
                                          (__u32)fs->active_subvol_id, -1);
            if (ret)
                break;
        } else if (flags & VTX_EXTENT_COMPRESSED) {
            /* Overlapping compressed extent: delete and decrement refcount. */
            ret = vtx_rw_delete(&fs->active_fs_tree, &found_k);
            if (ret)
                break;
            ret = vtx_enqueue_delayed_ref(fs, alloc_id, vblk, phys_blks,
                                          VTX_REF_FLAG_DATA,
                                          (__u32)fs->active_subvol_id, -1);
            if (ret)
                break;
        } else if (ext_start < drop_start && ext_end <= drop_end) {
            /* Trim right tail: keep left prefix [ext_start, drop_start). */
            __u32 keep = (__u32)((drop_start - ext_start) / VTXFS_BLOCK_SIZE);
            ret = build_and_insert_block_extent(fs, ino, ext_start, vblk,
                                                alloc_id, keep, flags, csums);
            if (ret)
                break;
        } else if (ext_start >= drop_start && ext_end > drop_end) {
            /* Trim left head: keep right suffix [drop_end, ext_end). */
            __u32 skip = (__u32)((drop_end - ext_start) / VTXFS_BLOCK_SIZE);
            __u32 keep = phys_blks - skip;
            ret = vtx_rw_delete(&fs->active_fs_tree, &found_k);
            if (ret)
                break;
            ret = build_and_insert_block_extent(fs, ino, drop_end,
                                                vblk + skip, alloc_id,
                                                keep, flags, csums + skip);
            if (ret)
                break;
            cur_k.offset = vtx_le64(drop_end);
        } else {
            /* Split into left [ext_start, drop_start) & right [drop_end, ext_end). */
            __u32 left_b = (__u32)((drop_start - ext_start) / VTXFS_BLOCK_SIZE);
            __u32 right_off = (__u32)((drop_end - ext_start) / VTXFS_BLOCK_SIZE);
            __u32 right_b = phys_blks - right_off;
            /* Insert both extent pieces first; only enqueue delayed ref +1 if insertion succeeds.
             * This prevents dangling extra reference on insertion failure. */
            ret = build_and_insert_block_extent(
                fs, ino, ext_start, vblk, alloc_id, left_b,
                flags, csums);
            if (ret)
                break;
            ret = build_and_insert_block_extent(
                fs, ino, drop_end, vblk + right_off, alloc_id, right_b,
                flags, csums + right_off);
            if (ret)
                break;
            ret = vtx_enqueue_delayed_ref(fs, alloc_id, vblk, phys_blks,
                                          VTX_REF_FLAG_DATA,
                                          (__u32)fs->active_subvol_id, 1);
            if (ret)
                break;
            cur_k.offset = vtx_le64(drop_end);
        }
    }

    vtx_free(ebuf);
    if (ret)
        fs->read_only = 1; /* partial CoW mutations cannot be rolled back */
    return ret;
}

static int promote_inline_if_present(struct vtx_fs *fs, __u64 ino)
{
    struct vtx_key k;
    __u8 *ebuf = NULL, *blk = NULL;
    const struct vtx_extent_item *ext;
    size_t bytes = VTXFS_BLOCK_SIZE;
    __u64 nv = 0, alloc_id;
    __le32 crc_le;
    int ret;

    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
    k.offset = 0;
    ebuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!ebuf)
        return -ENOMEM;
    ret = vtx_rw_lookup(&fs->active_fs_tree, &k, ebuf, &bytes);
    if (ret == -ENOENT) {
        vtx_free(ebuf);
        return 0;
    }
    if (ret) {
        vtx_free(ebuf);
        return ret;
    }
    ext = (const struct vtx_extent_item *)ebuf;
    if (!(vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)) {
        vtx_free(ebuf);
        return 0;
    }

    if (fs->num_pending_data >= VTX_MAX_PENDING_DATA) {
        ret = vtx_sync_checkpoint(fs);
        if (ret) {
            vtx_free(ebuf);
            return ret;
        }
    }

    blk = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!blk) {
        vtx_free(ebuf);
        return -ENOMEM;
    }
    memcpy(blk, ext->tail, vtx_cpu32(ext->payload_bytes));
    vtx_free(ebuf);

    ret = vtx_vag_alloc_range(&fs->vag, 1, &nv);
    if (ret) {
        vtx_free(blk);
        return ret;
    }
    alloc_id = fs->next_oid++;
    crc_le = vtx_le32(vtx_crc32c(blk, VTXFS_BLOCK_SIZE));

    ret = vtx_queue_data_write(fs, nv, 1, blk);
    vtx_free(blk);
    if (ret)
        return ret;
    ret = vtx_enqueue_delayed_ref(fs, alloc_id, nv, 1,
                                  VTX_REF_FLAG_DATA,
                                  (__u32)fs->active_subvol_id, 1);
    if (ret)
        return ret;
    return build_and_insert_block_extent(fs, ino, 0, nv, alloc_id, 1, 0,
                                         &crc_le);
}

int vtx_write(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
              __u64 file_offset, const void *data, size_t len)
{
    struct vtx_inode_item it;
    __u64 old_size, new_end;
    int ret;

    if (!len)
        return 0;
    if (!data)
        return -EINVAL;
    ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;

    old_size = vtx_cpu64(it.size_bytes);
    new_end = file_offset + len;

    /* Use inline extent if total span fits in VTXFS_MAX_INLINE_DATA (2048 B). */
    if (new_end <= VTXFS_MAX_INLINE_DATA &&
        old_size <= VTXFS_MAX_INLINE_DATA) {
        struct vtx_key k;
        __u8 *ibuf = (__u8 *)vtx_zalloc(sizeof(struct vtx_extent_item) +
                                        VTXFS_MAX_INLINE_DATA);
        struct vtx_extent_item *e;
        __u32 pay_len = (__u32)(new_end > old_size ? new_end : old_size);
        size_t got = 0;

        if (!ibuf)
            return -ENOMEM;
        e = (struct vtx_extent_item *)ibuf;
        if (old_size > 0) {
            ret = vtx_read(fs, subvol_id, ino, 0, e->tail,
                           (size_t)old_size, &got);
            if (ret) {
                vtx_free(ibuf);
                return ret;
            }
        }
        /* Ensure any previous block extent at 0 is dropped before inline upsert. */
        ret = drop_extent_range(fs, ino, 0, VTXFS_BLOCK_SIZE);
        if (ret) {
            vtx_free(ibuf);
            return ret;
        }
        memcpy(e->tail + file_offset, data, len);
        e->birth_xid = vtx_le64(fs->current_xid);
        e->owner_subvol_id = vtx_le64(subvol_id);
        e->logical_bytes = vtx_le32(pay_len);
        e->flags = vtx_le32(VTX_EXTENT_INLINE);
        e->csum_type = vtx_le16(VTX_CSUM_NONE);
        e->payload_bytes = vtx_le32(pay_len);
        e->header_bytes = vtx_le16((__u16)sizeof(*e));

        k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
        k.offset = 0;
        ret = vtx_rw_upsert(&fs->active_fs_tree, &k, ibuf,
                            (__u16)(sizeof(*e) + pay_len));
        vtx_free(ibuf);
        if (ret)
            return ret;

        {
            __u64 now = vtx_time_now_ns();
            it.mtime_ns = vtx_le64(now);
            it.ctime_ns = vtx_le64(now);
        }
        it.size_bytes = vtx_le64(pay_len);
        it.alloc_bytes = vtx_le64(pay_len);
        return save_inode(fs, ino, &it);
    }

    ret = promote_inline_if_present(fs, ino);
    if (ret)
        return ret;

#define VTX_WRITE_CHUNK_BLOCKS 256U
    /* Process write in chunks of at most 256 blocks (1024 KiB = 1 MiB). */
    {
        __u64 cur_off = file_offset;
        const __u8 *src = (const __u8 *)data;
        size_t rem = len;

        while (rem > 0) {
            __u64 al_start = cur_off & ~(__u64)(VTXFS_BLOCK_SIZE - 1U);
            size_t head_pad = (size_t)(cur_off - al_start);
            size_t max_payload = (size_t)VTX_WRITE_CHUNK_BLOCKS * VTXFS_BLOCK_SIZE - head_pad;
            size_t take = rem < max_payload ? rem : max_payload;
            __u64 al_end = (cur_off + take + VTXFS_BLOCK_SIZE - 1U) &
                           ~(__u64)(VTXFS_BLOCK_SIZE - 1U);
            __u32 nblks = (__u32)((al_end - al_start) / VTXFS_BLOCK_SIZE);
            __u8 *cbuf = (__u8 *)vtx_zalloc((size_t)nblks * VTXFS_BLOCK_SIZE);
            __le32 csums[VTX_WRITE_CHUNK_BLOCKS];
            __u64 nv = 0, alloc_id;
            __u32 b;

            if (!cbuf)
                return -ENOMEM;
            if (head_pad > 0 && al_start < old_size) {
                size_t got = 0;
                ret = vtx_read(fs, subvol_id, ino, al_start, cbuf,
                               VTXFS_BLOCK_SIZE, &got);
                if (ret) {
                    vtx_free(cbuf);
                    return ret;
                }
            }
            if (cur_off + take < al_end && cur_off + take < old_size) {
                __u64 last_blk = al_end - VTXFS_BLOCK_SIZE;
                size_t got = 0;
                ret = vtx_read(fs, subvol_id, ino, last_blk,
                               cbuf + (size_t)(nblks - 1U) * VTXFS_BLOCK_SIZE,
                               VTXFS_BLOCK_SIZE, &got);
                if (ret) {
                    vtx_free(cbuf);
                    return ret;
                }
            }

            memcpy(cbuf + head_pad, src, take);
            if (fs->num_pending_data >= VTX_MAX_PENDING_DATA) {
                ret = vtx_sync_checkpoint(fs);
                if (ret) {
                    vtx_free(cbuf);
                    return ret;
                }
            }
            ret = drop_extent_range(fs, ino, al_start, al_end);
            if (ret) {
                vtx_free(cbuf);
                return ret;
            }
            /* Compression check: only active if VTXFS_FEAT_INCOMPAT_COMPRESS is set */
            __u64 incompat = vtx_cpu64(fs->super_blk.hdr.incompat_flags);
            __u8 comp_algo = VTX_COMPR_NONE;
            if (incompat & VTXFS_FEAT_INCOMPAT_COMPRESS) {
                comp_algo = fs->super_blk.default_compression;
                if (comp_algo != VTX_COMPR_LZ4 && comp_algo != VTX_COMPR_ZLIB)
                    comp_algo = VTX_COMPR_ZSTD;
            }
            __u8 *comp_out = NULL;
            __u32 comp_len = 0;
            int did_compress = 0;

            if ((incompat & VTXFS_FEAT_INCOMPAT_COMPRESS) && nblks >= 1) {
                int level = (comp_algo == VTX_COMPR_LZ4) ? 1 : 3;
                ret = vtx_compress_extent(cbuf, nblks * VTXFS_BLOCK_SIZE,
                                          comp_algo, (__u8)level,
                                          &comp_out, &comp_len);
                if (ret == 0 && comp_len > 0) {
                    __u32 comp_blks = (comp_len + VTXFS_BLOCK_SIZE - 1U) / VTXFS_BLOCK_SIZE;
                    if (comp_blks < nblks) {
                        __u8 *comp_aligned = (__u8 *)vtx_zalloc((size_t)comp_blks * VTXFS_BLOCK_SIZE);
                        if (comp_aligned) {
                            memcpy(comp_aligned, comp_out, comp_len);
                            vtx_buffer_free(comp_out);
                            comp_out = NULL;

                            ret = vtx_vag_alloc_range(&fs->vag, comp_blks, &nv);
                            if (ret == 0) {
                                alloc_id = fs->next_oid++;
                                for (b = 0; b < comp_blks; b++) {
                                    __u32 crc = vtx_crc32c(comp_aligned + (size_t)b * VTXFS_BLOCK_SIZE,
                                                           VTXFS_BLOCK_SIZE);
                                    csums[b] = vtx_le32(crc);
                                }
                                ret = vtx_queue_data_write(fs, nv, comp_blks, comp_aligned);
                                vtx_free(comp_aligned);
                                if (ret) {
                                    vtx_free(cbuf);
                                    return ret;
                                }
                                ret = vtx_enqueue_delayed_ref(fs, alloc_id, nv, comp_blks,
                                                              VTX_REF_FLAG_DATA,
                                                              (__u32)subvol_id, 1);
                                if (ret) {
                                    vtx_free(cbuf);
                                    return ret;
                                }
                                ret = build_and_insert_block_extent_ex(
                                    fs, ino, al_start, nv, alloc_id, comp_blks,
                                    nblks * VTXFS_BLOCK_SIZE,
                                    VTX_EXTENT_COMPRESSED, comp_algo, csums);
                                vtx_free(cbuf);
                                if (ret)
                                    return ret;
                                did_compress = 1;
                            } else {
                                vtx_free(comp_aligned);
                            }
                        }
                    }
                }
                if (comp_out) {
                    vtx_buffer_free(comp_out);
                    comp_out = NULL;
                }
            }

            if (!did_compress) {
                ret = vtx_vag_alloc_range(&fs->vag, nblks, &nv);
                if (ret) {
                    vtx_free(cbuf);
                    return ret;
                }
                alloc_id = fs->next_oid++;
                for (b = 0; b < nblks; b++) {
                    __u32 crc = vtx_crc32c(cbuf + (size_t)b * VTXFS_BLOCK_SIZE,
                                           VTXFS_BLOCK_SIZE);
                    csums[b] = vtx_le32(crc);
                }
                ret = vtx_queue_data_write(fs, nv, nblks, cbuf);
                vtx_free(cbuf);
                if (ret)
                    return ret;
                ret = vtx_enqueue_delayed_ref(fs, alloc_id, nv, nblks,
                                              VTX_REF_FLAG_DATA,
                                              (__u32)subvol_id, 1);
                if (ret)
                    return ret;
                ret = build_and_insert_block_extent(fs, ino, al_start, nv,
                                                    alloc_id, nblks, 0, csums);
                if (ret)
                    return ret;
            }

            cur_off += take;
            src += take;
            rem -= take;
            if (cur_off > old_size)
                old_size = cur_off;
        }
    }

    {
        __u64 now = vtx_time_now_ns();
        it.mtime_ns = vtx_le64(now);
        it.ctime_ns = vtx_le64(now);
    }
    if (new_end > vtx_cpu64(it.size_bytes))
        it.size_bytes = vtx_le64(new_end);
    it.alloc_bytes = vtx_le64((vtx_cpu64(it.size_bytes) + VTXFS_BLOCK_SIZE - 1U) &
                              ~(__u64)(VTXFS_BLOCK_SIZE - 1U));
    return save_inode(fs, ino, &it);
}

/*
 * Real materialized preallocation (not an unwritten-extent promise): reserve
 * physical blocks for holes by writing zero-backed CRC-verified CoW extents.
 * Existing data is NOT touched: multi-block gap scanning skips allocated ranges,
 * and inline data is copied into a zero-padded physical first block.
 *
 * A v2.1 PREALLOC marker without a fully implemented read/recovery protocol
 * is deliberately never emitted; KEEP_SIZE and other fallocate modes remain
 * unsupported.  Each call is bounded to 1MiB for pending refs/CoW queues.
 */
#define VTX_FALLOC_ZERO_BUF_SIZE (32U * VTXFS_BLOCK_SIZE) /* 128 KiB */
#define VTX_FALLOC_MAX_GAPS 64

struct vtx_falloc_gap {
    __u64 start;
    __u64 end;
};

int vtx_fallocate_eager(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                        __u64 offset, __u64 len)
{
    struct vtx_inode_item it;
    struct vtx_key cur_k, found_k;
    struct vtx_falloc_gap gaps[VTX_FALLOC_MAX_GAPS];
    __u8 *ebuf = NULL, *zero_buf = NULL;
    __u64 end, old_size, cur_scan;
    size_t bytes;
    int num_gaps = 0, first = 1, ret, g;

    if (len == 0 || (offset & (VTXFS_BLOCK_SIZE - 1U)) ||
        (len & (VTXFS_BLOCK_SIZE - 1U)) || len > 1024U * 1024U ||
        offset > ~0ULL - len)
        return -EOPNOTSUPP;

    ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;
    if ((vtx_cpu32(it.mode) & 0170000U) != 0100000U)
        return -EINVAL;

    old_size = vtx_cpu64(it.size_bytes);
    end = offset + len;

    ret = promote_inline_if_present(fs, ino);
    if (ret)
        goto err;

    ebuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!ebuf) {
        ret = -ENOMEM;
        goto err;
    }

    /* Single-pass scan to identify unbacked ranges (gaps) in [offset, end) */
    cur_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
    cur_k.offset = vtx_le64(offset > VTXFS_EXTENT_LOOKBACK_BYTES ?
                            offset - VTXFS_EXTENT_LOOKBACK_BYTES : 0ULL);
    cur_scan = offset;

    for (;;) {
        const struct vtx_extent_item *ext;
        __u64 ext_start, ext_end;
        __u32 phys_blks;

        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first,
                          &found_k, ebuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            goto err;
        first = 0;
        cur_k = found_k;

        if (VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != ino ||
            VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) != VTX_TYPE_EXTENT)
            break;

        ext = (const struct vtx_extent_item *)ebuf;
        if (!vtx_extent_p0_value_valid(ext, bytes)) {
            ret = -EUCLEAN;
            goto err;
        }
        if (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)
            continue;

        ext_start = vtx_cpu64(found_k.offset);
        phys_blks = vtx_cpu32(ext->physical_blocks);
        ext_end = ext_start + (__u64)phys_blks * VTXFS_BLOCK_SIZE;
        if (ext_end < ext_start) {
            ret = -EUCLEAN;
            goto err;
        }

        if (ext_end <= cur_scan)
            continue;

        if (ext_start >= end)
            break;

        if (ext_start > cur_scan) {
            /* There is an unbacked gap between cur_scan and min(ext_start, end) */
            __u64 gap_end = ext_start < end ? ext_start : end;
            if (num_gaps < VTX_FALLOC_MAX_GAPS) {
                gaps[num_gaps].start = cur_scan;
                gaps[num_gaps].end = gap_end;
                num_gaps++;
            }
        }
        cur_scan = ext_end < end ? ext_end : end;
        if (cur_scan >= end)
            break;
    }

    if (cur_scan < end) {
        if (num_gaps < VTX_FALLOC_MAX_GAPS) {
            gaps[num_gaps].start = cur_scan;
            gaps[num_gaps].end = end;
            num_gaps++;
        }
    }

    vtx_free(ebuf);
    ebuf = NULL;

    /* Fill all identified gaps with zero-backed extents in 128 KiB chunks */
    if (num_gaps > 0) {
        zero_buf = (__u8 *)vtx_zalloc(VTX_FALLOC_ZERO_BUF_SIZE);
        if (!zero_buf) {
            ret = -ENOMEM;
            goto err;
        }

        for (g = 0; g < num_gaps; g++) {
            __u64 gap_curr = gaps[g].start;
            __u64 gap_end = gaps[g].end;

            while (gap_curr < gap_end) {
                size_t chunk = (size_t)((gap_end - gap_curr) < VTX_FALLOC_ZERO_BUF_SIZE
                                        ? (gap_end - gap_curr)
                                        : VTX_FALLOC_ZERO_BUF_SIZE);
                ret = vtx_write(fs, subvol_id, ino, gap_curr, zero_buf, chunk);
                if (ret)
                    goto err;
                gap_curr += chunk;
            }
        }
        vtx_free(zero_buf);
        zero_buf = NULL;
    }

    /* Update inode size if preallocation extended past EOF */
    if (end > old_size) {
        ret = load_inode(fs, ino, &it);
        if (ret)
            goto err;
        if (end > vtx_cpu64(it.size_bytes)) {
            it.size_bytes = vtx_le64(end);
            ret = save_inode(fs, ino, &it);
            if (ret)
                goto err;
        }
    }

    return 0;

err:
    if (ebuf)
        vtx_free(ebuf);
    if (zero_buf)
        vtx_free(zero_buf);
    fs->read_only = 1;
    return ret;
}

int vtx_truncate(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 __u64 new_size)
{
    struct vtx_inode_item it;
    __u64 old_size, al_new;
    int ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;

    old_size = vtx_cpu64(it.size_bytes);
    if (new_size == 0) {
        __u64 end_drop = (old_size + VTXFS_BLOCK_SIZE - 1U) &
                         ~(__u64)(VTXFS_BLOCK_SIZE - 1U);
        if (end_drop < VTXFS_BLOCK_SIZE)
            end_drop = VTXFS_BLOCK_SIZE;
        ret = drop_extent_range(fs, ino, 0, end_drop);
        if (ret)
            return ret;
        it.size_bytes = 0;
        it.alloc_bytes = 0;
        {
            __u64 now = vtx_time_now_ns();
            it.mtime_ns = vtx_le64(now);
            it.ctime_ns = vtx_le64(now);
        }
        return save_inode(fs, ino, &it);
    }

    if (new_size < old_size) {
        ret = promote_inline_if_present(fs, ino);
        if (ret)
            return ret;
        al_new = (new_size + VTXFS_BLOCK_SIZE - 1U) &
                 ~(__u64)(VTXFS_BLOCK_SIZE - 1U);
        {
            __u64 al_old = (old_size + VTXFS_BLOCK_SIZE - 1U) &
                           ~(__u64)(VTXFS_BLOCK_SIZE - 1U);
            if (al_old > al_new) {
                ret = drop_extent_range(fs, ino, al_new, al_old);
                if (ret)
                    return ret;
            }
        }
        /* Zero-fill tail of the partial boundary block if unaligned. */
        if (new_size & (VTXFS_BLOCK_SIZE - 1U)) {
            __u64 tail_len = al_new - new_size;
            __u8 *zeros = (__u8 *)vtx_zalloc((size_t)tail_len);
            if (!zeros)
                return -ENOMEM;
            ret = vtx_write(fs, subvol_id, ino, new_size,
                            zeros, (size_t)tail_len);
            vtx_free(zeros);
            if (ret)
                return ret;
            ret = load_inode(fs, ino, &it);
            if (ret)
                return ret;
        }
    }
    it.size_bytes = vtx_le64(new_size);
    {
        __u64 now = vtx_time_now_ns();
        it.mtime_ns = vtx_le64(now);
        it.ctime_ns = vtx_le64(now);
    }
    return save_inode(fs, ino, &it);
}

int vtx_reflink(struct vtx_fs *fs, __u64 subvol_id,
                __u64 src_ino, __u64 src_offset,
                __u64 dst_ino, __u64 dst_offset, __u64 byte_len)
{
    struct vtx_inode_item src_it, dst_it;
    struct vtx_key cur_k, found_k;
    __u8 *ebuf = NULL;
    __u64 clone_blks_len;
    size_t bytes;
    int first = 1, ret;

    if (!byte_len ||
        (src_offset & (VTXFS_BLOCK_SIZE - 1U)) ||
        (dst_offset & (VTXFS_BLOCK_SIZE - 1U)))
        return -EINVAL;
    if (src_offset > ~0ULL - byte_len ||
        dst_offset > ~0ULL - byte_len)
        return -EOVERFLOW;
    if (src_ino == dst_ino &&
        src_offset < dst_offset + byte_len &&
        dst_offset < src_offset + byte_len)
        return -EINVAL;

    ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    ret = load_inode(fs, src_ino, &src_it);
    if (ret)
        return ret;
    ret = load_inode(fs, dst_ino, &dst_it);
    if (ret)
        return ret;
    if ((vtx_cpu32(src_it.mode) & 0170000U) != 0100000U ||
        (vtx_cpu32(dst_it.mode) & 0170000U) != 0100000U)
        return -EINVAL;
    if (src_offset + byte_len > vtx_cpu64(src_it.size_bytes))
        return -EINVAL;
    if ((byte_len & (VTXFS_BLOCK_SIZE - 1U)) &&
        (src_offset + byte_len != vtx_cpu64(src_it.size_bytes)))
        return -EINVAL;

    clone_blks_len = (byte_len + VTXFS_BLOCK_SIZE - 1U) &
                     ~(__u64)(VTXFS_BLOCK_SIZE - 1U);

    /* Inline sources need bytewise copying. Refuse metadata cloning
     * rather than rewrite a supposedly read-only source inode. */
    if (vtx_cpu64(src_it.size_bytes) <= VTXFS_MAX_INLINE_DATA)
        return -EOPNOTSUPP;

    ret = promote_inline_if_present(fs, src_ino);
    if (ret)
        return ret;
    ret = promote_inline_if_present(fs, dst_ino);
    if (ret)
        return ret;
    ret = drop_extent_range(fs, dst_ino, dst_offset, dst_offset + clone_blks_len);
    if (ret)
        return ret;

    ebuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!ebuf)
        return -ENOMEM;

    cur_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(src_ino, VTX_TYPE_EXTENT));
    cur_k.offset = 0;

    for (;;) {
        struct vtx_extent_item *ext;
        __u64 ext_start, ext_end, ov_s, ov_e;
        __u32 phys_blks, slice_off, slice_blks;
        const __le32 *csums;

        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first,
                          &found_k, ebuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != src_ino ||
            VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) !=
                VTX_TYPE_EXTENT)
            break;

        ext = (struct vtx_extent_item *)ebuf;
        ext_start = vtx_cpu64(found_k.offset);
        phys_blks = vtx_cpu32(ext->physical_blocks);
        ext_end = ext_start + (__u64)phys_blks * VTXFS_BLOCK_SIZE;
        if (ext_end <= src_offset)
            continue;
        if (ext_start >= src_offset + clone_blks_len)
            break;

        /* Cloning must never mutate the source (even if source is RO). */
        ov_s = ext_start > src_offset ? ext_start : src_offset;
        ov_e = ext_end < (src_offset + clone_blks_len)
               ? ext_end : (src_offset + clone_blks_len);
        slice_off = (__u32)((ov_s - ext_start) / VTXFS_BLOCK_SIZE);
        slice_blks = (__u32)((ov_e - ov_s) / VTXFS_BLOCK_SIZE);
        csums = (const __le32 *)ext->tail;

        /* Increment the allocation ref BEFORE making the new extent
         * reachable. An ENOSPC here must leave no under-counted item. */
        ret = vtx_enqueue_delayed_ref(fs, vtx_cpu64(ext->allocation_id),
                                      vtx_cpu64(ext->vblk), phys_blks,
                                      VTX_REF_FLAG_DATA,
                                      (__u32)subvol_id, 1);
        if (ret)
            break;
        ret = build_and_insert_block_extent(
            fs, dst_ino, dst_offset + (ov_s - src_offset),
            vtx_cpu64(ext->vblk) + slice_off,
            vtx_cpu64(ext->allocation_id),
            slice_blks, VTX_EXTENT_SHARED, csums + slice_off);
        if (ret)
            break;
    }

    vtx_free(ebuf);
    if (ret) {
        fs->read_only = 1; /* never commit a partly applied clone */
        return ret;
    }
    ret = load_inode(fs, dst_ino, &dst_it);
    if (ret) { fs->read_only = 1; return ret; }
    if (dst_offset + byte_len > vtx_cpu64(dst_it.size_bytes))
        dst_it.size_bytes = vtx_le64(dst_offset + byte_len);
    dst_it.alloc_bytes = vtx_le64(
        (vtx_cpu64(dst_it.size_bytes) + VTXFS_BLOCK_SIZE - 1U) &
        ~(__u64)(VTXFS_BLOCK_SIZE - 1U));
    dst_it.mtime_ns = vtx_le64(vtx_time_now_ns());
    dst_it.ctime_ns = dst_it.mtime_ns;
    ret = save_inode(fs, dst_ino, &dst_it);
    if (ret) fs->read_only = 1;
    return ret;
}

/* -------------------------------------------------------------------------
 * 4. Unlink, Rmdir, Rename, Xattr & UAPI Ioctl Queries
 * ------------------------------------------------------------------------- */

/* INODE_REF's v2.1 key is (ino,parent), so its value describes ONE
 * representative name in each directory.  Hard links may share a parent:
 * on deletion, rescan surviving dirents and point the ref to a live name.
 * The separate DIR_INDEX/DIR_ITEM entries remain authoritative for every
 * hard-link name.  This avoids inventing an INCOMPAT key layout.
 */
static int repair_inode_ref(struct vtx_fs *fs, __u64 parent, __u64 ino,
                            const char *deleted_name)
{
    struct vtx_key key;
    struct vtx_inode_ref *ref;
    struct vtx_dirent *ents;
    __u8 value[sizeof(struct vtx_inode_ref) + VTXFS_NAME_MAX];
    struct vtx_inode_item inode;
    __u64 cookie = 0;
    __u32 batches = 0;
    size_t count, j, val_len = sizeof(value);
    int ret;

    key.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(
        ino, VTX_TYPE_INODE_REF));
    key.offset = vtx_le64(parent);
    ret = load_inode(fs, ino, &inode);
    if (ret)
        return ret;
    if (vtx_cpu32(inode.nlink) <= 1) {
        ret = vtx_rw_delete(&fs->active_fs_tree, &key);
        return ret == -ENOENT ? 0 : ret;
    }

    /* Fast path: if the existing INODE_REF is NOT pointing to the name
     * being deleted, it is already pointing to another valid surviving link! */
    if (deleted_name) {
        ret = vtx_rw_lookup(&fs->active_fs_tree, &key, value, &val_len);
        if (ret == -ENOENT)
            return 0;
        if (ret == 0 && val_len >= sizeof(struct vtx_inode_ref)) {
            ref = (struct vtx_inode_ref *)value;
            size_t nlen = (size_t)vtx_cpu16(ref->name_len);
            if (nlen != strlen(deleted_name) ||
                memcmp(ref->name, deleted_name, nlen) != 0) {
                return 0; /* Already pointing to another link in this parent */
            }
        }
    }

    ents = vtx_zalloc(sizeof(*ents) * 64);
    if (!ents)
        return -ENOMEM;
    for (;;) {
        if (++batches > 100000U) {
            ret = -ELOOP;
            break;
        }
        ret = vtx_readdir(fs, fs->active_subvol_id, parent,
                          &cookie, ents, 64, &count);
        if (ret || count == 0)
            break;
        for (j = 0; j < count; j++) {
            size_t nlen;
            if (ents[j].ino != ino)
                continue;
            nlen = strlen(ents[j].name);
            ref = (struct vtx_inode_ref *)value;
            memset(value, 0, sizeof(value));
            ref->dir_seq = vtx_le64(ents[j].dir_seq);
            ref->name_len = vtx_le16((__u16)nlen);
            memcpy(ref->name, ents[j].name, nlen);
            ret = vtx_rw_upsert(&fs->active_fs_tree, &key, value,
                                (__u16)(sizeof(*ref) + nlen));
            goto out;
        }
    }
    if (ret == 0) {
        ret = vtx_rw_delete(&fs->active_fs_tree, &key);
        if (ret == -ENOENT)
            ret = 0;
    }
out:
    vtx_free(ents);
    return ret;
}

static int dir_remove_entry_fast(struct vtx_fs *fs, __u64 dir_ino,
                                 const char *name,
                                 const struct vtx_key *opt_d_k,
                                 const struct vtx_dir_item *opt_d,
                                 __u64 *target_ino_out,
                                 __u8 *ftype_out,
                                 int skip_repair_ref)
{
    size_t nlen = name ? strlen(name) : 0;
    struct vtx_key d_k, idx_k;
    struct vtx_dir_item d;
    struct vtx_inode_item dir_it;
    int ret;

    if (opt_d_k && opt_d) {
        d_k = *opt_d_k;
        d = *opt_d;
    } else {
        ret = dir_find_entry(fs, dir_ino, name, nlen, &d_k, &d);
        if (ret)
            return ret;
    }

    ret = vtx_rw_delete(&fs->active_fs_tree, &d_k);
    if (ret)
        return ret;

    idx_k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(dir_ino, VTX_TYPE_DIR_INDEX));
    idx_k.offset = d.dir_seq;
    (void)vtx_rw_delete(&fs->active_fs_tree, &idx_k);

    if (!skip_repair_ref) {
        ret = repair_inode_ref(fs, dir_ino, vtx_cpu64(d.target_ino), name);
        if (ret) {
            /* A partially updated namespace must NEVER be checkpointed. */
            fs->read_only = 1;
            return ret;
        }
    }

    if (load_inode(fs, dir_ino, &dir_it) == 0) {
        __u64 db = vtx_cpu64(dir_it.u.dir_total_bytes);
        __u64 now = vtx_time_now_ns();
        dir_it.u.dir_total_bytes = vtx_le64(db >= nlen ? db - nlen : 0);
        dir_it.mtime_ns = vtx_le64(now);
        dir_it.ctime_ns = vtx_le64(now);
        (void)save_inode(fs, dir_ino, &dir_it);
    }
    if (target_ino_out)
        *target_ino_out = vtx_cpu64(d.target_ino);
    if (ftype_out)
        *ftype_out = d.file_type;
    return 0;
}

static int __attribute__((unused)) dir_remove_entry(struct vtx_fs *fs, __u64 dir_ino,
                                                     const char *name, __u64 *target_ino_out,
                                                     __u8 *ftype_out)
{
    return dir_remove_entry_fast(fs, dir_ino, name, NULL, NULL,
                                 target_ino_out, ftype_out, 0);
}

static int purge_all_inode_items(struct vtx_fs *fs, __u64 ino)
{
    struct vtx_key start_k, found_k;
    __u8 *ebuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    size_t bytes;
    int ret = 0;

    if (!ebuf)
        return -ENOMEM;
    start_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, 0));
    start_k.offset = 0;

    for (;;) {
        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &start_k, 1,
                          &found_k, ebuf, &bytes);
        if (ret == -ENOENT ||
            VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != ino) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) ==
            VTX_TYPE_EXTENT) {
            const struct vtx_extent_item *ext =
                (const struct vtx_extent_item *)ebuf;
            if (!(vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)) {
                if (fs->num_delayed_refs >= (VTX_MAX_DELAYED_REFS * 3U / 4U)) {
                    ret = vtx_sync_checkpoint(fs);
                    if (ret)
                        break;
                }
                ret = vtx_enqueue_delayed_ref(
                    fs, vtx_cpu64(ext->allocation_id),
                    vtx_cpu64(ext->vblk),
                    vtx_cpu32(ext->physical_blocks),
                    VTX_REF_FLAG_DATA, (__u32)fs->active_subvol_id, -1);
                if (ret)
                    break;
            }
        }
        ret = vtx_rw_delete(&fs->active_fs_tree, &found_k);
        if (ret)
            break;
    }
    vtx_free(ebuf);
    return ret;
}

/*
 * Directory removal and physical reclaim are separate in POSIX VFS mode.
 * A durable orphan marker lives in the existing FS tree (no format change).
 * It is removed at last evict_inode() or recovered after a crash/remount.
 * WARNING: one link per inode is still the current v2.1 contract.
 */
static void orphan_fatal(struct vtx_fs *fs)
{
    /* A failed multi-key mutation must never be checkpointed again. */
    fs->read_only = 1;
}
static int orphan_mark(struct vtx_fs *fs, __u64 ino)
{
    struct vtx_dir_index_val marker;
    struct vtx_key key;
    int ret;
    memset(&marker, 0, sizeof(marker));
    marker.target_ino = vtx_le64(ino);
    key.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(
        VTXFS_INO_ORPHAN_DIR, VTX_TYPE_DIR_INDEX));
    key.offset = vtx_le64(ino);
    ret = vtx_rw_insert(&fs->active_fs_tree, &key, &marker, sizeof(marker));
    if (ret == -EEXIST)
        return -EUCLEAN;
    return ret;
}

int vtx_orphan_purge(struct vtx_fs *fs, __u64 subvol_id, __u64 ino)
{
    struct vtx_key key;
    struct vtx_inode_item it;
    size_t size = 0;
    int ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    key.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(
        VTXFS_INO_ORPHAN_DIR, VTX_TYPE_DIR_INDEX));
    key.offset = vtx_le64(ino);
    ret = vtx_rw_lookup(&fs->active_fs_tree, &key, NULL, &size);
    if (ret == -ENOENT)
        return 0;
    if (ret && ret != -ENOBUFS)
        return ret;
    ret = load_inode(fs, ino, &it);
    if (ret || vtx_cpu32(it.nlink) != 0)
        return ret ? ret : -EUCLEAN;
    ret = purge_all_inode_items(fs, ino);
    if (ret)
        goto error;
    ret = vtx_rw_delete(&fs->active_fs_tree, &key);
    if (ret)
        goto error;
    if (vtx_cpu64(fs->active_subvol.num_inodes)) {
        fs->active_subvol.num_inodes = vtx_le64(
            vtx_cpu64(fs->active_subvol.num_inodes) - 1);
    }
    return 0;
error:
    orphan_fatal(fs);
    return ret;
}

int vtx_orphan_recover(struct vtx_fs *fs, __u64 subvol_id)
{
    struct vtx_key key, found;
    size_t size;
    int ret, first = 1;
    if (!fs || fs->read_only || fs->view_only)
        return 0;
    ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    key.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(
        VTXFS_INO_ORPHAN_DIR, VTX_TYPE_DIR_INDEX));
    key.offset = 0;
    for (;;) {
        size = 0;
        ret = vtx_rw_next(&fs->active_fs_tree, &key, first,
                          &found, NULL, &size);
        if (ret == -ENOENT)
            return 0;
        if (ret != 0 && ret != -ENOBUFS)
            return ret;
        if (vtx_cpu64(found.oid_and_type) !=
            VTX_MAKE_OID_TYPE(VTXFS_INO_ORPHAN_DIR, VTX_TYPE_DIR_INDEX))
            return 0;
        ret = vtx_orphan_purge(fs, subvol_id, vtx_cpu64(found.offset));
        if (ret)
            return ret;
        key = found;
        first = 0;
    }
}

int vtx_unlink(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
               const char *name)
{
    __u64 target_ino = 0;
    __u8 ftype = 0;
    struct vtx_key d_k;
    struct vtx_dir_item d;
    int ret = enter_subvol(fs, subvol_id, 1);

    if (ret)
        return ret;
    if (!name)
        return -EINVAL;
    ret = dir_find_entry(fs, parent_ino, name, strlen(name), &d_k, &d);
    if (ret)
        return ret;
    if (d.file_type == VTX_FT_DIR)
        return -EISDIR;

    ret = dir_remove_entry_fast(fs, parent_ino, name, &d_k, &d, &target_ino, &ftype, 0);
    if (ret)
        return ret;
    {
        struct vtx_inode_item linked;
        ret = load_inode(fs, target_ino, &linked);
        if (ret) { orphan_fatal(fs); return ret; }
        if (vtx_cpu32(linked.nlink) > 1) {
            linked.nlink = vtx_le32(vtx_cpu32(linked.nlink) - 1);
            linked.ctime_ns = vtx_le64(vtx_time_now_ns());
            ret = save_inode(fs, target_ino, &linked);
            if (ret) orphan_fatal(fs);
            return ret;
        }
    }
    if (fs->defer_unlink) {
        struct vtx_inode_item orphan;
        ret = load_inode(fs, target_ino, &orphan);
        if (ret) { orphan_fatal(fs); return ret; }
        if (vtx_cpu32(orphan.nlink) != 1) {
            orphan_fatal(fs); return -EOPNOTSUPP;
        }
        orphan.nlink = vtx_le32(0);
        ret = save_inode(fs, target_ino, &orphan);
        if (!ret)
            ret = orphan_mark(fs, target_ino);
        if (ret) orphan_fatal(fs);
        return ret;
    }
    ret = purge_all_inode_items(fs, target_ino);
    if (ret)
        return ret;
    if (vtx_cpu64(fs->active_subvol.num_inodes) > 0) {
        fs->active_subvol.num_inodes =
            vtx_le64(vtx_cpu64(fs->active_subvol.num_inodes) - 1ULL);
    }
    return 0;
}

int vtx_link(struct vtx_fs *fs, __u64 subvol_id, __u64 inode_ino,
             __u64 new_parent_ino, const char *new_name)
{
    struct vtx_inode_item inode;
    struct vtx_dir_item existing;
    __u32 links;
    int ret = enter_subvol(fs, subvol_id, 1);

    if (ret)
        return ret;
    if (!new_name || !*new_name || strlen(new_name) > VTXFS_NAME_MAX)
        return -EINVAL;
    ret = load_inode(fs, inode_ino, &inode);
    if (ret)
        return ret;
    if ((vtx_cpu32(inode.mode) & 0170000U) != 0100000U)
        return -EPERM;
    links = vtx_cpu32(inode.nlink);
    if (links == 0 || links == 0xFFFFFFFFU)
        return -EMLINK;
    ret = dir_find_entry(fs, new_parent_ino, new_name,
                         strlen(new_name), NULL, &existing);
    if (ret != -ENOENT)
        return ret ? ret : -EEXIST;
    ret = dir_add_entry(fs, new_parent_ino, inode_ino, new_name,
                        VTX_FT_REG);
    if (ret) {
        /* CoW tree mutations can be partial after allocator failure. */
        fs->read_only = 1;
        return ret;
    }
    inode.nlink = vtx_le32(links + 1);
    inode.ctime_ns = vtx_le64(vtx_time_now_ns());
    ret = save_inode(fs, inode_ino, &inode);
    if (ret)
        fs->read_only = 1;
    return ret;
}

int vtx_rmdir(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
              const char *name)
{
    __u64 target_ino = 0, cookie = 0;
    struct vtx_key d_k;
    struct vtx_dir_item d;
    struct vtx_dirent dent;
    size_t count = 0;
    int ret = enter_subvol(fs, subvol_id, 1);

    if (ret)
        return ret;
    if (!name)
        return -EINVAL;
    ret = dir_find_entry(fs, parent_ino, name, strlen(name), &d_k, &d);
    if (ret)
        return ret;
    if (d.file_type != VTX_FT_DIR)
        return -ENOTDIR;
    target_ino = vtx_cpu64(d.target_ino);

    ret = vtx_readdir(fs, subvol_id, target_ino, &cookie, &dent, 1, &count);
    if (ret)
        return ret;
    if (count > 0)
        return -ENOTEMPTY;

    ret = dir_remove_entry_fast(fs, parent_ino, name, &d_k, &d, &target_ino, NULL, 0);
    if (ret)
        return ret;
    ret = purge_all_inode_items(fs, target_ino);
    if (ret)
        return ret;
    if (vtx_cpu64(fs->active_subvol.num_inodes) > 0) {
        fs->active_subvol.num_inodes =
            vtx_le64(vtx_cpu64(fs->active_subvol.num_inodes) - 1ULL);
    }
    return 0;
}

int vtx_rename(struct vtx_fs *fs, __u64 subvol_id,
               __u64 old_parent_ino, const char *old_name,
               __u64 new_parent_ino, const char *new_name)
{
    __u64 src_ino = 0, dst_ino = 0;
    __u8 src_ft = 0;
    struct vtx_key src_k;
    struct vtx_dir_item src_d, existing;
    struct vtx_inode_item src_it;
    int ret = enter_subvol(fs, subvol_id, 1);

    if (ret)
        return ret;
    if (!old_name || !new_name)
        return -EINVAL;
    if (old_parent_ino == new_parent_ino && strcmp(old_name, new_name) == 0)
        return 0;

    /* If destination exists, remove it according to POSIX type rules. */
    if (dir_find_entry(fs, new_parent_ino, new_name, strlen(new_name),
                       NULL, &existing) == 0) {
        dst_ino = vtx_cpu64(existing.target_ino);
        if (existing.file_type == VTX_FT_DIR)
            ret = vtx_rmdir(fs, subvol_id, new_parent_ino, new_name);
        else
            ret = vtx_unlink(fs, subvol_id, new_parent_ino, new_name);
        if (ret)
            return ret;
        (void)dst_ino;
    }

    ret = dir_find_entry(fs, old_parent_ino, old_name, strlen(old_name),
                         &src_k, &src_d);
    if (ret)
        return ret;

    /* Remove old entry without triggering expensive dir scans on hardlinks */
    ret = dir_remove_entry_fast(fs, old_parent_ino, old_name, &src_k, &src_d,
                                &src_ino, &src_ft, 1);
    if (ret)
        return ret;

    /* Add new directory entry */
    ret = dir_add_entry(fs, new_parent_ino, src_ino, new_name, src_ft);
    if (ret)
        return ret;

    /* If moved across directories, clean up old parent INODE_REF and update parent_ino */
    if (old_parent_ino != new_parent_ino) {
        struct vtx_key old_ref_k;
        old_ref_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(src_ino, VTX_TYPE_INODE_REF));
        old_ref_k.offset = vtx_le64(old_parent_ino);
        (void)vtx_rw_delete(&fs->active_fs_tree, &old_ref_k);

        if (load_inode(fs, src_ino, &src_it) == 0) {
            src_it.parent_ino = vtx_le64(new_parent_ino);
            (void)save_inode(fs, src_ino, &src_it);
        }
    }
    return 0;
}

static int dir_replace_target(struct vtx_fs *fs, const struct vtx_key *key,
                              const char *name,
                              const struct vtx_dir_item *original,
                              __u64 target_ino, __u8 file_type)
{
    __u8 value[sizeof(struct vtx_dir_item) + VTXFS_NAME_MAX];
    struct vtx_dir_item *entry = (struct vtx_dir_item *)value;
    size_t name_len = strlen(name);

    if (name_len != vtx_cpu16(original->name_len))
        return -EUCLEAN;
    memset(value, 0, sizeof(value));
    *entry = *original;
    entry->target_ino = vtx_le64(target_ino);
    entry->file_type = file_type;
    memcpy(entry->name, name, name_len);
    return vtx_rw_replace(&fs->active_fs_tree, key, value,
                          (__u16)(sizeof(*entry) + name_len));
}

static int dir_index_replace_target(struct vtx_fs *fs, __u64 parent_ino,
                                    const struct vtx_dir_item *entry,
                                    __u64 target_ino, __u8 file_type)
{
    struct vtx_dir_index_val index;
    struct vtx_key key;
    size_t bytes = sizeof(index);
    int ret;

    key.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(parent_ino, VTX_TYPE_DIR_INDEX));
    key.offset = entry->dir_seq;
    ret = vtx_rw_lookup(&fs->active_fs_tree, &key, &index, &bytes);
    if (ret)
        return ret;
    if (bytes != sizeof(index))
        return -EUCLEAN;
    index.target_ino = vtx_le64(target_ino);
    index.file_type = file_type;
    return vtx_rw_replace(&fs->active_fs_tree, &key, &index, sizeof(index));
}

static int inode_ref_delete(struct vtx_fs *fs, __u64 ino, __u64 parent_ino)
{
    struct vtx_key key;

    key.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_INODE_REF));
    key.offset = vtx_le64(parent_ino);
    return vtx_rw_delete(&fs->active_fs_tree, &key);
}

static int inode_ref_set(struct vtx_fs *fs, __u64 ino, __u64 parent_ino,
                         const char *name, __le64 dir_seq)
{
    __u8 value[sizeof(struct vtx_inode_ref) + VTXFS_NAME_MAX];
    struct vtx_inode_ref *ref = (struct vtx_inode_ref *)value;
    struct vtx_key key;
    size_t name_len = strlen(name);

    if (!name_len || name_len > VTXFS_NAME_MAX)
        return -EINVAL;
    memset(value, 0, sizeof(value));
    ref->dir_seq = dir_seq;
    ref->name_len = vtx_le16((__u16)name_len);
    memcpy(ref->name, name, name_len);
    key.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_INODE_REF));
    key.offset = vtx_le64(parent_ino);
    return vtx_rw_upsert(&fs->active_fs_tree, &key, value,
                         (__u16)(sizeof(*ref) + name_len));
}

static int inode_is_ancestor(struct vtx_fs *fs, __u64 ancestor,
                             __u64 child_parent)
{
    __u32 depth;

    for (depth = 0; depth < 1024U; depth++) {
        struct vtx_inode_item inode;
        __u64 next;
        int ret;

        if (child_parent == ancestor)
            return 1;
        if (!child_parent || child_parent == VTXFS_INO_ROOT_DIR)
            return 0;
        ret = load_inode(fs, child_parent, &inode);
        if (ret)
            return ret;
        next = vtx_cpu64(inode.parent_ino);
        if (!next || next == child_parent)
            return next == child_parent ? -EUCLEAN : 0;
        child_parent = next;
    }
    return -ELOOP;
}

int vtx_rename_exchange(struct vtx_fs *fs, __u64 subvol_id,
                        __u64 old_parent_ino, const char *old_name,
                        __u64 new_parent_ino, const char *new_name)
{
    struct vtx_dir_item old_entry, new_entry;
    struct vtx_inode_item old_inode, new_inode;
    struct vtx_key old_key, new_key;
    __u64 old_ino, new_ino;
    int ret, rollback_ret = 0;
    unsigned int stage = 0;

    ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;
    if (!old_name || !new_name || !*old_name || !*new_name ||
        strlen(old_name) > VTXFS_NAME_MAX ||
        strlen(new_name) > VTXFS_NAME_MAX)
        return -EINVAL;
    if (old_parent_ino == new_parent_ino && strcmp(old_name, new_name) == 0)
        return 0;

    ret = dir_find_entry(fs, old_parent_ino, old_name, strlen(old_name),
                         &old_key, &old_entry);
    if (ret)
        return ret;
    ret = dir_find_entry(fs, new_parent_ino, new_name, strlen(new_name),
                         &new_key, &new_entry);
    if (ret)
        return ret;
    old_ino = vtx_cpu64(old_entry.target_ino);
    new_ino = vtx_cpu64(new_entry.target_ino);
    if (old_ino == new_ino)
        return 0;
    ret = load_inode(fs, old_ino, &old_inode);
    if (ret)
        return ret;
    ret = load_inode(fs, new_ino, &new_inode);
    if (ret)
        return ret;
    if (old_entry.file_type == VTX_FT_DIR) {
        ret = inode_is_ancestor(fs, old_ino, new_parent_ino);
        if (ret)
            return ret > 0 ? -EINVAL : ret;
    }
    if (new_entry.file_type == VTX_FT_DIR) {
        ret = inode_is_ancestor(fs, new_ino, old_parent_ino);
        if (ret)
            return ret > 0 ? -EINVAL : ret;
    }

    /* Primary dir items keep their names/cookies and exchange only targets. */
    ret = dir_replace_target(fs, &old_key, old_name, &old_entry, new_ino,
                             new_entry.file_type);
    if (ret)
        return ret;
    stage = 1;
    ret = dir_replace_target(fs, &new_key, new_name, &new_entry, old_ino,
                             old_entry.file_type);
    if (ret)
        goto rollback;
    stage = 2;
    ret = dir_index_replace_target(fs, old_parent_ino, &old_entry, new_ino,
                                   new_entry.file_type);
    if (ret)
        goto rollback;
    stage = 3;
    ret = dir_index_replace_target(fs, new_parent_ino, &new_entry, old_ino,
                                   old_entry.file_type);
    if (ret)
        goto rollback;
    stage = 4;

    ret = inode_ref_delete(fs, old_ino, old_parent_ino);
    if (ret && ret != -ENOENT)
        goto rollback;
    stage = 5;
    ret = inode_ref_delete(fs, new_ino, new_parent_ino);
    if (ret && ret != -ENOENT)
        goto rollback;
    ret = inode_ref_set(fs, old_ino, new_parent_ino, new_name,
                        new_entry.dir_seq);
    if (ret)
        goto rollback;
    ret = inode_ref_set(fs, new_ino, old_parent_ino, old_name,
                        old_entry.dir_seq);
    if (ret)
        goto rollback;
    stage = 6;

    old_inode.parent_ino = vtx_le64(new_parent_ino);
    new_inode.parent_ino = vtx_le64(old_parent_ino);
    ret = save_inode(fs, old_ino, &old_inode);
    if (ret)
        goto rollback;
    stage = 7;
    ret = save_inode(fs, new_ino, &new_inode);
    if (ret)
        goto rollback;
    return 0;

rollback:
    /* No checkpoint can publish mid-operation; restore the mutable view too. */
    if (stage >= 7) {
        old_inode.parent_ino = vtx_le64(old_parent_ino);
        new_inode.parent_ino = vtx_le64(new_parent_ino);
        rollback_ret |= save_inode(fs, old_ino, &old_inode);
        rollback_ret |= save_inode(fs, new_ino, &new_inode);
    }
    if (stage >= 5) {
        (void)inode_ref_delete(fs, old_ino, new_parent_ino);
        (void)inode_ref_delete(fs, new_ino, old_parent_ino);
        rollback_ret |= inode_ref_set(fs, old_ino, old_parent_ino, old_name,
                                      old_entry.dir_seq);
        rollback_ret |= inode_ref_set(fs, new_ino, new_parent_ino, new_name,
                                      new_entry.dir_seq);
    }
    if (stage >= 4)
        rollback_ret |= dir_index_replace_target(fs, new_parent_ino,
                                                  &new_entry, new_ino,
                                                  new_entry.file_type);
    if (stage >= 3)
        rollback_ret |= dir_index_replace_target(fs, old_parent_ino,
                                                  &old_entry, old_ino,
                                                  old_entry.file_type);
    if (stage >= 2)
        rollback_ret |= dir_replace_target(fs, &new_key, new_name,
                                           &new_entry, new_ino,
                                           new_entry.file_type);
    if (stage >= 1)
        rollback_ret |= dir_replace_target(fs, &old_key, old_name,
                                           &old_entry, old_ino,
                                           old_entry.file_type);
    return rollback_ret ? -EUCLEAN : ret;
}

int vtx_setxattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 const char *name, const void *value, size_t value_len)
{
    size_t nlen = name ? strlen(name) : 0;
    __u64 hash48 = 0;
    __u32 low32 = 0;
    size_t xbuf_cap = sizeof(struct vtx_xattr_item) + VTXFS_NAME_MAX + 1024U;
    __u8 *xbuf;
    struct vtx_xattr_item *x;
    struct vtx_key k;
    struct vtx_inode_item it;
    int ret = enter_subvol(fs, subvol_id, 1);

    if (ret)
        return ret;
    if (!name || !nlen || nlen > VTXFS_NAME_MAX || value_len > 1024U ||
        (value_len && !value))
        return -EINVAL;
    ret = load_inode(fs, ino, &it);
    if (ret)
        return ret;

    xbuf = (__u8 *)vtx_zalloc(xbuf_cap);
    if (!xbuf)
        return -ENOMEM;
    x = (struct vtx_xattr_item *)xbuf;
    vtx_dir_name_hash(fs->super_blk.hdr.dir_hash_seed, name, nlen,
                      &hash48, &low32);
    x->dstream_ino = 0;
    x->name_len = vtx_le16((__u16)nlen);
    x->value_len = vtx_le16((__u16)value_len);
    x->flags = VTX_XATTR_FLAG_EMBEDDED;
    memcpy(x->data, name, nlen);
    if (value_len)
        memcpy(x->data + nlen, value, value_len);

    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_XATTR));
    k.offset = vtx_le64(VTX_XATTR_MAKE_OFFSET(hash48, 0));
    ret = vtx_rw_upsert(&fs->active_fs_tree, &k, xbuf,
                        (__u16)(sizeof(*x) + nlen + value_len));
    vtx_free(xbuf);
    return ret;
}

int vtx_getxattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                 const char *name, void *value, size_t buf_size,
                 size_t *value_len_out)
{
    size_t nlen = name ? strlen(name) : 0;
    __u64 hash48 = 0;
    __u32 low32 = 0;
    size_t xbuf_cap = sizeof(struct vtx_xattr_item) + VTXFS_NAME_MAX + 1024U;
    __u8 *xbuf;
    const struct vtx_xattr_item *x;
    struct vtx_key k;
    size_t bytes = xbuf_cap;
    __u16 vlen;
    int ret = enter_subvol(fs, subvol_id, 0);

    if (ret)
        return ret;
    if (!name || !nlen || nlen > VTXFS_NAME_MAX)
        return -EINVAL;

    xbuf = (__u8 *)vtx_zalloc(xbuf_cap);
    if (!xbuf)
        return -ENOMEM;
    x = (const struct vtx_xattr_item *)xbuf;
    vtx_dir_name_hash(fs->super_blk.hdr.dir_hash_seed, name, nlen,
                      &hash48, &low32);
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_XATTR));
    k.offset = vtx_le64(VTX_XATTR_MAKE_OFFSET(hash48, 0));
    ret = vtx_rw_lookup(&fs->active_fs_tree, &k, xbuf, &bytes);
    if (ret) {
        vtx_free(xbuf);
        return ret;
    }
    if (bytes < sizeof(*x) || vtx_cpu16(x->name_len) != nlen ||
        memcmp(x->data, name, nlen) != 0) {
        vtx_free(xbuf);
        return -ENOENT;
    }
    vlen = vtx_cpu16(x->value_len);
    if (sizeof(*x) + nlen + vlen != bytes) {
        vtx_free(xbuf);
        return -EUCLEAN;
    }
    if (value_len_out)
        *value_len_out = vlen;
    if (!value && buf_size == 0) {
        vtx_free(xbuf);
        return 0;
    }
    if (buf_size < vlen || (vlen && !value)) {
        vtx_free(xbuf);
        return -ERANGE;
    }
    if (vlen)
        memcpy(value, x->data + nlen, vlen);
    vtx_free(xbuf);
    return 0;
}

int vtx_removexattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                    const char *name)
{
    size_t nlen = name ? strlen(name) : 0;
    __u64 hash48 = 0;
    __u32 low32 = 0;
    struct vtx_key k;
    int ret = enter_subvol(fs, subvol_id, 1);

    if (ret)
        return ret;
    if (!name || !nlen || nlen > VTXFS_NAME_MAX)
        return -EINVAL;
    vtx_dir_name_hash(fs->super_blk.hdr.dir_hash_seed, name, nlen,
                      &hash48, &low32);
    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_XATTR));
    k.offset = vtx_le64(VTX_XATTR_MAKE_OFFSET(hash48, 0));
    return vtx_rw_delete(&fs->active_fs_tree, &k);
}

int vtx_listxattr(struct vtx_fs *fs, __u64 subvol_id, __u64 ino,
                  char *buf, size_t buf_size, size_t *len_out)
{
    struct vtx_key cur_k = {0}, found_k;
    size_t total = 0;
    size_t cap = sizeof(struct vtx_xattr_item) + VTXFS_NAME_MAX + 1024U;
    __u8 *xbuf;
    int first = 1, ret;

    ret = enter_subvol(fs, subvol_id, 0);
    if (ret)
        return ret;
    if (buf_size && !buf)
        return -EINVAL;

    xbuf = (__u8 *)vtx_zalloc(cap);
    if (!xbuf)
        return -ENOMEM;

    cur_k.oid_and_type =
        vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_XATTR));
    cur_k.offset = 0;
    for (;;) {
        const struct vtx_xattr_item *x;
        __u16 nlen;
        size_t bytes = cap;

        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first,
                          &found_k, xbuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret == 0 || ret == -ENOBUFS) {
            if (VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type)) != ino ||
                VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) !=
                VTX_TYPE_XATTR) {
                ret = 0;
                break;
            }
        }
        if (ret)
            break;
        first = 0;
        cur_k = found_k;
        x = (const struct vtx_xattr_item *)xbuf;
        nlen = vtx_cpu16(x->name_len);
        if (bytes < sizeof(*x) || nlen == 0 ||
            nlen > VTXFS_NAME_MAX ||
            sizeof(*x) + nlen + vtx_cpu16(x->value_len) != bytes) {
            ret = -EUCLEAN;
            break;
        }
        if (buf && total + nlen + 1U <= buf_size) {
            memcpy(buf + total, x->data, nlen);
            buf[total + nlen] = '\0';
        }
        total += (size_t)nlen + 1U;
    }
    vtx_free(xbuf);
    if (ret == 0 && buf && total > buf_size)
        return -ERANGE;
    if (ret == 0 && len_out)
        *len_out = total;
    return ret;
}

int vtx_mknod(struct vtx_fs *fs, __u64 subvol_id, __u64 parent_ino,
              const char *name, __u32 mode, __u64 rdev, __u64 *ino_out)
{
    struct vtx_inode_item it;
    __u64 ino;
    __u8 ftype;
    int ret;

    switch (mode & 0170000U) {
    case 0010000U: ftype = VTX_FT_FIFO;   break;
    case 0020000U: ftype = VTX_FT_CHRDEV; break;
    case 0060000U: ftype = VTX_FT_BLKDEV; break;
    case 0140000U: ftype = VTX_FT_SOCK;   break;
    default: return -EINVAL;
    }

    ret = enter_subvol(fs, subvol_id, 1);
    if (ret)
        return ret;

    ino = fs->next_oid++;
    if (ino < VTXFS_INO_FIRST_USER) {
        ino = VTXFS_INO_FIRST_USER;
        fs->next_oid = ino + 1ULL;
    }
    memset(&it, 0, sizeof(it));
    it.parent_ino = vtx_le64(parent_ino);
    it.mode = vtx_le32(mode);
    it.nlink = vtx_le32(1);
    it.u.rdev = vtx_le64(rdev);
    {
        __u64 now = vtx_time_now_ns();
        it.atime_ns = vtx_le64(now);
        it.mtime_ns = vtx_le64(now);
        it.ctime_ns = vtx_le64(now);
        it.btime_ns = vtx_le64(now);
    }
    ret = save_inode(fs, ino, &it);
    if (ret)
        return ret;

    ret = dir_add_entry(fs, parent_ino, ino, name, ftype);
    if (ret) {
        struct vtx_key dk;
        dk.oid_and_type =
            vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_INODE));
        dk.offset = 0;
        (void)vtx_rw_delete(&fs->active_fs_tree, &dk);
        return ret;
    }
    fs->active_subvol.num_inodes =
        vtx_le64(vtx_cpu64(fs->active_subvol.num_inodes) + 1ULL);
    if (ino_out)
        *ino_out = ino;
    return 0;
}

int vtx_ioctl_get_abi(const struct vtx_fs *fs, struct vtx_ioctl_abi *arg)
{
    if (!fs || !fs->mounted || !arg ||
        arg->struct_size != sizeof(*arg) || arg->reserved != 0)
        return -EINVAL;
    arg->major = VTXFS_VERSION_MAJOR;
    arg->minor = VTXFS_VERSION_MINOR;
    arg->supported_incompat = VTXFS_P0_ALLOWED_INCOMPAT;
    arg->active_incompat = vtx_cpu64(fs->super_blk.hdr.incompat_flags);
    arg->uapi_version = VTXFS_UAPI_VERSION;
    return 0;
}

int vtx_ioctl_get_capabilities(const struct vtx_fs *fs,
                               struct vtx_ioc_capabilities *arg)
{
    if (!fs || !fs->mounted || !arg ||
        arg->struct_size != sizeof(*arg) || arg->flags != 0 ||
        arg->reserved[0] || arg->reserved[1] || arg->reserved[2])
        return -EINVAL;
    arg->capabilities = VTXFS_CAP_P0_RO_PARSER |
                        VTXFS_CAP_P0_SINGLEDEV_RW |
                        VTXFS_CAP_SNAPSHOTS |
                        VTXFS_CAP_SCRUB;
    arg->supported_incompat = VTXFS_P0_ALLOWED_INCOMPAT;
    arg->active_incompat = vtx_cpu64(fs->super_blk.hdr.incompat_flags);
    return 0;
}

static void restore_subvol_state(struct vtx_fs *fs, __u64 orig_subvol,
                                 int orig_view)
{
    if (!orig_subvol) {
        fs->active_subvol_id = 0;
        fs->view_only = 0;
        return;
    }
    if (orig_view)
        (void)vtx_subvol_switch_view(fs, orig_subvol);
    else
        (void)vtx_subvol_switch(fs, orig_subvol);
}

int vtx_ioctl_omap_lookup(struct vtx_fs *fs, struct vtx_ioc_omap_lookup *arg)
{
    struct vtx_rw_tree *omap;
    struct vtx_key k;
    struct vtx_omap_val val;
    size_t bytes = sizeof(val);
    __u64 orig_subvol, vblk, pblk = 0, rxid = 0;
    int orig_view, ret;
    const void *node = NULL;
    size_t nbytes = 0;
    int verified = 0;

    if (!fs || !fs->mounted || !arg ||
        arg->struct_size != sizeof(*arg) || arg->flags != 0 ||
        arg->reserved[0] || arg->reserved[1] || arg->oid == 0)
        return -EINVAL;

    orig_subvol = fs->active_subvol_id;
    orig_view = fs->view_only;
    if (arg->subvol_id == 0) {
        omap = &fs->omap_meta_tree;
    } else {
        ret = enter_subvol(fs, arg->subvol_id, 0);
        if (ret) {
            restore_subvol_state(fs, orig_subvol, orig_view);
            return ret;
        }
        omap = &fs->active_omap_view;
    }

    k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(arg->oid, VTX_TYPE_OMAP_VAL));
    k.offset = 0;
    ret = vtx_rw_lookup(omap, &k, &val, &bytes);
    if (ret == 0 &&
        (bytes != sizeof(val) ||
         (vtx_cpu32(val.flags) & VTX_OMAP_VAL_TOMBSTONE)))
        ret = -ENOENT;
    if (ret) {
        restore_subvol_state(fs, orig_subvol, orig_view);
        return ret;
    }

    vblk = vtx_cpu64(val.vblk);
    ret = vtx_vag_vblk_to_pblk(&fs->vag, vblk, &pblk);
    if (ret) {
        restore_subvol_state(fs, orig_subvol, orig_view);
        return ret;
    }
    if (omap->ops->read_node(omap->ctx, vblk, &node, &nbytes, &verified) == 0 &&
        node && nbytes == VTXFS_BLOCK_SIZE) {
        const struct vtx_node_hdr *nh = (const struct vtx_node_hdr *)node;
        rxid = vtx_cpu64(nh->xid);
        omap->ops->release_node(omap->ctx, node);
    }

    arg->resolved_vblk = vblk;
    arg->resolved_xid = rxid;
    arg->resolved_pblk = pblk;
    arg->resolved_dev_id = 0;
    arg->hint_valid = (arg->hint_vblk != 0 && arg->hint_vblk == vblk &&
                       (arg->hint_xid == 0 || arg->hint_xid == rxid)) ? 1U : 0U;
    restore_subvol_state(fs, orig_subvol, orig_view);
    return 0;
}

static int posix_scrub_walk_tree(struct vtx_fs *fs,
                                 const struct vtx_rw_tree *t,
                                 __u64 oid, __u64 vblk, int level,
                                 struct vtx_ioc_scrub_args *arg)
{
    __u8 *blk;
    const struct vtx_node_hdr *h;
    __u64 pblk = 0;
    __u32 vag_id = 0, ag_off = 0, nitems, i;
    int in_range = 1, ret;
#ifdef __KERNEL__
    if (signal_pending(current))
        return -EINTR;
#endif
    if (vtx_vblk_decode(vblk, &vag_id, &ag_off) != 0)
        return -EUCLEAN;
    if (arg->end_vag > arg->start_vag &&
        (vag_id < arg->start_vag || vag_id >= arg->end_vag))
        in_range = 0;

    blk = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    if (!blk)
        return -ENOMEM;

    {
        int found_dirty = 0;
        struct vtx_cached_node *c = vtx_find_cached_node(fs, vblk);
        if (c && c->in_use && c->dirty) {
            memcpy(blk, c->data, VTXFS_BLOCK_SIZE);
            found_dirty = 1;
        }
        if (!found_dirty) {
            ret = vtx_vag_vblk_to_pblk(&fs->vag, vblk, &pblk);
            if (ret == 0)
                ret = fs->bdev->ops->read_blocks(fs->bdev, pblk, 1, blk);
            if (ret) {
                vtx_free(blk);
                return ret;
            }
        }
        h = (const struct vtx_node_hdr *)blk;
        if ((!found_dirty &&
             !vtx_node_checksum_verify(blk, VTXFS_BLOCK_SIZE,
                                       fs->super_blk.hdr.metadata_uuid)) ||
            !vtx_node_slots_valid(h, VTXFS_BLOCK_SIZE) ||
            vtx_cpu64(h->oid) != oid ||
            vtx_cpu64(h->self_vblk) != vblk ||
            h->level != level) {
            if (in_range) {
                arg->meta_blocks_scrubbed++;
                arg->checksum_errors++;
                arg->uncorrectable_errors++;
            }
            vtx_free(blk);
            return 0;
        }
    }

    if (in_range)
        arg->meta_blocks_scrubbed++;

    if (level > 0) {
        nitems = vtx_cpu16(h->nitems);
        for (i = 0; i < nitems; i++) {
#ifdef __KERNEL__
            if (signal_pending(current)) {
                vtx_free(blk);
                return -EINTR;
            }
#endif
            const struct vtx_node_entry *e = &h->entries[i];
            struct vtx_key_ptr_val ptr;
            __u64 c_oid, c_vblk = 0;

            if (vtx_cpu16(e->slot.val_len) != sizeof(ptr)) {
                arg->checksum_errors++;
                arg->uncorrectable_errors++;
                continue;
            }
            memcpy(&ptr, blk + (__u32)vtx_cpu16(e->slot.val_off8) * 8U,
                   sizeof(ptr));
            c_oid = vtx_cpu64(ptr.child_oid);
            if (t->is_physical) {
                c_vblk = vtx_cpu64(ptr.hint_vblk);
            } else {
                if (t->ops->omap_resolve(t->ctx, t->view_id, c_oid, &c_vblk) != 0) {
                    arg->checksum_errors++;
                    arg->uncorrectable_errors++;
                    continue;
                }
            }
            ret = posix_scrub_walk_tree(fs, t, c_oid, c_vblk, level - 1, arg);
            if (ret) {
                vtx_free(blk);
                return ret;
            }
        }
    }

    vtx_free(blk);
    return 0;
}

static int posix_scrub_subvol_extents(struct vtx_fs *fs,
                                      struct vtx_ioc_scrub_args *arg)
{
    struct vtx_key cur_k = {0}, found_k;
    __u8 *ibuf, *blk_buf;
    size_t bytes;
    int first = 1, ret = 0;

    ibuf = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
    blk_buf = (__u8 *)vtx_zalloc(32U * VTXFS_BLOCK_SIZE);
    if (!ibuf || !blk_buf) {
        vtx_free(ibuf);
        vtx_free(blk_buf);
        return -ENOMEM;
    }

    for (;;) {
#ifdef __KERNEL__
        if (signal_pending(current)) {
            ret = -EINTR;
            break;
        }
#endif
        bytes = VTXFS_BLOCK_SIZE;
        ret = vtx_rw_next(&fs->active_fs_tree, &cur_k, first, &found_k,
                          ibuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            break;
        first = 0;
        cur_k = found_k;

        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) == VTX_TYPE_EXTENT) {
            const struct vtx_extent_item *ext =
                (const struct vtx_extent_item *)ibuf;
            __u32 phys_blks, b;

            if (!vtx_extent_p0_value_valid(ext, bytes)) {
                arg->checksum_errors++;
                arg->uncorrectable_errors++;
                continue;
            }
            if (vtx_cpu32(ext->flags) & VTX_EXTENT_INLINE)
                continue;

            phys_blks = vtx_cpu32(ext->physical_blocks);
            for (b = 0; b < phys_blks;) {
#ifdef __KERNEL__
                if (signal_pending(current)) {
                    ret = -EINTR;
                    break;
                }
#endif
                __u32 chunk_blks = phys_blks - b;
                if (chunk_blks > 32U)
                    chunk_blks = 32U;
                __u64 start_vblk = vtx_cpu64(ext->vblk) + b;
                __u32 vag_id = 0, ag_off = 0;
                __u64 pblk = 0;
                __u32 k;

                if (vtx_vblk_decode(start_vblk, &vag_id, &ag_off) != 0) {
                    arg->checksum_errors += chunk_blks;
                    arg->uncorrectable_errors += chunk_blks;
                    b += chunk_blks;
                    continue;
                }
                if (arg->end_vag > arg->start_vag &&
                    (vag_id < arg->start_vag || vag_id >= arg->end_vag)) {
                    b += chunk_blks;
                    continue;
                }

                ret = vtx_vag_vblk_to_pblk(&fs->vag, start_vblk, &pblk);
                if (ret == 0)
                    ret = fs->bdev->ops->read_blocks(fs->bdev, pblk, chunk_blks, blk_buf);
                if (ret) {
                    arg->checksum_errors += chunk_blks;
                    arg->uncorrectable_errors += chunk_blks;
                    ret = 0;
                    b += chunk_blks;
                    continue;
                }

                /* Overlay uncommitted pending writes */
                for (k = 0; k < chunk_blks; k++) {
                    __u64 cur_v = start_vblk + k;
                    __u32 pi;
                    for (pi = fs->num_pending_data; pi > 0; pi--) {
                        const struct vtx_pending_data_write *pw = &fs->pending_data[pi - 1U];
                        if (cur_v >= pw->vblk && cur_v < pw->vblk + pw->block_count) {
                            size_t off = (size_t)(cur_v - pw->vblk) * VTXFS_BLOCK_SIZE;
                            memcpy(blk_buf + (size_t)k * VTXFS_BLOCK_SIZE, pw->data + off, VTXFS_BLOCK_SIZE);
                            break;
                        }
                    }
                }

                /* Checksum verification for each block in the chunk */
                for (k = 0; k < chunk_blks; k++) {
                    __le32 crc_le;
                    arg->data_blocks_scrubbed++;
                    memcpy(&crc_le, ext->tail + (b + k) * sizeof(__le32), sizeof(crc_le));
                    if (vtx_crc32c(blk_buf + (size_t)k * VTXFS_BLOCK_SIZE, VTXFS_BLOCK_SIZE) != vtx_cpu32(crc_le)) {
                        arg->checksum_errors++;
                        arg->uncorrectable_errors++;
                    }
                }
                b += chunk_blks;
            }
            if (ret == -EINTR)
                break;
        }
    }

    vtx_free(ibuf);
    vtx_free(blk_buf);
    return ret;
}

int vtx_ioctl_scrub(struct vtx_fs *fs, struct vtx_ioc_scrub_args *arg)
{
    const struct vtx_rw_tree *sys_trees[6];
    struct vtx_key cur_k = {0}, found_k;
    __u64 *subvols = NULL;
    __u32 num_subvols = 0, cap_subvols = 64, i;
    __u8 vbuf[256];
    size_t bytes;
    __u64 orig_subvol;
    int orig_view, first = 1, ret = 0;

    if (!fs || !fs->mounted || !arg ||
        arg->struct_size != sizeof(*arg) ||
        (arg->flags & ~(VTX_SCRUB_VERIFY_ONLY | VTX_SCRUB_REPAIR_META)) ||
        arg->reserved[0] || arg->reserved[1] ||
        (arg->end_vag != 0 && arg->end_vag < arg->start_vag))
        return -EINVAL;

    if ((arg->flags & VTX_SCRUB_REPAIR_META) &&
        !(vtx_cpu64(fs->super_blk.hdr.incompat_flags) & VTXFS_FEAT_INCOMPAT_DUP_META))
        return -EOPNOTSUPP;

    arg->data_blocks_scrubbed = 0;
    arg->meta_blocks_scrubbed = 0;
    arg->checksum_errors = 0;
    arg->repaired_meta_blocks = 0;
    arg->uncorrectable_errors = 0;

    orig_subvol = fs->active_subvol_id;
    orig_view = fs->view_only;

    sys_trees[0] = &fs->omap_meta_tree;
    sys_trees[1] = &fs->root_tree;
    sys_trees[2] = &fs->ref_tree;
    sys_trees[3] = &fs->reaper_tree;
    sys_trees[4] = &fs->sfq_tree;
    sys_trees[5] = &fs->uuid_tree;

    for (i = 0; i < 6; i++) {
#ifdef __KERNEL__
        if (signal_pending(current)) {
            ret = -EINTR;
            goto out;
        }
#endif
        ret = posix_scrub_walk_tree(fs, sys_trees[i], sys_trees[i]->root_oid,
                                    sys_trees[i]->root_vblk,
                                    sys_trees[i]->root_level, arg);
        if (ret)
            goto out;
    }

    subvols = (__u64 *)vtx_zalloc((size_t)cap_subvols * sizeof(__u64));
    if (!subvols) {
        ret = -ENOMEM;
        goto out;
    }

    for (;;) {
#ifdef __KERNEL__
        if (signal_pending(current)) {
            ret = -EINTR;
            goto out;
        }
#endif
        bytes = sizeof(vbuf);
        ret = vtx_rw_next(&fs->root_tree, &cur_k, first, &found_k,
                          vbuf, &bytes);
        if (ret == -ENOENT) {
            ret = 0;
            break;
        }
        if (ret)
            goto out;
        first = 0;
        cur_k = found_k;
        if (VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type)) ==
            VTX_TYPE_SUBVOL_ROOT) {
            if (num_subvols == cap_subvols) {
                __u32 ncap = cap_subvols * 2U;
                __u64 *np = (__u64 *)vtx_zalloc((size_t)ncap * sizeof(__u64));
                if (!np) {
                    ret = -ENOMEM;
                    goto out;
                }
                memcpy(np, subvols, (size_t)num_subvols * sizeof(__u64));
                vtx_free(subvols);
                subvols = np;
                cap_subvols = ncap;
            }
            subvols[num_subvols++] =
                VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type));
        }
    }

    for (i = 0; i < num_subvols; i++) {
#ifdef __KERNEL__
        if (signal_pending(current)) {
            ret = -EINTR;
            goto out;
        }
#endif
        ret = enter_subvol(fs, subvols[i], 0);
        if (ret)
            goto out;
        ret = posix_scrub_walk_tree(fs, &fs->active_omap_view,
                                    fs->active_omap_view.root_oid,
                                    fs->active_omap_view.root_vblk,
                                    fs->active_omap_view.root_level, arg);
        if (ret)
            goto out;
        ret = posix_scrub_walk_tree(fs, &fs->active_fs_tree,
                                    fs->active_fs_tree.root_oid,
                                    fs->active_fs_tree.root_vblk,
                                    fs->active_fs_tree.root_level, arg);
        if (ret)
            goto out;
        ret = posix_scrub_subvol_extents(fs, arg);
        if (ret)
            goto out;
    }

out:
    vtx_free(subvols);
    restore_subvol_state(fs, orig_subvol, orig_view);
    return ret;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(vtx_ioctl_omap_lookup);
EXPORT_SYMBOL_GPL(vtx_ioctl_scrub);
#endif
