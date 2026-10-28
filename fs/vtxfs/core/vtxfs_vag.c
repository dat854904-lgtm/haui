/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __KERNEL__
#define _POSIX_C_SOURCE 200809L
#endif
#include "vtxfs_engine.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/bitops.h>
static inline void *vtx_zalloc(size_t size) { return vzalloc(size); }
static inline void vtx_free(void *ptr) { vfree(ptr); }
static inline __u32 vtx_popcount64(__u64 x) { return (__u32)hweight64(x); }
#else
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
static inline void *vtx_zalloc(size_t size) { return calloc(1, size); }
static inline void vtx_free(void *ptr) { free(ptr); }
static inline __u32 vtx_popcount64(__u64 x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (__u32)((x * 0x0101010101010101ULL) >> 56);
}
#endif

/* -------------------------------------------------------------------------
 * 1. Sparse RAM Block Device (4 KiB blocks in open-addressed hash table)
 * ------------------------------------------------------------------------- */

#define RAM_BUCKETS 16384U

struct ram_block_node {
    __u64 pblk;
    struct ram_block_node *next;
    __u8 data[VTXFS_BLOCK_SIZE];
};

struct ram_bdev_priv {
    struct ram_block_node *buckets[RAM_BUCKETS];
};

static unsigned int ram_hash(__u64 pblk)
{
    return (unsigned int)((pblk * 0x9E3779B97F4A7C15ULL) >> 50) &
           (RAM_BUCKETS - 1U);
}

static int ram_read_blocks(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                           void *buf)
{
    struct ram_bdev_priv *priv = (struct ram_bdev_priv *)bdev->priv;
    __u32 i;

    if (!priv || !buf || pblk >= bdev->total_blocks ||
        count > bdev->total_blocks - pblk)
        return -EIO;
    bdev->reads_issued++;
    for (i = 0; i < count; i++) {
        __u64 cur = pblk + i;
        struct ram_block_node *n = priv->buckets[ram_hash(cur)];
        __u8 *dst = (__u8 *)buf + (size_t)i * VTXFS_BLOCK_SIZE;
        while (n && n->pblk != cur)
            n = n->next;
        if (n)
            memcpy(dst, n->data, VTXFS_BLOCK_SIZE);
        else
            memset(dst, 0, VTXFS_BLOCK_SIZE);
    }
    return 0;
}

static int ram_write_blocks(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                            const void *buf)
{
    struct ram_bdev_priv *priv = (struct ram_bdev_priv *)bdev->priv;
    __u32 i;

    if (!priv || !buf || pblk >= bdev->total_blocks ||
        count > bdev->total_blocks - pblk)
        return -EIO;
    bdev->writes_issued++;
    for (i = 0; i < count; i++) {
        __u64 cur = pblk + i;
        unsigned int h = ram_hash(cur);
        struct ram_block_node *n = priv->buckets[h];
        const __u8 *src = (const __u8 *)buf + (size_t)i * VTXFS_BLOCK_SIZE;
        while (n && n->pblk != cur)
            n = n->next;
        if (!n) {
            n = (struct ram_block_node *)vtx_zalloc(sizeof(*n));
            if (!n)
                return -ENOMEM;
            n->pblk = cur;
            n->next = priv->buckets[h];
            priv->buckets[h] = n;
        }
        memcpy(n->data, src, VTXFS_BLOCK_SIZE);
    }
    return 0;
}

static int ram_write_fua(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                         const void *buf)
{
    bdev->fuas_issued++;
    return ram_write_blocks(bdev, pblk, count, buf);
}

static int ram_flush(struct vtx_bdev *bdev)
{
    bdev->flushes_issued++;
    return 0;
}

static const struct vtx_bdev_ops ram_bdev_ops = {
    .read_blocks = ram_read_blocks,
    .write_blocks = ram_write_blocks,
    .write_fua = ram_write_fua,
    .flush = ram_flush
};

int vtx_ram_bdev_init(struct vtx_bdev *bdev, __u64 total_blocks)
{
    struct ram_bdev_priv *priv;
    if (!bdev || total_blocks < VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS)
        return -EINVAL;
    priv = (struct ram_bdev_priv *)vtx_zalloc(sizeof(*priv));
    if (!priv)
        return -ENOMEM;
    memset(bdev, 0, sizeof(*bdev));
    bdev->ops = &ram_bdev_ops;
    bdev->priv = priv;
    bdev->total_blocks = total_blocks;
    return 0;
}

void vtx_ram_bdev_destroy(struct vtx_bdev *bdev)
{
    struct ram_bdev_priv *priv;
    unsigned int i;
    if (!bdev || !bdev->priv)
        return;
    priv = (struct ram_bdev_priv *)bdev->priv;
    for (i = 0; i < RAM_BUCKETS; i++) {
        struct ram_block_node *n = priv->buckets[i];
        while (n) {
            struct ram_block_node *next = n->next;
            vtx_free(n);
            n = next;
        }
    }
    vtx_free(priv);
    bdev->priv = NULL;
}

/* -------------------------------------------------------------------------
 * 2. Userspace File-Backed Block Device
 * ------------------------------------------------------------------------- */
#ifndef __KERNEL__

struct file_bdev_priv {
    int fd;
    int writable;
};

static int file_read_blocks(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                            void *buf)
{
    struct file_bdev_priv *fp = (struct file_bdev_priv *)bdev->priv;
    size_t total = (size_t)count * VTXFS_BLOCK_SIZE;
    off_t off = (off_t)(pblk * VTXFS_BLOCK_SIZE);
    size_t done = 0;

    if (!fp || !buf || pblk >= bdev->total_blocks ||
        count > bdev->total_blocks - pblk)
        return -EIO;
    bdev->reads_issued++;
    while (done < total) {
        ssize_t r = pread(fp->fd, (__u8 *)buf + done, total - done,
                          off + (off_t)done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -EIO;
        }
        if (r == 0) {
            memset((__u8 *)buf + done, 0, total - done);
            break;
        }
        done += (size_t)r;
    }
    return 0;
}

static int file_write_blocks(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                             const void *buf)
{
    struct file_bdev_priv *fp = (struct file_bdev_priv *)bdev->priv;
    size_t total = (size_t)count * VTXFS_BLOCK_SIZE;
    off_t off = (off_t)(pblk * VTXFS_BLOCK_SIZE);
    size_t done = 0;

    if (!fp || !fp->writable || !buf || pblk >= bdev->total_blocks ||
        count > bdev->total_blocks - pblk)
        return -EIO;
    bdev->writes_issued++;
    while (done < total) {
        ssize_t w = pwrite(fp->fd, (const __u8 *)buf + done, total - done,
                           off + (off_t)done);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -EIO;
        }
        done += (size_t)w;
    }
    return 0;
}

static int file_flush(struct vtx_bdev *bdev)
{
    struct file_bdev_priv *fp = (struct file_bdev_priv *)bdev->priv;
    if (!fp)
        return -EIO;
    bdev->flushes_issued++;
    return fsync(fp->fd) == 0 ? 0 : -EIO;
}

static int file_write_fua(struct vtx_bdev *bdev, __u64 pblk, __u32 count,
                          const void *buf)
{
    int ret;
    bdev->fuas_issued++;
    ret = file_write_blocks(bdev, pblk, count, buf);
    if (ret)
        return ret;
    return file_flush(bdev);
}

static const struct vtx_bdev_ops file_bdev_ops = {
    .read_blocks = file_read_blocks,
    .write_blocks = file_write_blocks,
    .write_fua = file_write_fua,
    .flush = file_flush
};

int vtx_file_bdev_open(struct vtx_bdev *bdev, const char *path,
                       int create_or_rw, __u64 create_blocks)
{
    struct file_bdev_priv *fp;
    struct stat st;
    int flags, fd, is_blk = 0;
    __u64 blocks = create_blocks;

    if (!bdev || !path)
        return -EINVAL;

    if (stat(path, &st) == 0 && S_ISBLK(st.st_mode)) {
        flags = (create_or_rw > 0) ? O_RDWR : O_RDONLY;
        is_blk = 1;
    } else {
        if (create_or_rw >= 2)
            flags = O_RDWR | O_CREAT | O_TRUNC;
        else if (create_or_rw == 1)
            flags = O_RDWR;
        else
            flags = O_RDONLY;
    }

    fd = open(path, flags, 0644);
    if (fd < 0)
        return -errno;

    if (fstat(fd, &st) != 0) {
        close(fd);
        return -errno;
    }
    if (S_ISBLK(st.st_mode))
        is_blk = 1;

    if (is_blk) {
        __u64 dev_bytes = 0;
        if (ioctl(fd, BLKGETSIZE64, &dev_bytes) != 0 || dev_bytes == 0) {
            close(fd);
            return -EIO;
        }
        blocks = dev_bytes / VTXFS_BLOCK_SIZE;
        if (create_blocks > 0 && create_blocks < blocks)
            blocks = create_blocks;
        if (blocks < VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS) {
            close(fd);
            return -EINVAL;
        }
    } else if (create_or_rw >= 2) {
        if (blocks < VTXFS_FIRST_VAG_PBLK + VTXFS_VAG_BLOCKS) {
            close(fd);
            return -EINVAL;
        }
        if (ftruncate(fd, (off_t)(blocks * VTXFS_BLOCK_SIZE)) != 0) {
            close(fd);
            return -EIO;
        }
    } else {
        blocks = (__u64)st.st_size / VTXFS_BLOCK_SIZE;
    }

    fp = (struct file_bdev_priv *)vtx_zalloc(sizeof(*fp));
    if (!fp) {
        close(fd);
        return -ENOMEM;
    }
    fp->fd = fd;
    fp->writable = (create_or_rw > 0);
    memset(bdev, 0, sizeof(*bdev));
    bdev->ops = &file_bdev_ops;
    bdev->priv = fp;
    bdev->total_blocks = blocks;
    return 0;
}

void vtx_file_bdev_close(struct vtx_bdev *bdev)
{
    struct file_bdev_priv *fp;
    if (!bdev || !bdev->priv)
        return;
    fp = (struct file_bdev_priv *)bdev->priv;
    if (fp->fd >= 0)
        close(fp->fd);
    vtx_free(fp);
    bdev->priv = NULL;
}
#endif /* !__KERNEL__ */

/* -------------------------------------------------------------------------
 * 3. Two-Level Physical VAG Directory & Block Allocator
 * ------------------------------------------------------------------------- */

void vtx_vag_destroy(struct vtx_vag_state *vag)
{
    __u32 i;
    if (!vag)
        return;
    for (i = 0; i < VTX_MAX_SUPPORTED_VAGS; i++) {
        if (vag->bitmaps[i]) {
            vtx_free(vag->bitmaps[i]);
            vag->bitmaps[i] = NULL;
        }
    }
    memset(vag, 0, sizeof(*vag));
}

int vtx_vag_init_empty(struct vtx_vag_state *vag, __u32 total_vags,
                       __u64 xid, const __u8 metadata_uuid[VTXFS_UUID_SIZE])
{
    __u32 i;
    __u64 slot = VTXFS_CPD_SLOT_FOR_XID(xid);
    __u64 slot_base = VTXFS_IP_BOOT_PBLK + slot * VTX_IP_SLOT_STRIDE;

    if (!vag || !metadata_uuid || total_vags == 0 ||
        total_vags > VTX_MAX_SUPPORTED_VAGS)
        return -EINVAL;

    memset(vag, 0, sizeof(*vag));
    vag->total_vags = total_vags;
    vag->root.hdr.magic = vtx_le32(VTXFS_VAG_DIR_MAGIC);
    vag->root.hdr.flags = vtx_le32(0);
    vag->root.hdr.xid = vtx_le64(xid);
    vag->root.hdr.total_vags = vtx_le64(total_vags);

    for (i = 0; i < total_vags; i++) {
        __u8 *bm = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);
        struct vtx_vag_desc *d = &vag->descs[i];
        __u32 free_blks = VTXFS_VAG_BLOCKS;
        if (!bm) {
            vtx_vag_destroy(vag);
            return -ENOMEM;
        }
        vag->bitmaps[i] = bm;
        /* VBLK 0 (VAG 0, ag_off 0) is permanently reserved by ABI. */
        if (i == 0) {
            bm[0] |= 1U;
            free_blks--;
        }
        d->dev_id = vtx_le16(0);
        d->flags = vtx_le16(VTX_VAG_FLAG_DATA | VTX_VAG_FLAG_META |
                            VTX_VAG_FLAG_IN_USE |
                            (i == 0 ? VTX_VAG_FLAG_SYSTEM : 0U));
        d->start_pblk = vtx_le64(VTXFS_FIRST_VAG_PBLK +
                                 (__u64)i * VTXFS_VAG_BLOCKS);
        d->bitmap_pblk = vtx_le64(slot_base + 17ULL + i);
        d->allocation_generation = vtx_le32(1);
        d->free_blocks = vtx_le32(free_blks);
        d->bitmap_crc32c = vtx_le32(vtx_crc32c(bm, VTXFS_BLOCK_SIZE));
    }

    vag->root.entries[0].start_pblk = vtx_le64(slot_base + 1ULL);
    vag->root.entries[0].dev_id = vtx_le16(0);
    vag->root.entries[0].valid_count = vtx_le16((__u16)total_vags);
    vag->root.entries[0].leaf_crc32c =
        vtx_le32(vtx_crc32c(vag->descs, sizeof(vag->descs)));
    vtx_vag_root_checksum_compute(&vag->root, metadata_uuid);
    vtx_vag_summary_rebuild(vag);
    return 0;
}

int vtx_vag_vblk_to_pblk(const struct vtx_vag_state *vag, __u64 vblk,
                         __u64 *pblk_out)
{
    __u32 vag_id, ag_off;
    __u64 start;

    if (!vag || !pblk_out || vtx_vblk_decode(vblk, &vag_id, &ag_off) != 0 ||
        vag_id >= vag->total_vags)
        return -ERANGE;
    start = vtx_cpu64(vag->descs[vag_id].start_pblk);
    if (start < VTXFS_FIRST_VAG_PBLK)
        return -EUCLEAN;
    *pblk_out = start + ag_off;
    return 0;
}

int vtx_vag_test_bit(const struct vtx_vag_state *vag, __u64 vblk)
{
    __u32 vag_id, ag_off;
    const __u8 *bm;

    if (!vag || vtx_vblk_decode(vblk, &vag_id, &ag_off) != 0 ||
        vag_id >= vag->total_vags)
        return -ERANGE;
    bm = vag->bitmaps[vag_id];
    if (!bm)
        return -EINVAL;
    return (bm[ag_off >> 3] >> (ag_off & 7U)) & 1;
}

/* -------------------------------------------------------------------------
 * Word-wise bitmap primitives.
 *
 * On-disk bit order is unchanged: block N is bit (N & 7) of byte (N >> 3).
 * Read as a little-endian 64-bit word this becomes "block N = bit N & 63 of
 * word N >> 6", which lets ctz/clz/popcount replace 64 single-bit steps.
 * memcpy keeps the access alignment-safe; vtx_cpu64/vtx_le64 keep it correct
 * on big-endian hosts.
 * ------------------------------------------------------------------------- */

static inline __u64 bm_load(const __u8 *bm, __u32 w)
{
    __le64 raw;

    memcpy(&raw, bm + (size_t)w * 8U, sizeof(raw));
    return vtx_cpu64(raw);
}

static inline void bm_store(__u8 *bm, __u32 w, __u64 v)
{
    __le64 raw = vtx_le64(v);

    memcpy(bm + (size_t)w * 8U, &raw, sizeof(raw));
}

/* Lowest bit in [from, limit) whose value is want_set; returns limit if none. */
static __u32 bm_next(const __u8 *bm, __u32 from, __u32 limit, int want_set)
{
    __u32 off = from;

    while (off < limit) {
        __u32 w = off >> 6;
        __u64 v = bm_load(bm, w);

        if (!want_set)
            v = ~v;
        v &= ~0ULL << (off & 63U);
        if (v) {
            __u32 r = (w << 6) + (__u32)__builtin_ctzll(v);

            return r < limit ? r : limit;
        }
        off = (w + 1U) << 6;
    }
    return limit;
}

/* Highest SET bit strictly below 'from'; 0xFFFFFFFF if there is none. */
static __u32 bm_prev_set(const __u8 *bm, __u32 from)
{
    __u32 off = from;

    while (off > 0) {
        __u32 w = (off - 1U) >> 6;
        __u32 top = (off - 1U) & 63U;
        __u64 v = bm_load(bm, w);

        v &= (top == 63U) ? ~0ULL : ((1ULL << (top + 1U)) - 1ULL);
        if (v)
            return (w << 6) + 63U - (__u32)__builtin_clzll(v);
        off = w << 6;
    }
    return 0xFFFFFFFFU;
}

/*
 * First window of 'count' free bits inside [from, limit); the window must lie
 * entirely before 'limit'. Returns 0 and the window start, or -ENOSPC.
 */
static int bm_find_run(const __u8 *bm, __u32 from, __u32 limit, __u32 count,
                       __u32 *first)
{
    __u32 off = from;

    while (off < limit) {
        __u32 z = bm_next(bm, off, limit, 0);
        __u32 o;

        if (z >= limit)
            break;
        /* Only 'count' bits matter: stop looking for the end of the run
         * once the window is satisfied (keeps small allocations O(1)). */
        o = bm_next(bm, z, (limit - z > count) ? z + count : limit, 1);
        if (o - z >= count) {
            *first = z;
            return 0;
        }
        off = o;
    }
    return -ENOSPC;
}

/*
 * Set (set=1) or clear (set=0) bits [start, start + count). Returns how many
 * bits actually changed; *first_changed receives the lowest changed bit.
 */
static __u32 bm_range_apply(__u8 *bm, __u32 start, __u32 count, int set,
                            __u32 *first_changed)
{
    __u32 end = start + count;
    __u32 off = start;
    __u32 changed = 0;
    int have_first = 0;

    while (off < end) {
        __u32 w = off >> 6;
        __u32 lo = off & 63U;
        __u32 n = 64U - lo;
        __u64 mask, v, diff;

        if (n > end - off)
            n = end - off;
        mask = (n == 64U) ? ~0ULL : (((1ULL << n) - 1ULL) << lo);
        v = bm_load(bm, w);
        diff = set ? (~v & mask) : (v & mask);
        if (diff) {
            if (!have_first) {
                *first_changed = (w << 6) + (__u32)__builtin_ctzll(diff);
                have_first = 1;
            }
            changed += vtx_popcount64(diff);
            bm_store(bm, w, set ? (v | mask) : (v & ~mask));
        }
        off += n;
    }
    return changed;
}

/* Exact number of free runs and the longest one inside a VAG bitmap. */
static __u32 bm_free_stats(const __u8 *bm, __u32 *runs_out)
{
    __u32 off = 0, best = 0, runs = 0;

    while (off < VTXFS_VAG_BLOCKS) {
        __u32 z = bm_next(bm, off, VTXFS_VAG_BLOCKS, 0);
        __u32 o;

        if (z >= VTXFS_VAG_BLOCKS)
            break;
        o = bm_next(bm, z, VTXFS_VAG_BLOCKS, 1);
        runs++;
        if (o - z > best)
            best = o - z;
        off = o;
    }
    if (runs_out)
        *runs_out = runs;
    return best;
}

void vtx_vag_summary_rebuild(struct vtx_vag_state *vag)
{
    __u32 v;

    if (!vag)
        return;
    for (v = 0; v < VTX_MAX_SUPPORTED_VAGS; v++)
        vag->max_free_run[v] = 0;
    for (v = 0; v < vag->total_vags; v++) {
        if (vag->bitmaps[v])
            vag->max_free_run[v] = bm_free_stats(vag->bitmaps[v], NULL);
    }
    vag->summary_ready = 1;
}

int vtx_vag_free_stats(const struct vtx_vag_state *vag, __u32 vag_id,
                       __u32 *free_runs, __u32 *longest_run)
{
    __u32 runs = 0, best;

    if (!vag || vag_id >= vag->total_vags || !vag->bitmaps[vag_id])
        return -EINVAL;
    best = bm_free_stats(vag->bitmaps[vag_id], &runs);
    if (free_runs)
        *free_runs = runs;
    if (longest_run)
        *longest_run = best;
    return 0;
}

int vtx_vag_mark_range(struct vtx_vag_state *vag, __u64 start_vblk,
                       __u32 count, int allocated)
{
    __u32 vag_id, ag_off, free_blks, changed, first = 0;
    __u8 *bm;

    if (!vag || !count ||
        vtx_vblk_decode(start_vblk, &vag_id, &ag_off) != 0 ||
        vag_id >= vag->total_vags ||
        ag_off + count > VTXFS_VAG_BLOCKS)
        return -EINVAL;

    bm = vag->bitmaps[vag_id];
    free_blks = vtx_cpu32(vag->descs[vag_id].free_blocks);
    changed = bm_range_apply(bm, ag_off, count, allocated ? 1 : 0, &first);

    if (allocated) {
        /* Allocation can only shrink runs: the upper bound stays valid. */
        free_blks = (changed > free_blks) ? 0U : free_blks - changed;
    } else if (changed) {
        free_blks += changed;
        if (first < vag->alloc_cursor[vag_id])
            vag->alloc_cursor[vag_id] = first;
        if (vag->summary_ready) {
            /* Freed bits may merge with neighbours: raise the bound to the
             * exact length of the merged run around the freed range. */
            __u32 left = bm_prev_set(bm, ag_off);
            __u32 lstart = (left == 0xFFFFFFFFU) ? 0U : left + 1U;
            __u32 rend = bm_next(bm, ag_off + count, VTXFS_VAG_BLOCKS, 1);

            if (rend - lstart > vag->max_free_run[vag_id])
                vag->max_free_run[vag_id] = rend - lstart;
        }
    }
    vag->descs[vag_id].free_blocks = vtx_le32(free_blks);
    return 0;
}

int vtx_vag_alloc_range(struct vtx_vag_state *vag, __u32 count,
                        __u64 *start_vblk_out)
{
    __u32 v;

    if (!vag || !count || count > VTXFS_VAG_BLOCKS || !start_vblk_out)
        return -EINVAL;
    if (!vag->summary_ready)
        vtx_vag_summary_rebuild(vag);

    for (v = 0; v < vag->total_vags; v++) {
        const __u8 *bm = vag->bitmaps[v];
        __u32 start_off = (v == 0) ? 1U : 0U;
        __u32 cur_cursor = vag->alloc_cursor[v];
        __u32 pass, first;

        if (vtx_cpu32(vag->descs[v].free_blocks) < count)
            continue;
        /* O(1) skip of VAGs that are free-rich but too fragmented. */
        if (vag->max_free_run[v] < count)
            continue;
        if (cur_cursor < start_off || cur_cursor >= VTXFS_VAG_BLOCKS)
            cur_cursor = start_off;

        for (pass = 0; pass < 2; pass++) {
            __u32 search_start, search_limit;

            if (pass == 0) {
                search_start = cur_cursor;
                search_limit = VTXFS_VAG_BLOCKS;
            } else {
                if (cur_cursor <= start_off)
                    break;
                search_start = start_off;
                search_limit = cur_cursor + count - 1U;
                if (search_limit > VTXFS_VAG_BLOCKS)
                    search_limit = VTXFS_VAG_BLOCKS;
            }
            if (bm_find_run(bm, search_start, search_limit, count,
                            &first) == 0) {
                __u64 vblk = VTXFS_MAKE_VBLK(v, first);
                int ret = vtx_vag_mark_range(vag, vblk, count, 1);

                if (ret)
                    return ret;
                vag->alloc_cursor[v] = first + count;
                *start_vblk_out = vblk;
                return 0;
            }
        }
        /* Both passes together cover every window, so this VAG truly cannot
         * hold 'count' blocks: the bound was loose. Tighten it so the next
         * request skips this VAG in O(1). */
        vag->max_free_run[v] = bm_free_stats(bm, NULL);
    }
    return -ENOSPC;
}

static int write_vag_copy(struct vtx_bdev *bdev, struct vtx_vag_state *vag,
                          __u64 base_pblk, __u64 xid,
                          const __u8 metadata_uuid[VTXFS_UUID_SIZE],
                          const __u8 *packed_bitmaps)
{
    __u32 i;
    int ret;

    vag->root.hdr.xid = vtx_le64(xid);
    vag->root.hdr.total_vags = vtx_le64(vag->total_vags);
    for (i = 0; i < vag->total_vags; i++) {
        vag->descs[i].bitmap_pblk = vtx_le64(base_pblk + 17ULL + i);
    }

    if (packed_bitmaps && vag->total_vags > 0) {
        ret = bdev->ops->write_blocks(bdev, base_pblk + 17ULL,
                                      vag->total_vags, packed_bitmaps);
        if (ret)
            return ret;
    }

    vag->root.entries[0].start_pblk = vtx_le64(base_pblk + 1ULL);
    vag->root.entries[0].dev_id = vtx_le16(0);
    vag->root.entries[0].valid_count = vtx_le16((__u16)vag->total_vags);
    vag->root.entries[0].leaf_crc32c =
        vtx_le32(vtx_crc32c(vag->descs, sizeof(vag->descs)));
    vtx_vag_root_checksum_compute(&vag->root, metadata_uuid);

    ret = bdev->ops->write_blocks(bdev, base_pblk + 1ULL, 16, vag->descs);
    if (ret)
        return ret;
    return bdev->ops->write_blocks(bdev, base_pblk, 1, &vag->root);
}

int vtx_vag_write_slot(struct vtx_bdev *bdev, struct vtx_vag_state *vag,
                       __u64 xid, const __u8 metadata_uuid[VTXFS_UUID_SIZE],
                       struct vtx_phys_ref *ref_out)
{
    __u64 slot = VTXFS_CPD_SLOT_FOR_XID(xid);
    __u64 prim_base = VTXFS_IP_BOOT_PBLK + slot * VTX_IP_SLOT_STRIDE;
    __u64 mirr_base = VTXFS_BOOT_MIRROR0_PBLK + prim_base;
    __u8 *packed_bitmaps = NULL;
    __u32 i;
    int ret;

    if (!bdev || !vag || !metadata_uuid || !ref_out)
        return -EINVAL;
    if (bdev->fault_stage == VTX_FAULT_VAG_WRITE) {
        bdev->fault_triggered = 1;
        return -EIO;
    }

    /* Compute CRC once and pack all bitmaps into a single contiguous buffer */
    if (vag->total_vags > 0) {
        packed_bitmaps = (__u8 *)vtx_zalloc((size_t)vag->total_vags * VTXFS_BLOCK_SIZE);
        if (!packed_bitmaps)
            return -ENOMEM;
        for (i = 0; i < vag->total_vags; i++) {
            vag->descs[i].bitmap_crc32c =
                vtx_le32(vtx_crc32c(vag->bitmaps[i], VTXFS_BLOCK_SIZE));
            memcpy(packed_bitmaps + (size_t)i * VTXFS_BLOCK_SIZE,
                   vag->bitmaps[i], VTXFS_BLOCK_SIZE);
        }
    }

    /* Write mirror copy first, then primary copy so vag->root reflects primary. */
    ret = write_vag_copy(bdev, vag, mirr_base, xid, metadata_uuid, packed_bitmaps);
    if (!ret)
        ret = write_vag_copy(bdev, vag, prim_base, xid, metadata_uuid, packed_bitmaps);

    if (packed_bitmaps)
        vtx_free(packed_bitmaps);
    if (ret)
        return ret;

    memset(ref_out, 0, sizeof(*ref_out));
    ref_out->pblk = vtx_le64(prim_base);
    ref_out->xid = vtx_le64(xid);
    ref_out->dev_id = vtx_le16(0);
    ref_out->blocks = vtx_le16(1);
    return 0;
}

static int try_load_vag_copy(struct vtx_bdev *bdev, struct vtx_vag_state *vag,
                             __u64 root_pblk, __u64 expected_xid,
                             __u64 total_blocks,
                             const __u8 metadata_uuid[VTXFS_UUID_SIZE])
{
    __u64 total_vags, leaf_pblk;
    __u32 i;
    int ret;

    vtx_vag_destroy(vag);
    ret = bdev->ops->read_blocks(bdev, root_pblk, 1, &vag->root);
    if (ret)
        return ret;
    if (vtx_cpu32(vag->root.hdr.magic) != VTXFS_VAG_DIR_MAGIC ||
        vtx_cpu64(vag->root.hdr.xid) != expected_xid ||
        !vtx_vag_root_checksum_verify(&vag->root, metadata_uuid))
        return -EUCLEAN;

    total_vags = vtx_cpu64(vag->root.hdr.total_vags);
    if (total_vags == 0 || total_vags > VTX_MAX_SUPPORTED_VAGS ||
        vtx_cpu16(vag->root.entries[0].valid_count) != total_vags ||
        vtx_cpu16(vag->root.entries[0].dev_id) != 0)
        return -EUCLEAN;

    leaf_pblk = vtx_cpu64(vag->root.entries[0].start_pblk);
    if (leaf_pblk >= total_blocks || 16ULL > total_blocks - leaf_pblk)
        return -EUCLEAN;
    ret = bdev->ops->read_blocks(bdev, leaf_pblk, 16, vag->descs);
    if (ret)
        return ret;
    if (vtx_crc32c(vag->descs, sizeof(vag->descs)) !=
        vtx_cpu32(vag->root.entries[0].leaf_crc32c))
        return -EUCLEAN;

    vag->total_vags = (__u32)total_vags;
    __u64 first_bm = (vag->total_vags > 0) ? vtx_cpu64(vag->descs[0].bitmap_pblk) : 0;
    int is_contiguous = 1;

    for (i = 0; i < vag->total_vags; i++) {
        const struct vtx_vag_desc *d = &vag->descs[i];
        __u64 start_pblk = vtx_cpu64(d->start_pblk);
        __u64 bm_pblk = vtx_cpu64(d->bitmap_pblk);

        if (start_pblk != VTXFS_FIRST_VAG_PBLK + (__u64)i * VTXFS_VAG_BLOCKS ||
            start_pblk + VTXFS_VAG_BLOCKS > total_blocks ||
            bm_pblk >= total_blocks ||
            vtx_cpu16(d->dev_id) != 0) {
            vtx_vag_destroy(vag);
            return -EUCLEAN;
        }
        if (bm_pblk != first_bm + (__u64)i)
            is_contiguous = 0;
    }

    __u8 *bulk = NULL;
    if (is_contiguous && vag->total_vags > 1) {
        bulk = (__u8 *)vtx_zalloc((size_t)vag->total_vags * VTXFS_BLOCK_SIZE);
        if (bulk) {
            ret = bdev->ops->read_blocks(bdev, first_bm, vag->total_vags, bulk);
            if (ret) {
                vtx_free(bulk);
                bulk = NULL;
            }
        }
    }

    for (i = 0; i < vag->total_vags; i++) {
        const struct vtx_vag_desc *d = &vag->descs[i];
        __u64 bm_pblk = vtx_cpu64(d->bitmap_pblk);
        __u8 *bm = (__u8 *)vtx_zalloc(VTXFS_BLOCK_SIZE);

        if (!bm) {
            if (bulk)
                vtx_free(bulk);
            vtx_vag_destroy(vag);
            return -ENOMEM;
        }
        vag->bitmaps[i] = bm;
        vag->alloc_cursor[i] = (i == 0) ? 1U : 0U;

        if (bulk) {
            memcpy(bm, bulk + (size_t)i * VTXFS_BLOCK_SIZE, VTXFS_BLOCK_SIZE);
        } else {
            ret = bdev->ops->read_blocks(bdev, bm_pblk, 1, bm);
            if (ret) {
                vtx_vag_destroy(vag);
                return ret;
            }
        }
        if (vtx_crc32c(bm, VTXFS_BLOCK_SIZE) != vtx_cpu32(d->bitmap_crc32c)) {
            if (bulk)
                vtx_free(bulk);
            vtx_vag_destroy(vag);
            return -EUCLEAN;
        }
        if (i == 0 && (bm[0] & 1U) == 0) {
            if (bulk)
                vtx_free(bulk);
            vtx_vag_destroy(vag);
            return -EUCLEAN;
        }
    }
    if (bulk)
        vtx_free(bulk);

    vtx_vag_summary_rebuild(vag);
    return 0;
}

int vtx_vag_load_from_ref(struct vtx_bdev *bdev, struct vtx_vag_state *vag,
                          const struct vtx_phys_ref *ref,
                          __u64 total_blocks,
                          const __u8 metadata_uuid[VTXFS_UUID_SIZE])
{
    __u64 prim_pblk, exp_xid;
    int ret;

    if (!bdev || !vag || !ref || !metadata_uuid ||
        !vtx_phys_ref_valid(ref, total_blocks, 0))
        return -EUCLEAN;
    prim_pblk = vtx_cpu64(ref->pblk);
    exp_xid = vtx_cpu64(ref->xid);
    if (!exp_xid)
        return -EUCLEAN;

    ret = try_load_vag_copy(bdev, vag, prim_pblk, exp_xid,
                            total_blocks, metadata_uuid);
    if (ret == 0)
        return 0;
    if (prim_pblk < VTXFS_BOOT_MIRROR0_PBLK &&
        prim_pblk + VTXFS_BOOT_MIRROR0_PBLK < total_blocks) {
        return try_load_vag_copy(bdev, vag,
                                 prim_pblk + VTXFS_BOOT_MIRROR0_PBLK,
                                 exp_xid, total_blocks, metadata_uuid);
    }
    return ret;
}
