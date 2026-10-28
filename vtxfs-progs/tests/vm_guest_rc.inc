/* SPDX-License-Identifier: GPL-2.0-only
 * RC guest workloads:
 *   - Live VTX_IOC_SCRUB and VTX_IOC_OMAP_LOOKUP verification
 *   - vtx_mode=dmlogwrites: kernel dm-log-writes FUA & mark logging
 *   - vtx_mode=bench: verified cold-remount I/O & scrub benchmark
 */
#include <linux/dm-ioctl.h>
#include <stdint.h>
#include <sys/sysmacros.h>
#include <time.h>
#include "../uapi/vtxfs.h"

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static unsigned char rc_stream_byte(uint64_t off)
{
    uint64_t x = off * 0x9E3779B97F4A7C15ULL + 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    return (unsigned char)(x & 0xFFU);
}

static void fill_rc_stream(unsigned char *buf, size_t len, uint64_t base_off)
{
    size_t i;
    for (i = 0; i < len; i++)
        buf[i] = rc_stream_byte(base_off + i);
}

static void verify_live_rc_ioctls(const char *mnt_path)
{
    struct vtx_ioc_capabilities caps;
    struct vtx_ioc_omap_lookup omap_req;
    struct vtx_ioc_scrub_args scrub_req;
    int fd = open(mnt_path, O_RDONLY | O_DIRECTORY);

    if (fd < 0) {
        fail("rc_ioctl_open");
        return;
    }

    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    if (ioctl(fd, VTXFS_IOC_GET_CAPABILITIES, &caps) != 0 ||
        !(caps.capabilities & VTXFS_CAP_SCRUB)) {
        fail("rc_ioctl_capabilities");
        close(fd);
        return;
    }

    memset(&omap_req, 0, sizeof(omap_req));
    omap_req.struct_size = sizeof(omap_req);
    omap_req.subvol_id = 0;
    omap_req.oid = 1ULL; /* VTXFS_OID_ROOT_TREE */
    if (ioctl(fd, VTX_IOC_OMAP_LOOKUP, &omap_req) != 0 ||
        omap_req.resolved_vblk == 0 || omap_req.resolved_pblk == 0) {
        fail("rc_ioctl_omap_lookup");
        close(fd);
        return;
    }
    msg("VTXFS_VM_OMAP_IOCTL_OK vblk=0x%llx pblk=%llu xid=%llu\n",
        (unsigned long long)omap_req.resolved_vblk,
        (unsigned long long)omap_req.resolved_pblk,
        (unsigned long long)omap_req.resolved_xid);

    memset(&scrub_req, 0, sizeof(scrub_req));
    scrub_req.struct_size = sizeof(scrub_req);
    scrub_req.flags = VTX_SCRUB_VERIFY_ONLY;
    if (ioctl(fd, VTX_IOC_SCRUB, &scrub_req) != 0 ||
        scrub_req.meta_blocks_scrubbed < 8ULL ||
        scrub_req.checksum_errors != 0ULL ||
        scrub_req.uncorrectable_errors != 0ULL) {
        fail("rc_ioctl_scrub");
        close(fd);
        return;
    }
    msg("VTXFS_VM_SCRUB_IOCTL_OK data=%llu meta=%llu errs=%llu\n",
        (unsigned long long)scrub_req.data_blocks_scrubbed,
        (unsigned long long)scrub_req.meta_blocks_scrubbed,
        (unsigned long long)scrub_req.checksum_errors);
    close(fd);
}

static int open_dm_control(void)
{
    int fd = open("/dev/mapper/control", O_RDWR);
    if (fd >= 0)
        return fd;
    mkdir("/dev/mapper", 0755);
    {
        FILE *f = fopen("/sys/class/misc/device-mapper/dev", "r");
        unsigned int maj = 10, min = 236;
        if (f) {
            if (fscanf(f, "%u:%u", &maj, &min) != 2) {
                maj = 10;
                min = 236;
            }
            fclose(f);
        }
        mknod("/dev/mapper/control", S_IFCHR | 0600, makedev(maj, min));
    }
    return open("/dev/mapper/control", O_RDWR);
}

static void init_dm_ioctl(struct dm_ioctl *dmi, size_t total_size,
                          const char *name)
{
    memset(dmi, 0, total_size);
    dmi->version[0] = 4;
    dmi->version[1] = 0;
    dmi->version[2] = 0;
    dmi->data_size = (uint32_t)total_size;
    dmi->data_start = sizeof(struct dm_ioctl);
    if (name)
        strncpy(dmi->name, name, DM_NAME_LEN - 1);
}

static void dm_log_writes_workload(void)
{
    enum {
        DMLOG_DATA_SIZE = 4 * 1024 * 1024,
        DMLOG_CHUNK = 65536
    };
    unsigned char dm_buf[4096];
    struct dm_ioctl *dmi = (struct dm_ioctl *)dm_buf;
    unsigned char *chunk = NULL;
    uint64_t vda_bytes = 0, sectors = 0;
    uint64_t dm_dev = 0;
    int ctl_fd = -1, vda_fd = -1, fd = -1, dirfd = -1;
    int dm_created = 0, mounted = 0;
    size_t off;

    vda_fd = open("/dev/vda", O_RDONLY);
    if (vda_fd < 0 || ioctl(vda_fd, BLKGETSIZE64, &vda_bytes) != 0) {
        fail("dmlog_vda_size");
        if (vda_fd >= 0)
            close(vda_fd);
        return;
    }
    close(vda_fd);
    sectors = vda_bytes / 512ULL;

    ctl_fd = open_dm_control();
    if (ctl_fd < 0) {
        fail("dmlog_open_ctl");
        return;
    }

    /* 1. Create DM device "vtxlog" */
    init_dm_ioctl(dmi, sizeof(dm_buf), "vtxlog");
    if (ioctl(ctl_fd, DM_DEV_CREATE, dmi) != 0) {
        fail("dmlog_dev_create");
        close(ctl_fd);
        return;
    }
    dm_dev = dmi->dev;
    dm_created = 1;

    /* 2. Load table: "0 <sectors> log-writes /dev/vda /dev/vdb" */
    init_dm_ioctl(dmi, sizeof(dm_buf), "vtxlog");
    dmi->target_count = 1;
    {
        struct dm_target_spec *spec =
            (struct dm_target_spec *)(dm_buf + dmi->data_start);
        char *params = (char *)(spec + 1);
        size_t param_len;

        spec->sector_start = 0;
        spec->length = sectors;
        spec->status = 0;
        strcpy(spec->target_type, "log-writes");
        strcpy(params, "/dev/vda /dev/vdb");
        param_len = (strlen(params) + 1U + 7U) & ~7U;
        spec->next = (uint32_t)(sizeof(*spec) + param_len);
    }
    if (ioctl(ctl_fd, DM_TABLE_LOAD, dmi) != 0) {
        fail("dmlog_table_load");
        goto out;
    }

    /* 3. Resume (activate) "vtxlog" */
    init_dm_ioctl(dmi, sizeof(dm_buf), "vtxlog");
    if (ioctl(ctl_fd, DM_DEV_SUSPEND, dmi) != 0) {
        fail("dmlog_dev_resume");
        goto out;
    }

    unlink("/dev/vtxlog");
    if (mknod("/dev/vtxlog", S_IFBLK | 0600, (dev_t)dm_dev) != 0) {
        fail("dmlog_mknod");
        goto out;
    }

    /* 4. Mount /dev/vtxlog and run durable write + fsync workload */
    if (mount("/dev/vtxlog", "/mnt", "vtxfs", 0, "") != 0) {
        fail("dmlog_mount");
        goto out;
    }
    mounted = 1;
    msg("VTXFS_VM_MOUNT_OK\n");

    chunk = malloc(DMLOG_CHUNK);
    if (!chunk) {
        fail("dmlog_malloc");
        goto out;
    }

    fd = open("/mnt/durable.bin", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fail("dmlog_open_durable");
        goto out;
    }
    for (off = 0; off < DMLOG_DATA_SIZE; off += DMLOG_CHUNK) {
        fill_rc_stream(chunk, DMLOG_CHUNK, off);
        if (write(fd, chunk, DMLOG_CHUNK) != DMLOG_CHUNK) {
            fail("dmlog_write");
            goto out;
        }
    }
    if (fsync(fd) != 0) {
        fail("dmlog_fsync_file");
        goto out;
    }
    close(fd);
    fd = -1;

    dirfd = open("/mnt", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0 || fsync(dirfd) != 0) {
        fail("dmlog_fsync_dir");
        goto out;
    }
    close(dirfd);
    dirfd = -1;

    /* 5. Emit "mark fsync" into dm-log-writes log */
    init_dm_ioctl(dmi, sizeof(dm_buf), "vtxlog");
    {
        struct dm_target_msg *tmsg =
            (struct dm_target_msg *)(dm_buf + dmi->data_start);
        tmsg->sector = 0;
        strcpy(tmsg->message, "mark fsync");
    }
    if (ioctl(ctl_fd, DM_TARGET_MSG, dmi) != 0) {
        fail("dmlog_mark_fsync");
        goto out;
    }

    if (umount("/mnt") != 0) {
        fail("dmlog_umount");
        goto out;
    }
    mounted = 0;
    msg("VTXFS_VM_UMOUNT_OK\n");

    init_dm_ioctl(dmi, sizeof(dm_buf), "vtxlog");
    if (ioctl(ctl_fd, DM_DEV_REMOVE, dmi) != 0) {
        fail("dmlog_dev_remove");
        goto out;
    }
    dm_created = 0;

    if (!fail_count) {
        msg("VTXFS_VM_DMLOGWRITES_OK bytes=%u sectors=%llu\n",
            (unsigned int)DMLOG_DATA_SIZE, (unsigned long long)sectors);
    }

out:
    if (fd >= 0)
        close(fd);
    if (dirfd >= 0)
        close(dirfd);
    free(chunk);
    if (mounted)
        umount("/mnt");
    if (dm_created) {
        init_dm_ioctl(dmi, sizeof(dm_buf), "vtxlog");
        ioctl(ctl_fd, DM_DEV_REMOVE, dmi);
    }
    if (ctl_fd >= 0)
        close(ctl_fd);
}

static uint64_t read_vda_write_sectors(void)
{
    FILE *f = fopen("/sys/block/vda/stat", "r");
    unsigned long long r_ios = 0, r_merges = 0, r_sec = 0, r_ticks = 0;
    unsigned long long w_ios = 0, w_merges = 0, w_sec = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%llu %llu %llu %llu %llu %llu %llu",
               &r_ios, &r_merges, &r_sec, &r_ticks,
               &w_ios, &w_merges, &w_sec) != 7) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return (uint64_t)w_sec;
}

static void verified_bench_workload(void)
{
    enum {
        BENCH_TOTAL_BYTES = 64 * 1024 * 1024,
        BENCH_CHUNK_BYTES = 1024 * 1024
    };
    unsigned char *wbuf = NULL, *rbuf = NULL;
    uint64_t before_wsec, after_wsec, disk_written;
    uint64_t t0, write_ns, read_ns;
    struct vtx_ioc_scrub_args scrub_req;
    int fd = -1, dirfd = -1, mounted = 0;
    size_t off;

    wbuf = malloc(BENCH_CHUNK_BYTES);
    rbuf = malloc(BENCH_CHUNK_BYTES);
    if (!wbuf || !rbuf) {
        fail("bench_malloc");
        goto out;
    }

    before_wsec = read_vda_write_sectors();
    if (mount("/dev/vda", "/mnt", "vtxfs", 0, "") != 0) {
        fail("bench_mount_rw");
        goto out;
    }
    mounted = 1;
    msg("VTXFS_VM_MOUNT_OK\n");

    t0 = monotonic_ns();
    fd = open("/mnt/bench.bin", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fail("bench_open_write");
        goto out;
    }
    for (off = 0; off < BENCH_TOTAL_BYTES; off += BENCH_CHUNK_BYTES) {
        fill_rc_stream(wbuf, BENCH_CHUNK_BYTES, off);
        if (write(fd, wbuf, BENCH_CHUNK_BYTES) != BENCH_CHUNK_BYTES) {
            fail("bench_write");
            goto out;
        }
        msg("BENCH_CHUNK %zu/%d\n", off + BENCH_CHUNK_BYTES, BENCH_TOTAL_BYTES);
    }
    msg("BENCH_FSYNC_START\n");
    if (fsync(fd) != 0) {
        fail("bench_fsync");
        goto out;
    }
    msg("BENCH_FSYNC_DONE\n");
    close(fd);
    fd = -1;

    dirfd = open("/mnt", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0 || syncfs(dirfd) != 0) {
        fail("bench_syncfs");
        goto out;
    }
    write_ns = monotonic_ns() - t0;
    msg("BENCH_SYNCFS_DONE\n");

    memset(&scrub_req, 0, sizeof(scrub_req));
    scrub_req.struct_size = sizeof(scrub_req);
    scrub_req.flags = VTX_SCRUB_VERIFY_ONLY;
    msg("BENCH_SCRUB_START\n");
    if (ioctl(dirfd, VTX_IOC_SCRUB, &scrub_req) != 0 ||
        scrub_req.checksum_errors != 0 ||
        scrub_req.data_blocks_scrubbed < (BENCH_TOTAL_BYTES / 4096)) {
        fail("bench_live_scrub");
        goto out;
    }
    msg("BENCH_SCRUB_DONE\n");
    close(dirfd);
    dirfd = -1;

    if (umount("/mnt") != 0) {
        fail("bench_umount_rw");
        goto out;
    }
    mounted = 0;

    after_wsec = read_vda_write_sectors();
    disk_written = (after_wsec - before_wsec) * 512ULL;
    if (disk_written < BENCH_TOTAL_BYTES) {
        fail("bench_short_disk_writes");
        goto out;
    }

    {
        int dc = open("/proc/sys/vm/drop_caches", O_WRONLY);
        if (dc >= 0) {
            if (write(dc, "3\n", 2) != 2) {
                /* non-fatal */
            }
            close(dc);
        }
    }

    /* Cold remount read-only and verify all 64 MiB */
    if (mount("/dev/vda", "/mnt", "vtxfs", MS_RDONLY, "") != 0) {
        fail("bench_remount_ro");
        goto out;
    }
    mounted = 1;

    t0 = monotonic_ns();
    fd = open("/mnt/bench.bin", O_RDONLY);
    if (fd < 0) {
        fail("bench_open_read");
        goto out;
    }
    for (off = 0; off < BENCH_TOTAL_BYTES; off += BENCH_CHUNK_BYTES) {
        fill_rc_stream(wbuf, BENCH_CHUNK_BYTES, off);
        if (read(fd, rbuf, BENCH_CHUNK_BYTES) != BENCH_CHUNK_BYTES ||
            memcmp(wbuf, rbuf, BENCH_CHUNK_BYTES) != 0) {
            fail("bench_verify_read");
            goto out;
        }
    }
    close(fd);
    fd = -1;
    read_ns = monotonic_ns() - t0;

    if (umount("/mnt") != 0) {
        fail("bench_umount_ro");
        goto out;
    }
    mounted = 0;
    msg("VTXFS_VM_UMOUNT_OK\n");

    if (!fail_count) {
        msg("VTXFS_VM_BENCH_OK bytes=%u write_ns=%llu read_ns=%llu disk_written_bytes=%llu scrub_data=%llu scrub_meta=%llu\n",
            (unsigned int)BENCH_TOTAL_BYTES,
            (unsigned long long)write_ns,
            (unsigned long long)read_ns,
            (unsigned long long)disk_written,
            (unsigned long long)scrub_req.data_blocks_scrubbed,
            (unsigned long long)scrub_req.meta_blocks_scrubbed);
    }

out:
    if (fd >= 0)
        close(fd);
    if (dirfd >= 0)
        close(dirfd);
    if (mounted)
        umount("/mnt");
    free(wbuf);
    free(rbuf);
}
