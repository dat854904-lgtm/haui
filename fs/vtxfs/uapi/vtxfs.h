/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * VTXFS v2.1 DRAFT Linux UAPI (ioctls, feature flags and capability query).
 *
 * No ioctl is implemented by this header. Only VTXFS_IOC_GET_ABI is intended
 * for the first driver; all other commands are reserved for later phases.
 * Do not use this as evidence that VTXFS is ready for production data.
 *
 * v2.1 is ON-DISK INCOMPATIBLE with the original vtxfs_raw.c v2.0 design.
 * On-disk types belong to ../include/vtxfs_disk.h (not this UAPI header).
 */
#ifndef _UAPI_VTXFS_H
#define _UAPI_VTXFS_H
#include <linux/types.h>
#include <linux/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VTXFS_VERSION_MAJOR                 2U
#define VTXFS_VERSION_MINOR                 1U
#define VTXFS_UAPI_VERSION                  1U
#define VTXFS_SUBVOL_NAME_MAX               255U
#define VTXFS_MAX_DEVICES                   255U

/* Incompat bits: unknown incompat features always block mounting. */
#define VTXFS_FEAT_INCOMPAT_V21_LAYOUT      (1ULL << 0)
#define VTXFS_FEAT_INCOMPAT_OMAP_VIEWS      (1ULL << 1)
#define VTXFS_FEAT_INCOMPAT_EXTENT_CSUM     (1ULL << 2)
#define VTXFS_FEAT_INCOMPAT_MULTIDEV        (1ULL << 3)  /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_FAST_WAL        (1ULL << 4)  /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_AEAD            (1ULL << 5)  /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_DUP_META        (1ULL << 6)  /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_COMPRESS        (1ULL << 7)  /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_HINT_FASTPATH   (1ULL << 8)  /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_SEND_RECEIVE    (1ULL << 9)  /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_FS_VERITY       (1ULL << 10) /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_TIERING         (1ULL << 11) /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_ENCODED_IO      (1ULL << 12) /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_DIR_STATS       (1ULL << 13) /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_SIMPLE_QUOTA    (1ULL << 14) /* DEFERRED */
#define VTXFS_FEAT_INCOMPAT_CHAIN_ATTEST    (1ULL << 15) /* DEFERRED */
/* Unknown COMPAT ignored, unknown RO_COMPAT blocks R/W, unknown INCOMPAT refuses mount. */
#define VTXFS_FEAT_COMPAT_UTF8_LABEL             (1ULL << 0)
#define VTXFS_FEAT_RO_COMPAT_FREE_SPACE_CACHE    (1ULL << 0)
#define VTXFS_FEAT_RO_COMPAT_INODE_V2            (1ULL << 1) /* DEFERRED */
#define VTXFS_P0_ALLOWED_COMPAT                  VTXFS_FEAT_COMPAT_UTF8_LABEL
#define VTXFS_P0_ALLOWED_RO_COMPAT               VTXFS_FEAT_RO_COMPAT_FREE_SPACE_CACHE
#define VTXFS_P0_REQUIRED_INCOMPAT          (VTXFS_FEAT_INCOMPAT_V21_LAYOUT | VTXFS_FEAT_INCOMPAT_OMAP_VIEWS | VTXFS_FEAT_INCOMPAT_EXTENT_CSUM)
#define VTXFS_P0_ALLOWED_INCOMPAT           (VTXFS_P0_REQUIRED_INCOMPAT | VTXFS_FEAT_INCOMPAT_COMPRESS | VTXFS_FEAT_INCOMPAT_MULTIDEV | VTXFS_FEAT_INCOMPAT_SEND_RECEIVE)

/* Feature query flags; these are not mount flags. */
#define VTXFS_CAP_P0_RO_PARSER               (1ULL << 0)
#define VTXFS_CAP_P0_SINGLEDEV_RW            (1ULL << 1)
#define VTXFS_CAP_SNAPSHOTS                  (1ULL << 2)
#define VTXFS_CAP_SEND_RECEIVE               (1ULL << 3)
#define VTXFS_CAP_MULTI_DEVICE               (1ULL << 4)
#define VTXFS_CAP_SCRUB                      (1ULL << 5)
#define VTXFS_CAP_ENCODED_IO                 (1ULL << 6)
#define VTXFS_CAP_ENCRYPTION                 (1ULL << 7)

/* No implicit pointers, flexible arrays, longs, size_t or time_t in ioctls.
 * struct_size MUST equal known fixed size in UAPI v1 (not silently ignored).
 * Unknown flags and nonzero reserved fields MUST be rejected.
 */
struct vtx_ioctl_abi {
    __u16 major;
    __u16 minor;
    __u32 struct_size;
    __u64 supported_incompat;
    __u64 active_incompat;
    __u32 uapi_version; /* output */
    __u32 reserved;     /* must be zero; still 32 bytes */
};

struct vtx_ioc_capabilities {
    __u32 struct_size;
    __u32 flags;
    __u64 capabilities;
    __u64 supported_incompat;
    __u64 active_incompat;
    __u64 reserved[3];
};

/* RESERVED / DEFERRED: subvolume creation, snapshots, removal. */
#define VTX_SUBVOL_FLAG_RDONLY          (1ULL << 0)
#define VTX_SUBVOL_FLAG_ENCRYPTED       (1ULL << 1) /* requires AEAD */
#define VTX_SUBVOL_FLAG_QUOTA_ENABLED   (1ULL << 2) /* requires quotas */
struct vtx_ioc_subvol_args {
    __u32 struct_size;
    __u32 flags;
    __u64 src_subvol_id;
    __u64 new_subvol_id;   /* output */
    __u64 parent_subvol_id;
    __u64 quota_max_rfer;
    __u64 quota_max_excl;
    char name[VTXFS_SUBVOL_NAME_MAX + 1];
    __u64 reserved[2];
};

/* RESERVED / DEFERRED: hot/cold tier and multi-device management.
 * Path input is UTF-8 and NUL-terminated. Caller MUST have privileges.
 */
enum vtx_storage_tier {
    VTX_TIER_NVME_HOT = 0,
    VTX_TIER_SSD_WARM = 1,
    VTX_TIER_HDD_COLD = 2
};
#define VTX_DEV_STATE_ONLINE       (1U << 0)
#define VTX_DEV_STATE_EVACUATING  (1U << 1)
#define VTX_DEV_STATE_REPLACING   (1U << 2)
#define VTX_DEV_STATE_READONLY    (1U << 3)
#define VTX_DEV_STATE_MISSING     (1U << 4)
struct vtx_ioc_dev_args {
    __u32 struct_size;
    __u32 flags;
    __u64 dev_id;
    __u64 target_dev_id;
    __u32 tier_id;
    __u32 state_flags;
    __u64 total_blocks;
    __u64 used_blocks;
    __u64 progress_permille;
    char dev_path[256];
    char target_dev_path[256];
    __u64 reserved[4];
};

/* RESERVED / DEFERRED: view-scoped diagnostics, never user-selected raw I/O.
 * Restrict to privileged callers; no address disclosure without permission.
 */
struct vtx_ioc_omap_lookup {
    __u32 struct_size;
    __u32 flags;
    __u64 subvol_id;
    __u64 oid;
    __u64 hint_vblk;
    __u64 hint_xid;
    __u64 resolved_vblk;
    __u64 resolved_xid;
    __u64 resolved_pblk;
    __u32 resolved_dev_id;
    __u32 hint_valid;
    __u64 reserved[2];
};

/* RESERVED / DEFERRED: scrub / verify / optional repair. */
#define VTX_SCRUB_VERIFY_ONLY       (1U << 0)
#define VTX_SCRUB_REPAIR_META       (1U << 1) /* requires DUP_META */
struct vtx_ioc_scrub_args {
    __u32 struct_size;
    __u32 flags;
    __u64 start_vag;
    __u64 end_vag;
    __u64 data_blocks_scrubbed;
    __u64 meta_blocks_scrubbed;
    __u64 checksum_errors;
    __u64 repaired_meta_blocks;
    __u64 uncorrectable_errors;
    __u64 reserved[2];
};

/* RESERVED / DEFERRED: encoded I/O, with stable-width aligned user pointer. */
struct vtx_ioc_encoded_io {
    __u32 struct_size;
    __u32 flags;
    __u64 file_offset;
    __u64 logical_len;
    __u64 encoded_len;
    __aligned_u64 user_buf_ptr;
    __u32 compression;
    __u32 encryption;
    __u64 reserved[2];
};

/* RESERVED / DEFERRED: send/receive stream protocol NOT specified yet.
 * A future feature gate is mandatory before ioctl implementations appear.
 */
struct vtx_ioc_send_args {
    __u32 struct_size;
    __u32 flags;
    __u64 subvol_id;
    __u64 parent_subvol_id;
    __s32 stream_fd;
    __u32 stream_version;
    __u64 reserved[4];
};

/* Candidate ioctl magic; upstream allocation/registration still required. */
#define VTXFS_IOCTL_MAGIC 'V'
struct vtx_ioc_sync {
    __u32 struct_size;
    __u32 flags;
    __u64 xid;
    __u64 reserved[2];
};
#define VTXFS_IOC_GET_ABI          _IOWR(VTXFS_IOCTL_MAGIC, 0x01, struct vtx_ioctl_abi)
#define VTXFS_IOC_GET_CAPABILITIES _IOWR(VTXFS_IOCTL_MAGIC, 0x02, struct vtx_ioc_capabilities)
#define VTX_IOC_START_SYNC         _IOWR(VTXFS_IOCTL_MAGIC, 0x03, struct vtx_ioc_sync)
#define VTX_IOC_WAIT_SYNC          _IOWR(VTXFS_IOCTL_MAGIC, 0x04, struct vtx_ioc_sync)

/* All commands below RESERVED: no semantic/runtime implementation exists.
 * Number space intentionally differs from vtxfs_raw.c v2.0.
 */
#define VTX_IOC_SUBVOL_CREATE     _IOWR(VTXFS_IOCTL_MAGIC, 0x10, struct vtx_ioc_subvol_args)
#define VTX_IOC_SNAP_CREATE       _IOWR(VTXFS_IOCTL_MAGIC, 0x11, struct vtx_ioc_subvol_args)
#define VTX_IOC_SUBVOL_DESTROY    _IOW(VTXFS_IOCTL_MAGIC, 0x12, struct vtx_ioc_subvol_args)
#define VTX_IOC_DEV_ADD           _IOWR(VTXFS_IOCTL_MAGIC, 0x20, struct vtx_ioc_dev_args)
#define VTX_IOC_DEV_REMOVE        _IOWR(VTXFS_IOCTL_MAGIC, 0x21, struct vtx_ioc_dev_args)
#define VTX_IOC_DEV_REPLACE       _IOWR(VTXFS_IOCTL_MAGIC, 0x22, struct vtx_ioc_dev_args)
#define VTX_IOC_DEV_INFO          _IOWR(VTXFS_IOCTL_MAGIC, 0x23, struct vtx_ioc_dev_args)
#define VTX_IOC_OMAP_LOOKUP       _IOWR(VTXFS_IOCTL_MAGIC, 0x30, struct vtx_ioc_omap_lookup)
#define VTX_IOC_SCRUB             _IOWR(VTXFS_IOCTL_MAGIC, 0x40, struct vtx_ioc_scrub_args)
#define VTX_IOC_ENCODED_READ      _IOWR(VTXFS_IOCTL_MAGIC, 0x50, struct vtx_ioc_encoded_io)
#define VTX_IOC_ENCODED_WRITE     _IOWR(VTXFS_IOCTL_MAGIC, 0x51, struct vtx_ioc_encoded_io)
#define VTX_IOC_SEND             _IOWR(VTXFS_IOCTL_MAGIC, 0x60, struct vtx_ioc_send_args)
#define VTX_IOC_RECEIVE          _IOWR(VTXFS_IOCTL_MAGIC, 0x61, struct vtx_ioc_send_args)

#ifdef __cplusplus
}
#endif
#endif /* _UAPI_VTXFS_H */
