// SPDX-License-Identifier: GPL-2.0-only
/* Regression: reflink delayed-ref overflow must not checkpoint corrupt refs;
 * snapshot-of-readonly source must leave the source's extent item unchanged.
 * No physical device: VTXFS RAM bdev only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "vtxfs_engine.h"

#define SV VTXFS_OID_FIRST_USER_SUBVOL

static void check_fsck(struct vtx_bdev *b)
{
    struct vtx_fsck_report r;
    int ret = vtx_fsck(b, &r);
    if (ret)
        fprintf(stderr, "fsck failed rc=%d errors=%u\n", ret, r.errors_found);
    assert(ret == 0 && r.errors_found == 0);
}

int main(void)
{
    static const __u8 uuid[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    struct vtx_bdev bd;
    struct vtx_fs *fs;
    __u64 src=0, dst=0, snap_ro=0, snap_clone=0;
    __u8 writebuf[8192], buf[8192];
    size_t got=0;
    int rc;
    struct vtx_inode_item it;
    unsigned i;

    for (i=0;i<sizeof(writebuf);i++) writebuf[i]=(__u8)((i*7U)^0x96U);
    rc=vtx_ram_bdev_init(&bd,65536);assert(rc==0);
    rc=vtx_mkfs(&bd,"ref-safety",uuid);assert(rc==0);
    fs=vtx_fs_alloc();assert(fs);
    rc=vtx_mount(fs,&bd,0);assert(rc==0);
    rc=vtx_create(fs,SV,VTXFS_INO_ROOT_DIR,"src",0644,&src);assert(!rc);
    rc=vtx_create(fs,SV,VTXFS_INO_ROOT_DIR,"dst",0644,&dst);assert(!rc);
    rc=vtx_write(fs,SV,src,0,writebuf,sizeof(writebuf));assert(!rc);
    rc=vtx_write(fs,SV,dst,0,writebuf,sizeof(writebuf));assert(!rc);
    rc=vtx_sync_checkpoint(fs);assert(!rc);
    check_fsck(&bd);

    /* An unsupported type, or a clone past source EOF, mutates nothing. */
    rc=vtx_reflink(fs,SV,VTXFS_INO_ROOT_DIR,0,dst,0,4096);
    assert(rc==-EINVAL);
    rc=vtx_reflink(fs,SV,src,0,dst,0,12288);
    assert(rc==-EINVAL);
    assert(!fs->read_only);

    rc=vtx_snapshot_create(fs,SV,NULL,1,&snap_ro);assert(!rc);
    rc=vtx_sync_checkpoint(fs);assert(!rc);
    check_fsck(&bd);

    /* Snapshot from RO must not modify even one byte of its source
     * extent item. A named snapshot under the RO parent must be denied. */
    {
        struct vtx_key key;
        __u8 before[256], after[256];
        size_t before_size=sizeof(before), after_size=sizeof(after);
        key.oid_and_type=vtx_le64(VTX_MAKE_OID_TYPE(src,VTX_TYPE_EXTENT));
        key.offset=0;
        rc=vtx_subvol_switch(fs,snap_ro);assert(!rc);
        rc=vtx_rw_lookup(&fs->active_fs_tree,&key,before,&before_size);
        assert(!rc);
        rc=vtx_snapshot_create(fs,snap_ro,"should-refuse",1,&snap_clone);
        assert(rc==-EROFS);
        rc=vtx_snapshot_create(fs,snap_ro,NULL,1,&snap_clone);
        assert(!rc);
        rc=vtx_subvol_switch(fs,snap_ro);assert(!rc);
        rc=vtx_rw_lookup(&fs->active_fs_tree,&key,after,&after_size);
        assert(!rc && before_size==after_size &&
               memcmp(before,after,before_size)==0);
    }
    rc=vtx_sync_checkpoint(fs);assert(!rc);
    check_fsck(&bd);
    rc=vtx_read(fs,snap_ro,src,0,buf,sizeof(buf),&got);
    assert(!rc && got==sizeof(buf) && !memcmp(buf,writebuf,sizeof(buf)));
    rc=vtx_read(fs,snap_clone,src,0,buf,sizeof(buf),&got);
    assert(!rc && got==sizeof(buf) && !memcmp(buf,writebuf,sizeof(buf)));
    rc=vtx_stat(fs,snap_ro,src,&it);assert(!rc);
    assert(vtx_cpu64(fs->active_subvol.flags)&VTX_SUBVOL_RDONLY);

    /* Normal successful clone and persistent ref accounting. */
    rc=vtx_reflink(fs,SV,src,0,dst,0,8192);assert(!rc);
    rc=vtx_sync_checkpoint(fs);assert(!rc);
    check_fsck(&bd);
    rc=vtx_read(fs,SV,dst,0,buf,sizeof(buf),&got);
    assert(!rc && got==sizeof(buf) && !memcmp(buf,writebuf,sizeof(buf)));

    /* Deterministic full delayed-ref queue: must fail closed and MUST NOT
     * leave a checkpoint with the destination pointing to unreferenced
     * source blocks. The current engine intentionally has no tx rollback. */
    fs->num_delayed_refs=VTX_MAX_DELAYED_REFS;
    rc=vtx_reflink(fs,SV,src,0,dst,0,8192);
    assert(rc==-ENOSPC);
    assert(fs->read_only);
    rc=vtx_sync_checkpoint(fs);
    assert(rc==-EROFS);
    rc=vtx_unmount(fs);assert(!rc);
    vtx_fs_free(fs);
    fs=vtx_fs_alloc();assert(fs);
    rc=vtx_mount(fs,&bd,0);assert(!rc);
    check_fsck(&bd);
    rc=vtx_read(fs,SV,dst,0,buf,sizeof(buf),&got);
    assert(!rc && got==sizeof(buf) && !memcmp(buf,writebuf,sizeof(buf)));
    rc=vtx_unmount(fs);assert(!rc);
    vtx_fs_free(fs);
    vtx_ram_bdev_destroy(&bd);
    printf("PASS: snapshot RO source unchanged + reflink EOF/type rejection + under-ref ENOSPC fail-closed + remount/fsck\n");
    return 0;
}
