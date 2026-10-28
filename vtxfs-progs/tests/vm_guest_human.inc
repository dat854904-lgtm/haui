/* Human-style mounted VFS experiences in an isolated QEMU guest.
 * The companion /dev/vdb is a separate disposable tmpfs image.
 */
static unsigned int human_defects;
static unsigned int human_missing;
static void human_report(const char *kind, const char *what, int err)
{
    msg("VTXFS_VM_HUMAN_%s item=%s errno=%d\n",kind,what,err);
    if (!strcmp(kind,"DEFECT"))
        human_defects++;
    else if (!strcmp(kind,"MISSING"))
        human_missing++;
}
static void human_copy_file_range_cache_test(void)
{
    unsigned char srcbuf[4096], oldbuf[4096], got[4096];
    unsigned char *mapped;
    int srcfd=-1, destfd=-1;
    ssize_t n;

    memset(srcbuf,0x5a,sizeof(srcbuf));
    memset(oldbuf,0xb3,sizeof(oldbuf));
    srcfd=open("/mnt/human/cfr-source",O_RDWR|O_CREAT|O_EXCL,0644);
    destfd=open("/mnt/human/cfr-dest",O_RDWR|O_CREAT|O_EXCL,0644);
    if(srcfd<0 || destfd<0)goto defect;
    if(write(srcfd,srcbuf,sizeof(srcbuf))!=(ssize_t)sizeof(srcbuf) ||
       write(destfd,oldbuf,sizeof(oldbuf))!=(ssize_t)sizeof(oldbuf))
        goto defect;
    if(pread(destfd,got,sizeof(got),0)!=(ssize_t)sizeof(got))
        goto defect;  /* Prewarm uptodate pagecache of destination. */
    mapped=mmap(NULL,sizeof(srcbuf),PROT_READ|PROT_WRITE,MAP_SHARED,srcfd,0);
    if(mapped==MAP_FAILED)goto defect;
    memset(mapped,0x3c,sizeof(srcbuf)); /* Source dirty mmap, NOT fsynced. */
    if(lseek(srcfd,0,SEEK_SET)!=0 || lseek(destfd,0,SEEK_SET)!=0){
        munmap(mapped,sizeof(srcbuf));goto defect;
    }
    n=syscall(SYS_copy_file_range,srcfd,NULL,destfd,NULL,sizeof(srcbuf),0);
    if(n!=(ssize_t)sizeof(srcbuf)){
        munmap(mapped,sizeof(srcbuf));
        goto defect;
    }
    memset(got,0,sizeof(got));
    if(pread(destfd,got,sizeof(got),0)!=(ssize_t)sizeof(got)){
        munmap(mapped,sizeof(srcbuf));
        goto defect;
    }
    memset(srcbuf,0x3c,sizeof(srcbuf));
    if(memcmp(got,srcbuf,sizeof(srcbuf))!=0){
        msg("VTXFS_VM_CFR_DEBUG copied=%zd mapped=%02x got=%02x expected=%02x\n",
            n,mapped[0],got[0],srcbuf[0]);
        munmap(mapped,sizeof(srcbuf));
        goto defect;
    }
    if(fsync(destfd)){munmap(mapped,sizeof(srcbuf));goto defect;}
    munmap(mapped,sizeof(srcbuf));
    msg("VTXFS_VM_HUMAN_OK item=copy_file_range_dirty_mmap_and_old_cache\n");
    goto out;
defect:
    human_report("DEFECT","copy_file_range_pagecache",errno);
out:
    if(srcfd>=0)close(srcfd);
    if(destfd>=0)close(destfd);
}

static unsigned long long human_ro_snapshot_id(void)
{
    char cmdline[2048]={0};
    char *p;
    FILE *f=fopen("/proc/cmdline","r");
    if(!f)return 0;
    if(!fgets(cmdline,sizeof(cmdline),f)){fclose(f);return 0;}
    fclose(f);
    p=strstr(cmdline,"vtx_ro_id=");
    return p?strtoull(p+10,NULL,10):0;
}

#include "vm_guest_ra_group.inc"

static void human_workload(void)
{
    unsigned char data[8192], got[8192];
    struct stat st;
    struct statfs fsst;
    int fd=-1, rd=-1, bd=-1, sub_fd=-1, mounted2=0, i, status=0;
    ssize_t n;
    pid_t pid;
    char xattr[32]={0};

    msg("VTXFS_VM_HUMAN_BEGIN\n");
    if(mkdir("/mnt/human",0755) || mkdir("/mnt/human/docs",0755)){
        fail("human_mkdir");return;
    }
    fd=open("/mnt/human/data.bin",O_RDWR|O_CREAT|O_EXCL,0644);
    if(fd<0){fail("human_open_create");return;}
    for(i=0;i<8192;i++)data[i]=(unsigned char)((i*7+11)&255);
    if(write(fd,data,sizeof(data))!=(ssize_t)sizeof(data)){
        fail("human_write");goto out;
    }
    if(fsync(fd)){fail("human_fsync");goto out;}
    if(fstat(fd,&st) || st.st_size!=(off_t)sizeof(data)){
        fail("human_stat");goto out;
    }
    if(symlink("../data.bin","/mnt/human/docs/latest")){
        fail("human_symlink");goto out;
    }
    rd=open("/mnt/human/docs/latest",O_RDONLY);
    if(rd<0){fail("human_symlink_follow");goto out;}
    if(read(rd,got,sizeof(got))!=(ssize_t)sizeof(got) ||
       memcmp(data,got,sizeof(data))){
        errno=EIO;fail("human_symlink_read");goto out;
    }
    close(rd);rd=-1;
    msg("VTXFS_VM_HUMAN_OK item=symlink_follow_read\n");

    if(fsetxattr(fd,"user.note","project",7,0)){
        fail("human_user_xattr_write");goto out;
    }
    if(fgetxattr(fd,"user.note",xattr,sizeof(xattr))!=7 ||
       memcmp(xattr,"project",7)){
        errno=EIO;fail("human_user_xattr_read");goto out;
    }
    msg("VTXFS_VM_HUMAN_OK item=user_xattr\n");

    if(fchmod(fd,0444)){fail("human_chmod");goto out;}
    if(fstat(fd,&st) || (st.st_mode&0777)!=0444){
        errno=EIO;fail("human_chmod_stat");goto out;
    }
    pid=fork();
    if(pid<0){fail("human_fork");goto out;}
    if(pid==0){
        int w;
        if(setgid(65534) || setuid(65534))_exit(3);
        w=open("/mnt/human/data.bin",O_WRONLY);
        if(w>=0){close(w);_exit(2);}
        _exit(errno==EACCES?0:1);
    }
    if(waitpid(pid,&status,0)!=pid || !WIFEXITED(status) ||
       WEXITSTATUS(status)!=0)
        human_report("DEFECT","chmod_0444_denied_for_unprivileged",status);
    else
        msg("VTXFS_VM_HUMAN_OK item=chmod_unprivileged_denied\n");
    if(fchmod(fd,0644)){fail("human_chmod_restore");goto out;}

    if(statfs("/mnt",&fsst)){fail("human_statfs");goto out;}
    if(fsst.f_ffree==0)
        human_report("DEFECT","df_inode_free_zero",(int)fsst.f_ffree);
    else
        msg("VTXFS_VM_HUMAN_OK item=statfs_inode_free\n");

    sub_fd=open("/mnt/human/links-origin",O_CREAT|O_EXCL|O_RDWR,0644);
    if(sub_fd<0) {fail("human_hardlink_create");goto out;}
    if(write(sub_fd,"HARDLINK-DATA",13)!=13 || fsync(sub_fd)){
        fail("human_hardlink_prepare");goto out;
    }
    close(sub_fd);sub_fd=-1;
    if(link("/mnt/human/links-origin","/mnt/human/hardlink-same-dir") ||
       link("/mnt/human/links-origin","/mnt/human/docs/hardlink-other-dir")){
        human_report("DEFECT","hardlink_create",errno);
    } else {
        if(stat("/mnt/human/links-origin",&st) || st.st_nlink!=3)
            human_report("DEFECT","hardlink_nlink_three",0);
        if(unlink("/mnt/human/links-origin"))
            human_report("DEFECT","hardlink_unlink_origin",errno);
        rd=open("/mnt/human/hardlink-same-dir",O_RDONLY);
        if(rd<0 || read(rd,got,13)!=13 || memcmp(got,"HARDLINK-DATA",13))
            human_report("DEFECT","hardlink_read_survivor",errno);
        if(rd>=0)close(rd);
        rd=-1;
        if(unlink("/mnt/human/hardlink-same-dir"))
            human_report("DEFECT","hardlink_unlink_second",errno);
        if(stat("/mnt/human/docs/hardlink-other-dir",&st) ||
           st.st_nlink!=1)
            human_report("DEFECT","hardlink_nlink_last",errno);
        rd=open("/mnt/human/docs/hardlink-other-dir",O_RDONLY);
        if(rd<0 || read(rd,got,13)!=13 ||
           memcmp(got,"HARDLINK-DATA",13))
            human_report("DEFECT","hardlink_crossdir_read",errno);
        if(rd>=0)close(rd);
        rd=-1;
        msg("VTXFS_VM_HUMAN_OK item=hardlink_nlink_unlink_survivor\n");
    }

    /* Mode=0 eagerly reserves actual physical CRC-verified data blocks.
     * The contents of already allocated regions MUST NOT change. */
    if(syscall(SYS_fallocate,fd,0,0,4096)!=0)
        human_report("DEFECT","fallocate_mode0_existing",errno);
    else {
        if(pread(fd,got,sizeof(got),0)!=(ssize_t)sizeof(got) ||
           memcmp(got,data,sizeof(data)))
            human_report("DEFECT","fallocate_clobber_existing_data",errno);
        else
            msg("VTXFS_VM_HUMAN_OK item=fallocate_keeps_existing\n");
    }
    sub_fd=open("/mnt/human/preallocated.bin",O_CREAT|O_EXCL|O_RDWR,0644);
    if(sub_fd<0)fail("fallocate_new_file_open");
    else {
        if(syscall(SYS_fallocate,sub_fd,0,0,8192)!=0)
            human_report("DEFECT","fallocate_eager_zero",errno);
        else if(fstat(sub_fd,&st) || st.st_size!=8192)
            human_report("DEFECT","fallocate_eager_size",errno);
        else {
            int clean=1,k;
            if(pread(sub_fd,got,sizeof(got),0)!=(ssize_t)sizeof(got))
                clean=0;
            for(k=0;k<8192;k++)
                if(got[k])clean=0;
            if(!clean)
                human_report("DEFECT","fallocate_eager_read_zero",0);
            else
                msg("VTXFS_VM_HUMAN_OK item=fallocate_real_8KiB\n");
        }
        close(sub_fd);sub_fd=-1;
    }
    errno=0;
    if(syscall(SYS_fallocate,fd,FALLOC_FL_KEEP_SIZE,0,4096)!=-1 ||
       errno!=EOPNOTSUPP)
        human_report("DEFECT","fallocate_keep_size_should_reject",errno);
    else
        msg("VTXFS_VM_HUMAN_OK item=fallocate_keep_size_rejected\n");

    sub_fd=open("/mnt/human/app.log",O_WRONLY|O_CREAT|O_APPEND,0644);
    if(sub_fd<0){fail("human_append_open");goto out;}
    if(write(sub_fd,"start\n",6)!=6 ||
       write(sub_fd,"finish\n",7)!=7 ||
       fsync(sub_fd)){
        fail("human_append_write");goto out;
    }
    close(sub_fd);sub_fd=-1;
    rd=open("/mnt/human/app.log",O_RDONLY);
    if(rd<0){fail("human_append_read_open");goto out;}
    memset(got,0,sizeof(got));
    if(read(rd,got,13)!=13 || memcmp(got,"start\nfinish\n",13)){
        errno=EIO;fail("human_append_data");goto out;
    }
    close(rd);rd=-1;
    msg("VTXFS_VM_HUMAN_OK item=append\n");

    /* fds to unlinked files are required to remain usable by POSIX. */
    if(unlink("/mnt/human/data.bin")){fail("human_unlink");goto out;}
    (void)posix_fadvise(fd,0,sizeof(data),POSIX_FADV_DONTNEED);
    errno=0;
    n=pread(fd,got,sizeof(got),0);
    if(n!=(ssize_t)sizeof(got) || memcmp(got,data,sizeof(got)))
        human_report("DEFECT","unlink_open_fd_read",n<0?errno:(int)n);
    else
        msg("VTXFS_VM_HUMAN_OK item=unlink_open_fd_read\n");
    close(fd);fd=-1;
    if(access("/mnt/human/data.bin",F_OK)==0)
        human_report("DEFECT","unlink_path_still_present",0);

    rd=open("/mnt/human/target.txt",O_RDWR|O_CREAT|O_TRUNC,0644);
    if(rd<0){fail("human_rename_target_open");goto out;}
    if(write(rd,"OLD-TARGET",10)!=10 || fsync(rd)){
        fail("human_rename_target_write");goto out;
    }
    sub_fd=open("/mnt/human/source.txt",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(sub_fd<0){fail("human_rename_source_open");goto out;}
    if(write(sub_fd,"NEW-SOURCE",10)!=10 || fsync(sub_fd)){
        fail("human_rename_source_write");goto out;
    }
    close(sub_fd);sub_fd=-1;
    if(rename("/mnt/human/source.txt","/mnt/human/target.txt")){
        fail("human_rename_over_target");goto out;
    }
    (void)posix_fadvise(rd,0,10,POSIX_FADV_DONTNEED);
    errno=0;
    n=pread(rd,got,10,0);
    if(n!=10 || memcmp(got,"OLD-TARGET",10))
        human_report("DEFECT","rename_open_target_fd_read",n<0?errno:(int)n);
    else
        msg("VTXFS_VM_HUMAN_OK item=rename_open_target_fd_read\n");
    close(rd);rd=-1;
    rd=open("/mnt/human/target.txt",O_RDONLY);
    if(rd<0){fail("human_rename_target_reopen");goto out;}
    if(read(rd,got,10)!=10 || memcmp(got,"NEW-SOURCE",10)){
        errno=EIO;fail("human_rename_new_content");goto out;
    }
    close(rd);rd=-1;
    msg("VTXFS_VM_HUMAN_OK item=rename_reopen_new_content\n");

    if(mkdir("/mnt-second",0755)){fail("human_second_mountpoint");goto out;}
    {
        char ro_opts[64];
        unsigned long long ro_id=human_ro_snapshot_id();
        if(!ro_id){
            human_report("DEFECT","ro_snapshot_id_missing",0);
        }else{
            snprintf(ro_opts,sizeof(ro_opts),"subvol=%llu",ro_id);
            /* Deliberately request RW; readonly must be enforced by VFS. */
            errno=0;
            if(mount("/dev/vdb","/mnt-second","vtxfs",0,ro_opts)){
                human_report("DEFECT","second_volume_ro_snapshot_mount",errno);
            }else{
                int probe;
                mounted2=1;
                msg("VTXFS_VM_HUMAN_OK item=second_volume_mount\n");
                if(statfs("/mnt-second",&fsst) ||
                   !(fsst.f_flags & MS_RDONLY))
                    human_report("DEFECT","readonly_snapshot_sb_flag",errno);
                probe=open("/mnt-second/should-not-create",
                           O_WRONLY|O_CREAT|O_EXCL,0644);
                if(probe>=0){
                    close(probe);
                    human_report("DEFECT","readonly_snapshot_writable",0);
                }else if(errno!=EROFS){
                    human_report("DEFECT","readonly_snapshot_errno",errno);
                }else
                    msg("VTXFS_VM_HUMAN_OK item=readonly_snapshot_vfs\n");
            }
        }
    }
    if(mounted2 && umount("/mnt-second"))fail("human_second_unmount");
    human_copy_file_range_cache_test();
    human_readahead_group_test();
    msg("VTXFS_VM_HUMAN_FINISHED defects=%u missing=%u\n",
        human_defects,human_missing);
out:
    if(sub_fd>=0)close(sub_fd);
    if(rd>=0)close(rd);
    if(bd>=0)close(bd);
    if(fd>=0)close(fd);
}

static void human_verify_after_remount(void)
{
    int fd;
    unsigned char buf[8192];
    struct stat st;
    struct statfs sf;
    unsigned int i;
    fd=open("/mnt/human/docs/hardlink-other-dir",O_RDONLY);
    if(fd<0) {fail("human_remount_hardlink_open");return;}
    memset(buf,0,sizeof(buf));
    if(read(fd,buf,13)!=13 || memcmp(buf,"HARDLINK-DATA",13) ||
       fstat(fd,&st) || st.st_nlink!=1)
        fail("human_remount_hardlink_data");
    close(fd);
    if(access("/mnt/human/data.bin",F_OK)==0)fail("human_remount_unlinked");
    fd=open("/mnt/human/target.txt",O_RDONLY);
    if(fd<0)fail("human_remount_rename_open");
    else{
        if(read(fd,buf,10)!=10 || memcmp(buf,"NEW-SOURCE",10))
            fail("human_remount_rename_data");
        close(fd);
    }
    fd=open("/mnt/human/preallocated.bin",O_RDONLY);
    if(fd<0)fail("human_remount_preallocation_open");
    else{
        if(read(fd,buf,sizeof(buf))!=sizeof(buf))
            fail("human_remount_preallocation_len");
        else for(i=0;i<sizeof(buf);i++)
            if(buf[i]){fail("human_remount_preallocation_nonzero");break;}
        close(fd);
    }
    if(statfs("/mnt",&sf)||sf.f_ffree==0)
        fail("human_remount_inode_free");
    if(!fail_count)
        msg("VTXFS_VM_HUMAN_REOPEN_OK hardlink=1 rename=1 prealloc=1 orphan=1\n");
}

/* Crash while an unlinked FD is still live. No close, no unmount:
 * after a new guest boot, orphan recovery MUST reclaim it safely. */
static void human_orphan_powercut(void)
{
    int fd=-1,dirfd=-1;
    unsigned char buf[4096], verify[4096];
    memset(buf,0x9c,sizeof(buf));
    if(mkdir("/mnt/orphan-test",0755)){fail("orphancut_mkdir");return;}
    fd=open("/mnt/orphan-test/unlinked.bin",O_CREAT|O_EXCL|O_RDWR,0644);
    if(fd<0){fail("orphancut_open");return;}
    if(write(fd,buf,sizeof(buf))!=(ssize_t)sizeof(buf) ||
       fsync(fd)){fail("orphancut_write");return;}
    if(unlink("/mnt/orphan-test/unlinked.bin")){
        fail("orphancut_unlink");return;
    }
    if(pread(fd,verify,sizeof(verify),0)!=(ssize_t)sizeof(verify) ||
       memcmp(buf,verify,sizeof(buf))){
        fail("orphancut_open_fd_data");return;
    }
    dirfd=open("/mnt/orphan-test",O_RDONLY|O_DIRECTORY);
    if(dirfd<0 || fsync(dirfd)){
        fail("orphancut_dir_fsync");return;
    }
    close(dirfd);
    if(!fail_count){
        msg("VTXFS_VM_ORPHAN_CUT_READY opened_fd=1 unlinked=1\n");
        for(;;)pause(); /* controlled by host killing ONLY this QEMU */
    }
    close(fd);
}
static void human_orphan_recover(void)
{
    int dirfd;
    struct statfs s;
    if(access("/mnt/orphan-test/unlinked.bin",F_OK)==0)
        fail("orphan_recover_name_reappeared");
    if(statfs("/mnt",&s) || s.f_ffree==0)
        fail("orphan_recover_statfs");
    dirfd=open("/mnt/orphan-test",O_RDONLY|O_DIRECTORY);
    if(dirfd<0 || fsync(dirfd))
        fail("orphan_recover_fsync");
    if(dirfd>=0)close(dirfd);
    if(!fail_count)
        msg("VTXFS_VM_ORPHAN_RECOVER_OK name_absent=1\n");
}
