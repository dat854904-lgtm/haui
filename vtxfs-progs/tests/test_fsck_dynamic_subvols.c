// SPDX-License-Identifier: GPL-2.0-only
/* Verify fsck no longer fails at its historic 128-subvolume limit. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vtxfs_engine.h"

int main(void)
{
    const __u8 uuid[16]={0x16,0x87,0x55,0x12,0x38,0x99,0x40,0x20,
                          0x31,0x88,0x17,0x62,0x11,0x56,0x10,0x05};
    struct vtx_fs *fs;
    struct vtx_bdev bd;
    struct vtx_fsck_report report;
    __u64 id;
    int rc, i;
    rc=vtx_ram_bdev_init(&bd,65536);assert(!rc);
    rc=vtx_mkfs(&bd,"many-subvols",uuid);assert(!rc);
    fs=vtx_fs_alloc();assert(fs);
    rc=vtx_mount(fs,&bd,0);assert(!rc);
    for (i=0;i<136;i++) {
        rc=vtx_subvol_create(fs,VTXFS_OID_FIRST_USER_SUBVOL,NULL,&id);
        if (rc) fprintf(stderr,"create(%d) rc=%d\n",i,rc);
        assert(!rc);
        if (i%32==31) {
            rc=vtx_sync_checkpoint(fs);
            assert(!rc);
        }
    }
    rc=vtx_sync_checkpoint(fs);assert(!rc);
    rc=vtx_unmount(fs);assert(!rc);
    vtx_fs_free(fs);
    memset(&report,0,sizeof(report));
    rc=vtx_fsck(&bd,&report);
    if (rc)
        fprintf(stderr,"fsck rc=%d errors=%u subvols=%u\n",
                rc,report.errors_found,report.subvolumes_checked);
    assert(!rc && !report.errors_found && report.subvolumes_checked==137);
    vtx_ram_bdev_destroy(&bd);
    puts("PASS: fsck 137 subvolumes (>128 static limit) clean");
    return 0;
}
