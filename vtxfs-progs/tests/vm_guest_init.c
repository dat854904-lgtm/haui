/* SPDX-License-Identifier: GPL-2.0-only
 * Disposable QEMU guest PID1; NEVER runs on the host.
 * Guest block device is a newly formatted /dev/shm/ image passed as virtio.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/xattr.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static int fail_count;
static void msg(const char *fmt, ...)
{
    va_list args;
    va_start(args,fmt);
    vprintf(fmt,args);
    va_end(args);
    fflush(stdout);
}
static void fail(const char *stage)
{
    msg("VTXFS_VM_FAILURE step=%s errno=%d (%s)\n",stage,errno,strerror(errno));
    fail_count++;
}
static int modload(const char *p)
{
    struct stat st;
    int fd=open(p,O_RDONLY);
    if(fd<0)return -1;
    if(fstat(fd,&st)||st.st_size<1||st.st_size>64*1024*1024) {close(fd);errno=EINVAL;return -1;}
    char *buf=malloc((size_t)st.st_size);
    if(!buf){close(fd);return -1;}
    size_t have=0;
    while(have<(size_t)st.st_size){
        ssize_t n=read(fd,buf+have,(size_t)st.st_size-have);
        if(n<=0){free(buf);close(fd);errno=EIO;return -1;}
        have+=(size_t)n;
    }
    close(fd);
    int ret=(int)syscall(SYS_init_module,buf,have,"");
    free(buf);
    return ret;
}
static int load_dependencies(void)
{
    char path[512];
    FILE *fp=fopen("/deps/order","r");
    if(!fp){ errno=ENOENT; return -1; }
    while(fgets(path,sizeof(path),fp)){
        size_t len=strcspn(path,"\r\n");
        path[len]='\0';
        if(!len)continue;
        if(modload(path)){
            msg("VTXFS_VM_DEP_FAIL %s errno=%d\n",path,errno);
            fclose(fp);
            return -1;
        }
        msg("VTXFS_VM_DEP_OK %s\n",path);
    }
    fclose(fp);
    return 0;
}
static int guest_mode(void)
{
    char cmdline[2048]={0};
    int fd=open("/proc/cmdline",O_RDONLY);
    ssize_t got;
    if(fd<0)return 0;
    got=read(fd,cmdline,sizeof(cmdline)-1);
    close(fd);
    if(got<=0)return 0;
    if(strstr(cmdline,"vtx_mode=dmlogwrites"))return 10;
    if(strstr(cmdline,"vtx_mode=bench"))return 11;
    if(strstr(cmdline,"vtx_mode=powercut"))return 1;
    if(strstr(cmdline,"vtx_mode=recover"))return 2;
    if(strstr(cmdline,"vtx_mode=stressverify"))return 5;
    if(strstr(cmdline,"vtx_mode=orphancut"))return 8;
    if(strstr(cmdline,"vtx_mode=orphanrecover"))return 9;
    if(strstr(cmdline,"vtx_mode=humanverify"))return 7;
    if(strstr(cmdline,"vtx_mode=human"))return 6;
    if(strstr(cmdline,"vtx_mode=stress"))return 3;
    if(strstr(cmdline,"vtx_mode=reject"))return 4;
    return 0;
}
static unsigned char crash_pattern(size_t n)
{
    return (unsigned char)((n*17U+53U)&255U);
}
static void crash_writer(void)
{
    int fd, dfd;
    unsigned char buf[4096];
    size_t i,j;
    if(mkdir("/mnt/recovery",0755)){fail("crash_mkdir");return;}
    fd=open("/mnt/recovery/staging.bin",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd<0){fail("crash_open");return;}
    for(i=0;i<24;i++){
        for(j=0;j<sizeof(buf);j++)
            buf[j]=crash_pattern(i*sizeof(buf)+j);
        if(write(fd,buf,sizeof(buf))!=(ssize_t)sizeof(buf)){
            fail("crash_write");break;
        }
    }
    if(fsync(fd))fail("crash_file_fsync");
    if(close(fd))fail("crash_close");
    if(rename("/mnt/recovery/staging.bin","/mnt/recovery/durable.bin"))
        fail("crash_rename");
    dfd=open("/mnt/recovery",O_RDONLY|O_DIRECTORY);
    if(dfd<0)fail("crash_dir_open");
    else {if(fsync(dfd))fail("crash_dir_fsync");close(dfd);}
    if(!fail_count){
        msg("VTXFS_VM_CRASH_READY durable_bytes=98304\n");
        for(;;)pause();
    }
}
static void crash_recovery_verify(void)
{
    int fd;
    unsigned char buf[4096];
    struct stat st;
    size_t i,j;
    fd=open("/mnt/recovery/durable.bin",O_RDONLY);
    if(fd<0){fail("recovery_open_durable");return;}
    for(i=0;i<24;i++){
        if(read(fd,buf,sizeof(buf))!=(ssize_t)sizeof(buf)){
            fail("recovery_read_len");break;
        }
        for(j=0;j<sizeof(buf);j++)
            if(buf[j]!=crash_pattern(i*sizeof(buf)+j)){
                errno=EIO;fail("recovery_data_mismatch");goto done;
            }
    }
done:
    if(fstat(fd,&st)||st.st_size!=24*4096)fail("recovery_size");
    close(fd);
    if(access("/mnt/recovery/staging.bin",F_OK)==0)
        fail("recovery_old_name_persisted");
    if(!fail_count)msg("VTXFS_VM_RECOVER_DATA_OK bytes=98304\n");
}
#define VM_STRESS_WORKERS 4
#define VM_STRESS_FILES 8
struct stress_worker_state {int thread_id; int error;};
static void *stress_writer_thread(void *arg)
{
    struct stress_worker_state *st=arg;
    char before[128], after[128];
    unsigned char buf[4096];
    int i,fd;
    for(i=0;i<VM_STRESS_FILES;i++){
        snprintf(before,sizeof(before),"/mnt/stress/t%d-%02d.partial",
                 st->thread_id,i);
        snprintf(after,sizeof(after),"/mnt/stress/t%d-%02d.bin",
                 st->thread_id,i);
        fd=open(before,O_CREAT|O_EXCL|O_RDWR,0644);
        if(fd<0){st->error=errno;return NULL;}
        memset(buf,st->thread_id*16+i+1,sizeof(buf));
        if(write(fd,buf,sizeof(buf))!=(ssize_t)sizeof(buf)){
            st->error=errno?errno:EIO;close(fd);return NULL;
        }
        if(fsync(fd)){st->error=errno;close(fd);return NULL;}
        if(close(fd)){st->error=errno;return NULL;}
        if(rename(before,after)){st->error=errno;return NULL;}
    }
    return NULL;
}
static void stress_workload(void)
{
    struct stress_worker_state states[VM_STRESS_WORKERS]={0};
    pthread_t threads[VM_STRESS_WORKERS];
    char path[128];
    int i,j,fd,created=0;
    unsigned char buf[4096];

    if(mkdir("/mnt/stress",0755)){fail("stress_mkdir");return;}
    for(i=0;i<VM_STRESS_WORKERS;i++){
        states[i].thread_id=i;
        if(pthread_create(&threads[i],NULL,stress_writer_thread,&states[i])){
            fail("stress_pthread_create");break;
        }
        created++;
    }
    for(i=0;i<created;i++){
        if(pthread_join(threads[i],NULL))fail("stress_pthread_join");
        if(states[i].error){errno=states[i].error;fail("stress_thread_error");}
    }
    if(fail_count)return;
    for(i=0;i<VM_STRESS_WORKERS;i++){
        for(j=0;j<VM_STRESS_FILES;j++){
            void *mapped;
            snprintf(path,sizeof(path),"/mnt/stress/t%d-%02d.bin",i,j);
            fd=open(path,O_RDWR);
            if(fd<0){fail("stress_verify_open");return;}
            mapped=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0);
            if(mapped==MAP_FAILED){
                fail("stress_mmap_read");close(fd);return;
            }
            memset(buf,i*16+j+1,sizeof(buf));
            if(memcmp(mapped,buf,sizeof(buf)))fail("stress_mmap_mismatch");
            munmap(mapped,4096);
            close(fd);
        }
    }
    msg("VTXFS_VM_STRESS_OK threads=4 files=32\n");
    /* Exercise dirty MAP_SHARED pages and verified buffered readback. */
    fd=open("/mnt/stress/t0-00.bin",O_RDWR);
    if(fd<0){fail("mmap_rw_open");return;}
    {
        unsigned char *mapped=mmap(NULL,4096,PROT_READ|PROT_WRITE,
                                    MAP_SHARED,fd,0);
        if(mapped==MAP_FAILED)fail("mmap_rw_map");
        else {
            mapped[17]=0xb7;
            if(msync(mapped,4096,MS_SYNC))fail("mmap_rw_msync");
            if(munmap(mapped,4096))fail("mmap_rw_unmap");
        }
    }
    if(fsync(fd))fail("mmap_rw_fsync");
    if(pread(fd,buf,sizeof(buf),0)!=(ssize_t)sizeof(buf) ||
       buf[17]!=0xb7){errno=EIO;fail("mmap_rw_verify");}
    else msg("VTXFS_VM_MMAP_RW_OK\n");
    close(fd);
}

static void stress_verify_after_remount(void)
{
    char path[128];
    unsigned char got[4096], expected[4096];
    int i,j,fd;
    struct stat st;
    for(i=0;i<VM_STRESS_WORKERS;i++){
        for(j=0;j<VM_STRESS_FILES;j++){
            snprintf(path,sizeof(path),"/mnt/stress/t%d-%02d.bin",i,j);
            fd=open(path,O_RDONLY);
            if(fd<0){fail("stress_remount_open");return;}
            if(fstat(fd,&st)||st.st_size!=4096)
                fail("stress_remount_size");
            memset(expected,i*16+j+1,sizeof(expected));
            if(i==0 && j==0)expected[17]=0xb7;
            if(read(fd,got,sizeof(got))!=4096 ||
               memcmp(expected,got,sizeof(expected))){
                errno=EIO;fail("stress_remount_bytes");
            }
            close(fd);
        }
    }
    if(!fail_count)
        msg("VTXFS_VM_STRESS_REOPEN_OK files=32 mmap_dirty_persisted=1\n");
}

#include "vm_guest_rc.inc"

static void run_workload(void)
{
    int fd=-1;
    struct stat st;
    struct statfs fs;
    const char *data="VTXFS-VM-POSIX-ROUNDTRIP\n";
    char buf[64]={0};
    if(mkdir("/mnt/work",0755)) fail("mkdir");
    fd=open("/mnt/work/test.dat",O_CREAT|O_TRUNC|O_RDWR,0644);
    if(fd<0){fail("open_create");return;}
    if(write(fd,data,strlen(data))!=(ssize_t)strlen(data))fail("write");
    if(fsync(fd))fail("fsync");
    if(lseek(fd,0,SEEK_SET)<0)fail("lseek");
    if(read(fd,buf,strlen(data))!=(ssize_t)strlen(data)||
       memcmp(buf,data,strlen(data))) {errno=EIO;fail("read_verify");}
    if(ftruncate(fd,8))fail("ftruncate");
    if(fsync(fd))fail("fsync_after_truncate");
    close(fd);
    if(rename("/mnt/work/test.dat","/mnt/work/renamed.dat"))fail("rename");
    if(symlink("renamed.dat","/mnt/work/link.dat"))fail("symlink");
    memset(buf,0,sizeof(buf));
    if(readlink("/mnt/work/link.dat",buf,sizeof(buf))!=11 ||
       strcmp(buf,"renamed.dat")){errno=EIO;fail("readlink");}
    if(stat("/mnt/work/renamed.dat",&st)||st.st_size!=8)fail("stat_size");
    msg("VTXFS_VM_STEP xattr\n");
    if(setxattr("/mnt/work/renamed.dat","user.audit","m5",2,0))fail("setxattr");
    memset(buf,0,sizeof(buf));
    if(getxattr("/mnt/work/renamed.dat","user.audit",buf,sizeof(buf))!=2 ||
       memcmp(buf,"m5",2)) {errno=EIO;fail("getxattr");}

    if(statfs("/mnt",&fs)||fs.f_blocks==0)fail("statfs");
    fd=open("/mnt/work/renamed.dat",O_RDONLY);
    if(fd<0)fail("fiemap_open");
    else {
        struct {
            struct fiemap fm;
            struct fiemap_extent ex[8];
        } req={0};
        req.fm.fm_length=4096;
        req.fm.fm_extent_count=8;
        if(ioctl(fd,FS_IOC_FIEMAP,&req)<0)fail("fiemap_ioctl");
        else msg("VTXFS_VM_FIEMAP extents=%u\n",req.fm.fm_mapped_extents);
        close(fd);
    }
    msg("VTXFS_VM_STEP direct_io_read\n");
    {
        void *aligned=NULL;
        int dio;
        fd=open("/mnt/work/direct.bin",O_CREAT|O_TRUNC|O_RDWR,0644);
        if(fd<0)fail("dio_create");
        else {
            char payload[4096];
            memset(payload,0xa5,sizeof(payload));
            if(write(fd,payload,sizeof(payload))!=(ssize_t)sizeof(payload))
                fail("dio_write_prepare");
            if(fsync(fd))fail("dio_fsync_prepare");
            close(fd);
            if(posix_memalign(&aligned,4096,4096))fail("dio_alloc_aligned");
            if(aligned){
                memset(aligned,0,4096);
                dio=open("/mnt/work/direct.bin",O_RDONLY|O_DIRECT);
                if(dio<0)fail("dio_open");
                else {
                    if(read(dio,aligned,4096)!=4096 ||
                       memcmp(aligned,payload,4096)) { errno=EIO;fail("dio_read_verify"); }
                    else msg("VTXFS_VM_DIO_READ_OK\n");
                    close(dio);
                    msg("VTXFS_VM_STEP direct_io_write\n");
                    memset(aligned,0x5a,4096);
                    dio=open("/mnt/work/direct.bin",O_WRONLY|O_DIRECT);
                    if(dio<0)fail("dio_write_open");
                    else {
                        if(write(dio,aligned,4096)!=4096)fail("dio_write");
                        if(fsync(dio))fail("dio_write_fsync");
                        close(dio);
                        dio=open("/mnt/work/direct.bin",O_RDONLY);
                        if(dio<0)fail("dio_buffered_verify_open");
                        else {
                            char verify[4096];
                            if(read(dio,verify,4096)!=4096)
                                fail("dio_buffered_verify_read");
                            else {
                                memset(payload,0x5a,4096);
                                if(memcmp(verify,payload,4096))
                                    fail("dio_buffered_stale_cache");
                                else msg("VTXFS_VM_DIO_WRITE_OK\n");
                            }
                            close(dio);
                        }
                    }
                }
                free(aligned);
            }
        }
    }
    msg("VTXFS_VM_STEP fallocate_eager\n");
    fd=open("/mnt/work/direct.bin",O_RDWR);
    if(fd<0)fail("fallocate_guard_open");
    else {
        int result=(int)syscall(SYS_fallocate,fd,0,0,4096);
        if(result != 0) {
            fail("fallocate_eager_existing_allocated_file");
        } else {
            unsigned char verified[64];
            memset(verified,0,sizeof(verified));
            if(pread(fd,verified,sizeof(verified),0)!=(ssize_t)sizeof(verified))
                fail("fallocate_guard_readback");
            else {
                size_t k;
                for(k=0;k<sizeof(verified);k++)
                    if(verified[k]!=0x5a){errno=EIO;fail("fallocate_clobbered_data");break;}
                if(k==sizeof(verified))msg("VTXFS_VM_FALLOCATE_SAFE_OK\n");
            }
        }
        close(fd);
    }
    msg("VTXFS_VM_STEP rc_ioctls\n");
    verify_live_rc_ioctls("/mnt");
    msg("VTXFS_VM_STEP unlink_link\n");
    if(unlink("/mnt/work/link.dat"))fail("unlink_link");
    if(unlink("/mnt/work/direct.bin"))fail("unlink_dio");
    msg("VTXFS_VM_STEP unlink_data\n");
    if(unlink("/mnt/work/renamed.dat"))fail("unlink_data");
    msg("VTXFS_VM_STEP rmdir\n");
    if(rmdir("/mnt/work"))fail("rmdir");
    msg("VTXFS_VM_STEP syncfs\n");
    fd=open("/mnt",O_RDONLY|O_DIRECTORY);
    if(fd<0)fail("syncfs_open");
    else { if(syncfs(fd))fail("syncfs"); close(fd); }
}
#include "vm_guest_human.inc"

int main(void)
{
    mkdir("/dev",0755);mkdir("/proc",0755);mkdir("/sys",0755);mkdir("/mnt",0755);
    if(mount("devtmpfs","/dev","devtmpfs",0,""))fail("mount_dev");
    if(mount("proc","/proc","proc",0,""))fail("mount_proc");
    if(mount("sysfs","/sys","sysfs",0,""))fail("mount_sys");
    int console=open("/dev/console",O_RDWR);
    if(console>=0){dup2(console,0);dup2(console,1);dup2(console,2);if(console>2)close(console);}
    msg("VTXFS_VM_BOOT PID=%ld\n",(long)getpid());
    if(access("/dev/vda",R_OK))fail("virtio_block_missing");
    if(load_dependencies())fail("load_dependencies");
    if(access("/vtxfs_free_cache_test.ko",F_OK)==0){
        if(modload("/vtxfs_free_cache_test.ko"))fail("m3_cache_selftest");
        else msg("VTXFS_VM_M3_CACHE_MODULE_LOADED\n");
    }
    if(modload("/vtxfs.ko"))fail("insmod");
    else {
        int mode=guest_mode();
        if(mode==10){
            dm_log_writes_workload();
        } else if(mode==11){
            verified_bench_workload();
        } else {
            int mounted=mount("/dev/vda","/mnt","vtxfs",
                              (mode==2||mode==4||mode==5||mode==7) ? MS_RDONLY : 0,"");
            if(mode==4){
                if(mounted==0){
                    fail("corrupted_image_accepted");
                    umount("/mnt");
                } else if(errno==EUCLEAN||errno==EINVAL||errno==EIO){
                    msg("VTXFS_VM_REJECT_OK errno=%d\n",errno);
                } else fail("corrupted_image_wrong_errno");
            } else if(mounted)
                fail("mount_vtxfs");
            else {
                msg("VTXFS_VM_MOUNT_OK\n");
                if(mode==1)crash_writer();
                else if(mode==2)crash_recovery_verify();
                else if(mode==3)stress_workload();
                else if(mode==5)stress_verify_after_remount();
                else if(mode==6)human_workload();
                else if(mode==7)human_verify_after_remount();
                else if(mode==8)human_orphan_powercut();
                else if(mode==9)human_orphan_recover();
                else run_workload();
                if(umount("/mnt"))fail("umount");
                else msg("VTXFS_VM_UMOUNT_OK\n");
            }
        }
    }
    msg("VTXFS_VM_%s failures=%d\n",fail_count ? "FAIL":"PASS",fail_count);
    sync();
    reboot(RB_POWER_OFF);
    for(;;)pause();
}
