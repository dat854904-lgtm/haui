/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* VTXFS 2.1 DRAFT on-disk ABI (NOT a functional filesystem).
 * NEVER reinterpret VTXFS v2.0 media as v2.1.
 * Single-device CoW checkpoint is the initial implementation profile.
 * See ../uapi/vtxfs.h for ioctl types, feature-gate definitions and version.
 */
#ifndef _VTXFS_DISK_H
#define _VTXFS_DISK_H
#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif
#include "../uapi/vtxfs.h"
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__cplusplus)
#define VTXFS_STATIC_ASSERT(e, msg) static_assert((e), msg)
#else
#define VTXFS_STATIC_ASSERT(e, msg) _Static_assert((e), msg)
#endif

/* 1. Versioning, profile and fundamental geometry ---------------------- */

/* Eight bytes, including the terminating NUL; compare all eight bytes. */
#define VTXFS_MAGIC_BYTES             "VTXFS21"
#define VTXFS_CPD_MAGIC               "VTXCP21"
#define VTXFS_MAGIC_LEN               8U
#define VTXFS_DEV_TABLE_MAGIC         0x31544456U
#define VTXFS_NODE_MAGIC              0x314E5856U  /* "VXN1" LE */
#define VTXFS_VAG_DIR_MAGIC           0x31444756U  /* "VGD1" LE */

#define VTXFS_BLOCK_SHIFT             12U
#define VTXFS_BLOCK_SIZE              4096U
#define VTXFS_MAX_NODE_SIZE           65536U
#define VTXFS_MIN_NODE_SIZE           4096U
#define VTXFS_NODE_OFFSET_UNIT        8U
#define VTXFS_UUID_SIZE               16U
#define VTXFS_NAME_MAX                255U
#define VTXFS_LABEL_SIZE              64U
#define VTXFS_DEV_ID_NONE             0xFFFFU

/* 128 MiB/VAG, 32768 logical 4 KiB blocks per VAG. */
#define VTXFS_VAG_BLOCK_SHIFT         15U
#define VTXFS_VAG_BLOCKS              (1U << VTXFS_VAG_BLOCK_SHIFT)
#define VTXFS_VAG_BLOCK_MASK          (VTXFS_VAG_BLOCKS - 1U)
#define VTXFS_VAG_BYTES               (128ULL * 1024ULL * 1024ULL)

/*
 * The v2.1 draft retains a two-level physical VAG directory:
 *   252 root entries * 2048 descriptors/leaf * 128 MiB = 63 TiB.
 * This is the IMPLEMENTED ADDRESS WINDOW for the proposed 2-level layout,
 * not the theoretical size of a wider virtual-block integer.
 */
#define VTXFS_VAG_ROOT_ENTRIES        252U
#define VTXFS_VAG_LEAF_ENTRIES        2048U
#define VTXFS_MAX_VAGS                (VTXFS_VAG_ROOT_ENTRIES * VTXFS_VAG_LEAF_ENTRIES)
#define VTXFS_MAX_VBLKS               ((unsigned long long)VTXFS_MAX_VAGS * VTXFS_VAG_BLOCKS)
#define VTXFS_VBLK_TO_VAG_ID(vblk)    ((__u32)(((__u64)(vblk)) >> VTXFS_VAG_BLOCK_SHIFT))
#define VTXFS_VBLK_TO_AG_OFF(vblk)    ((__u32)((__u64)(vblk) & VTXFS_VAG_BLOCK_MASK))
#define VTXFS_MAKE_VBLK(vag, off)     ((((__u64)(vag)) << VTXFS_VAG_BLOCK_SHIFT) | ((__u64)(off) & VTXFS_VAG_BLOCK_MASK))
#define VTXFS_VALID_VBLK(vblk)        ((__u64)(vblk) != 0ULL && (__u64)(vblk) < VTXFS_MAX_VBLKS)

/* Fixed physical bootstrap region, per member. Single device in P0. */
#define VTXFS_BOOT_RESERVED_PBLK      0ULL   /* blocks 0..3 */
#define VTXFS_DEV_TABLE_PBLK          4ULL   /* blocks 4..7 */
#define VTXFS_CPD_RING_PBLK           8ULL   /* blocks 8..15 */
#define VTXFS_CPD_RING_SLOTS          8U
#define VTXFS_SUPER_RING_PBLK         16ULL  /* blocks 16..31 */
#define VTXFS_SUPER_RING_SLOTS        16U
#define VTXFS_WAL_RING_PBLK           32ULL /* blocks 32..287: DEFERRED */
#define VTXFS_WAL_RING_BLOCKS         256U
#define VTXFS_IP_BOOT_PBLK            288ULL /* 16 MiB reserve region */
#define VTXFS_IP_BOOT_BLOCKS          4096U
#define VTXFS_FIRST_VAG_PBLK          VTXFS_VAG_BLOCKS
#define VTXFS_BOOT_MIRROR0_PBLK       16384ULL /* 64MiB */
#define VTXFS_CPD_SLOT_FOR_XID(xid)   ((__u32)((xid) % VTXFS_CPD_RING_SLOTS))
#define VTXFS_DEV_TABLE_BLOCKS        4U
#define VTXFS_MAX_INLINE_DATA         2048U /* max; actual payload must fit leaf */
#define VTXFS_MAX_COMPRESSED_FRAME_BYTES 131072U /* 128 KiB, DEFERRED */

/* P0: single-device, full CoW checkpoints, no crypto/WAL fast path.
 * Optional features MUST NOT be set on media until their protocols are
 * defined, implemented and crash-tested.
 */

enum vtx_dir_hash_type { VTX_DIR_HASH_NONE=0, VTX_DIR_HASH_SIPHASH24=1 };
#define VTXFS_STATE_ERROR        (1U << 0)
#define VTXFS_STATE_FSCK_RUNNING (1U << 1)
#define VTXFS_CP_FLAG_CLEAN_UNMOUNT  (1U << 0)
#define VTXFS_CP_FLAG_REAPER_PENDING (1U << 1)
#define VTXFS_CP_FLAG_FSCK_REQUIRED  (1U << 2)
#define VTXFS_CP_FLAGS_P0_KNOWN      0x7U
enum vtx_tree_type {
    VTX_TREE_NONE=0, VTX_TREE_ROOT=1, VTX_TREE_OMAP_META=2,
    VTX_TREE_REF=3, VTX_TREE_REAPER=4, VTX_TREE_SFQ=5,
    VTX_TREE_UUID=6, VTX_TREE_FS=7, VTX_TREE_OMAP_VIEW=8,
    VTX_TREE_MIRROR=9, VTX_TREE_EVAC=10, VTX_TREE_SCRUB=11,
    VTX_TREE_KEYBAG=12, VTX_TREE_CHAIN=13,
    VTX_TREE_FREE_SPACE=14, VTX_TREE_DEV=15, VTX_TREE_MAX=16
};
enum vtx_csum_type {
    VTX_CSUM_NONE = 0,
    VTX_CSUM_CRC32C = 1,      /* recommended P0 data CRC32C, 4 bytes */
    VTX_CSUM_BLAKE3_128 = 2,  /* metadata node, 16 bytes */
    VTX_CSUM_BLAKE3_256 = 3   /* superblock/checkpoint, 32 bytes */
};
enum vtx_compr_type { VTX_COMPR_NONE = 0, VTX_COMPR_ZSTD = 1, VTX_COMPR_LZ4 = 2, VTX_COMPR_ZLIB = 3 };
enum vtx_enc_type {
    VTX_ENC_NONE = 0,
    VTX_ENC_AES_256_XTS = 1,       /* NOT authenticated encryption */
    VTX_ENC_CHACHA20_POLY1305 = 2, /* AEAD, deferred */
    VTX_ENC_AES_256_GCM = 3        /* AEAD, deferred */
};

/* 2. Addressing and bootstrap ------------------------------------------ */

/*
 * Physical roots MUST be directly resolvable before VAG translation works.
 * pblk is counted in filesystem blocks, not device-native sectors.
 * generation prevents accidental reuse of an older physical root.
 */
struct vtx_phys_ref {
    __le64 pblk;
    __le64 xid; /* full transaction generation */
    __le16 dev_id;
    __le16 blocks;
    __le32 reserved;
} __packed;

/*
 * Superblock and checkpoint use 4096-byte records. blake3_256 covers
 * bytes [0x20..0xfff], including any reserved bytes. Reserved bytes MUST
 * be zero on writing, validated on reading according to feature policy.
 *
 * Superblock stores configuration only. Scan all CPD slots (and 64MiB
 * mirror), check fsid/xid/checksum, select highest committed checkpoint.
 * CPD slot = xid % 8; persist data and metadata before CPD FUA.
 * Bootstrap requires physical refs; no circular VAG lookup.
 */
struct vtx_super_header {
    __u8  blake3_256[32];
    char  magic[8];
    __le16 major;
    __le16 minor;
    __le32 block_size;
    __u8 fsid_uuid[16];
    __u8 device_uuid[16];
    __le64 super_generation; /* changes on config update, not commit */
    __le64 compat_flags;
    __le64 ro_compat_flags;
    __le64 incompat_flags;
    __le32 device_count;
    __le32 self_dev_id;
    __le64 total_blocks;
    __le32 node_size;
    __le32 sector_size;
    __le16 meta_csum_type;
    __le16 data_csum_type;
    __le16 dir_hash_type;
    __le16 state;
    __u8 metadata_uuid[16];
    __u8 dir_hash_seed[16];
    __le64 mkfs_time_ns; /* signed 2-complement nanoseconds */
    __le64 first_alloc_pblk;
    char label[VTXFS_LABEL_SIZE];
} __packed;

struct vtx_super_block {
    struct vtx_super_header hdr;
    __u8 default_compression; /* Valid when incompat_flags & VTXFS_FEAT_INCOMPAT_COMPRESS */
    __u8 reserved[VTXFS_BLOCK_SIZE - sizeof(struct vtx_super_header) - 1];
} __packed;

/*
 * CPD selects the superblock by matching full super_generation and fsid.
 * A checkpoint is atomic ONLY if the implementation persists every
 * dependency and validates its commit/durability protocol.
 * New checkpoints MUST be published only after all dependency flushes.
 * The VAG root must be a PHYSICAL pointer, not a VBLK needing itself.
 * Tree roots below are VBLKs resolved after the VAG root is loaded.
 */
struct vtx_checkpoint_header {
    __u8 blake3_256[32];
    char magic[8];
    __le64 xid;
    struct vtx_phys_ref vag_dir_root;
    __le64 root_tree_vblk;
    __le64 ref_tree_vblk;
    __le64 reaper_tree_vblk;
    __le64 sfq_tree_vblk;
    __le64 uuid_tree_vblk;
    __le64 mirror_tree_vblk; /* optional DUP_META mapping (DEFERRED) */
    __le64 evac_tree_vblk;   /* durable evacuation state (DEFERRED) */
    __le64 scrub_tree_vblk;  /* scrub progress (DEFERRED) */
    __le64 keybag_tree_vblk; /* crypto keybag tree (DEFERRED) */
    __le64 chain_tree_vblk;  /* optional signed provenance (DEFERRED) */
    __le64 wal_replay_xid;   /* durable WAL checkpoint boundary (DEFERRED) */
    __le64 next_oid;
    __le64 used_blocks;
    __le32 root_tree_level;
    __le32 cp_flags;
    __u8 fsid_uuid[16];    /* reject stale metadata from previous mkfs */
    __le64 omap_meta_tree_vblk; /* system-tree OID resolution */
    __le64 prev_xid;
    __le64 super_generation; /* must match selected config superblock */
    __le64 commit_time_ns; /* signed 2-complement nanoseconds */
    __le64 reuse_safe_xid; /* NOT sufficient alone for reuse */
    __le64 total_blocks;
    __le64 free_blocks;
    __u8 tree_levels[VTX_TREE_MAX];
} __packed;

struct vtx_checkpoint_desc {
    struct vtx_checkpoint_header hdr;
    __u8 reserved[VTXFS_BLOCK_SIZE - sizeof(struct vtx_checkpoint_header)];
} __packed;

/* VAG root block: fixed 64-byte header followed by 252 * 16 B entries. */
struct vtx_vag_root_header {
    __u8  checksum[16]; /* BLAKE3-128(metadata_uuid || bytes[16..4095)); NOT a MAC */
    __le32 magic;
    __le32 flags;
    __le64 xid;
    __le64 total_vags;
    __u8  reserved[24];
} __packed;

/* Each entry describes one contiguous 64 KiB VAG leaf (16 blocks).
 * Integrity of leaf: CRC32C covers all 65536 bytes; leaf has NO header.
 */
struct vtx_vag_dir_entry {
    __le64 start_pblk;
    __le32 leaf_crc32c;
    __le16 dev_id;
    __le16 valid_count;
} __packed;

struct vtx_vag_dir_root {
    struct vtx_vag_root_header hdr;
    struct vtx_vag_dir_entry entries[VTXFS_VAG_ROOT_ENTRIES];
} __packed;

/* 2048 * 32 = 65536 bytes; NO space for an additional leaf header. */
#define VTX_VAG_FLAG_DATA          (1U << 0)
#define VTX_VAG_FLAG_META          (1U << 1)
#define VTX_VAG_FLAG_SYSTEM        (1U << 2)
#define VTX_VAG_FLAG_MIRRORED      (1U << 3) /* deferred */
#define VTX_VAG_FLAG_IN_USE        (1U << 4)
struct vtx_vag_desc {
    __le32 bitmap_crc32c; /* bitmap cache, rebuildable */
    __le16 dev_id;
    __le16 flags;
    __le64 start_pblk;
    __le64 bitmap_pblk;
    __le32 allocation_generation;
    __le32 free_blocks;
} __packed;

/* 3. CoW B+Tree and object mapping ------------------------------------- */

/* Key on disk is (oid << 8) | type, then numeric offset; little-endian.
 * Compare decoded CPU integers as (oid, type, offset). NEVER memcmp() the
 * little-endian bytes to implement numeric ordering.
 */
#define VTX_KEY_OID_MASK        0x00FFFFFFFFFFFFFFULL
#define VTX_MAKE_OID_TYPE(oid, type)     ((((__u64)(oid) & VTX_KEY_OID_MASK) << 8) | ((__u64)(type) & 255ULL))
#define VTX_KEY_GET_OID(word)   ((__u64)(word) >> 8)
#define VTX_KEY_GET_TYPE(word)  ((__u8)((__u64)(word) & 255ULL))

enum vtx_item_type {
    VTX_TYPE_ANY         = 0,
    VTX_TYPE_INODE       = 1,
    VTX_TYPE_INODE_REF   = 2,
    VTX_TYPE_XATTR       = 3,
    VTX_TYPE_DIR_ITEM    = 4,
    VTX_TYPE_DIR_INDEX   = 5,
    VTX_TYPE_EXTENT      = 6,
    VTX_TYPE_SUBVOL_ROOT = 7,
    VTX_TYPE_OMAP_VAL    = 8,
    VTX_TYPE_REF         = 9,
    VTX_TYPE_SFQ         = 10,
    VTX_TYPE_REAPER_JOB  = 11,
    VTX_TYPE_UUID_MAP    = 12
};

struct vtx_key {
    __le64 oid_and_type;
    __le64 offset;
} __packed;

/* val_off8: 8-byte units from start of node.
 * val_len: actual value length in BYTES, excluding padding.
 * key_prefix is an advisory search hint; never part of identity/order.
 */
struct vtx_slot {
    __le16 val_off8;
    __le16 val_len;
    __le16 key_prefix;
    __le16 flags;
} __packed;

struct vtx_node_entry {
    struct vtx_key key;
    struct vtx_slot slot;
} __packed;

#define VTX_NODE_FLAG_LEAF          (1U << 0)
#define VTX_NODE_FLAG_ROOT          (1U << 1)
#define VTX_NODE_FLAG_VIRTUAL_OID   (1U << 2)
#define VTX_NODE_FLAG_PHYSICAL_VBLK (1U << 3)
struct vtx_node_hdr {
    __u8 checksum[16];   /* BLAKE3-128(metadata_uuid || bytes[16..node_size)); integrity domain, NOT a MAC */
    __le64 oid;
    __le64 xid;
    __le64 self_vblk;
    __le64 owner_id;
    __le32 magic;
    __le16 flags;
    __u8 level;
    __u8 tree_type;
    __le16 nitems;
    __le16 free_head8; /* unit = 8 bytes; 65536 -> 8192 */
    __le16 free_tail8; /* unit = 8 bytes */
    __le16 dead_bytes8;/* unit = 8 bytes */
    struct vtx_node_entry entries[];
} __packed;

/* Child-pointer semantics are selected by the PARENT node flags:
 *   VIRTUAL_OID: child_oid is authoritative. hint_vblk is only a cache;
 *                P0 MUST resolve via this branch's OMAP view.
 *   PHYSICAL_VBLK: child_oid is the child's identity, hint_vblk is the
 *                authoritative child VBLK; hint_xid is its required XID.
 *                CoW of an OMAP child MUST update this pointer in its
 *                parent, propagate CoW to the physical root, then publish
 *                the new OMAP-view root through the checkpoint.
 * This resolves the draft's G1 ambiguity WITHOUT overloading child_oid
 * to sometimes mean a VBLK. A hint never owns an allocation.
 */
struct vtx_key_ptr_val {
    __le64 child_oid;
    __le64 hint_vblk;
    __le64 hint_xid;
} __packed;

struct vtx_omap_val {
    __le64 vblk;
    __le32 node_size;
    __le32 flags;
} __packed;

#define VTX_OMAP_VAL_TOMBSTONE (1U << 0)
#define VTX_SLOT_FLAG_PREFIX_VALID (1U << 0)
#define VTX_SLOT_FLAGS_P0_KNOWN    VTX_SLOT_FLAG_PREFIX_VALID
#define VTX_REF_FLAG_DATA         (1U << 0)
#define VTX_REF_FLAG_META         (1U << 1)
#define VTX_SFQ_FLAG_META         (1U << 0)
#define VTXFS_OID_SFQ_TREE        5ULL
#define VTXFS_OID_UUID_TREE       6ULL
#define VTXFS_OID_DEV_TREE        7ULL

/* 4. Inodes, extents, directory and branch roots ----------------------- */

struct vtx_inode_item {
    __le64 parent_ino;
    __le64 size_bytes;
    __le64 alloc_bytes;
    __le64 update_xid;
    __le64 atime_ns;
    __le64 mtime_ns;
    __le64 ctime_ns;
    __le64 btime_ns;
    __le32 uid;
    __le32 gid;
    __le32 mode;
    __le32 nlink;
    __le64 flags;
    union {
        __le64 rdev;
        __le64 dir_total_bytes;
    } u;
} __packed;

/*
 * One extent ITEM contains header + checksums [+ optional inline payload].
 * No separately keyed checksum item is permitted in the P0 profile.
 * tail[0..csum_count*4) contains little-endian CRC32C values for 4 KiB
 * uncompressed data blocks. crypto/compression are disabled in P0.
 */
#define VTX_EXTENT_PREALLOC       (1U << 0)
#define VTX_EXTENT_INLINE        (1U << 1)
#define VTX_EXTENT_SHARED        (1U << 2)
#define VTX_EXTENT_COMPRESSED    (1U << 3) /* deferred */
#define VTX_EXTENT_ENCRYPTED     (1U << 4) /* deferred */
struct vtx_extent_item {
    __le64 vblk;
    __le64 allocation_id;  /* immutable ID used by ownership/reference */
    __le64 birth_xid;
    __le64 owner_subvol_id;
    __le32 physical_blocks;
    __le32 logical_bytes;
    __le32 flags;
    __le16 csum_type;
    __le16 csum_count;
    __le32 payload_bytes;  /* inline payload bytes after checksums */
    __u8 compression;
    __u8 encryption;
    __le16 header_bytes;   /* must equal sizeof(vtx_extent_item) in P0 */
    __le64 crypto_identity;/* reserved: zero in P0 */
    __u8 tail[];
} __packed;

#define VTXFS_EXTENT_LOOKBACK_BYTES  (256ULL * VTXFS_BLOCK_SIZE)

/*
 * Directory primary key offset: (48-bit hash << 16) | collision slot.
 * Search every candidate in that hash bucket, compare full names. The
 * 16-bit slot bounds collision count and must not silently overwrite data.
 */
#define VTX_DIR_HASH_BITS          48U
#define VTX_DIR_COLLISION_BITS     16U
#define VTX_DIR_MAKE_OFFSET(hash48, slot)     ((((__u64)(hash48) & 0xFFFFFFFFFFFFULL) << 16) | ((__u64)(slot) & 65535ULL))
struct vtx_dir_item {
    __le64 target_ino;
    __le64 dir_seq;
    __le16 name_len;
    __u8 file_type;
    __u8 flags;
    __le32 name_hash_low32;
    __u8 name[];  /* full name; compare it to disambiguate collisions */
} __packed;

#define VTX_SUBVOL_RDONLY         (1ULL << 0)
#define VTX_SUBVOL_REAPING        (1ULL << 1)
#define VTX_SUBVOL_QUOTA_ENABLED  (1ULL << 2)
struct vtx_subvol_root_item {
    __le64 subvol_id;
    __le64 parent_subvol_id;
    __le64 root_node_oid;
    __le64 root_node_vblk;
    __le64 omap_view_root_vblk; /* authoritative view root for this branch */
    __le64 create_xid;
    __le64 change_xid;
    __le64 flags;
    __le64 quota_referenced_bytes;
    __le64 quota_exclusive_bytes;
    __le64 quota_max_referenced;
    __le64 quota_max_exclusive;
    __le64 num_inodes;
    __le64 directory_bytes;
    __u8 uuid[16];
    __u8 parent_uuid[16];
    __u8 received_uuid[16];
    __le64 stransid;             /* last send transaction (DEFERRED) */
    __le64 rtransid;             /* received transaction (DEFERRED) */
    __le64 last_snap_xid;        /* diagnostic only; NOT GC reachability */
    __le64 keybag_oid;           /* encrypted subvolume (DEFERRED) */
    __le64 dir_stats_root_oid;   /* recursive dir stats (DEFERRED) */
    __le64 quota_generation;     /* quota update generation (DEFERRED) */
    __u8 reserved[48];           /* namespace name belongs to directory items */
} __packed;

/* 5. Authoritative ownership and deferred reuse ------------------------ */

/* Refcount 1 MUST be representable in P0; never infer it solely from XID. */
/* Key: (allocation_id, VTX_TYPE_REF, 0).
 * This is authoritative for allocation ownership, including sliced refs. */
struct vtx_ref_item {
    __le64 allocation_id;
    __le64 start_vblk;
    __le64 refcount;
    __le64 birth_xid;
    __le32 block_count;
    __le32 flags;
    __le32 owner_hint;
    __le32 reserved;
} __packed;

/*
 * REFERENCED -> PENDING_REUSE -> REUSABLE.
 * SFQ entry is created only after reference accounting says unreachable.
 * Only a committed retirement epoch + protected-root/I/O validation can
 * turn an allocation reusable. oldest_snap_xid alone is insufficient.
 */
struct vtx_sfq_entry {
    __le64 start_vblk;
    __le64 block_count;
    __le64 retired_xid;
    __le32 flags;
    __le32 reserved;
} __packed;

enum vtx_reaper_phase {
    VTX_REAPER_INVALID=0, VTX_REAPER_QUEUED=1,
    VTX_REAPER_TRAVERSING=2, VTX_REAPER_DONE=3,
    VTX_REAPER_FAILED=4
};
enum vtx_reaper_op {
    VTX_REAPER_OP_DROP_SUBVOL=1,
    VTX_REAPER_OP_DROP_ORPHAN_INODE=2,
    VTX_REAPER_OP_DEC_REF_RANGE=3
};
struct vtx_reaper_job {
    __le64 job_id;
    __le64 object_id;
    __le64 owning_root_id;
    struct vtx_key cursor;
    __le32 phase;
    __le32 operation;
    __le64 completed_xid;
    __le64 flags;
} __packed;




/* -----------------------------------------------------------------------
 * 6. Optional on-disk extensions (DEFERRED: protocol is not implemented).
 *
 * These structs describe candidate layouts. A feature cannot be enabled
 * merely because the compiler verifies the layout.
 * ----------------------------------------------------------------------- */

/* Keep original VTXFS v2 feature scope, but never reuse its incompatible
 * numeric encodings without an explicit migration/format-version bump.
 */
#define VTX_INODE_NODATACOW       (1ULL << 0)  /* NOT ALLOWED in P0 */
#define VTX_INODE_NODATASUM       (1ULL << 1)  /* NOT ALLOWED in P0 */
#define VTX_INODE_COMPRESS        (1ULL << 2)  /* DEFERRED */
#define VTX_INODE_NOCOMPRESS      (1ULL << 3)
#define VTX_INODE_IMMUTABLE       (1ULL << 4)
#define VTX_INODE_APPEND          (1ULL << 5)
#define VTX_INODE_VERITY          (1ULL << 6)  /* DEFERRED */
#define VTX_INODE_DIR_STATS       (1ULL << 7)  /* DEFERRED */
#define VTX_INODE_PIN_HOT_TIER    (1ULL << 8)  /* DEFERRED */
#define VTX_INODE_PIN_COLD_TIER   (1ULL << 9)  /* DEFERRED */

#define VTXFS_OID_INVALID            0ULL
#define VTXFS_OID_ROOT_TREE          1ULL
#define VTXFS_OID_OMAP_META          2ULL
#define VTXFS_OID_REF_TREE           3ULL
#define VTXFS_OID_REAPER_TREE        4ULL
#define VTXFS_OID_FIRST_USER_SUBVOL  256ULL
#define VTXFS_INO_ROOT_DIR           2ULL
#define VTXFS_INO_ORPHAN_DIR         3ULL
#define VTXFS_INO_FIRST_USER         256ULL

/* Assigned AFTER the original v2.1 type IDs, to avoid renumbering. */
#define VTX_TYPE_VERITY_DESC    13U /* DEFERRED */
#define VTX_TYPE_VERITY_MERKLE  14U /* DEFERRED */
#define VTX_TYPE_SUBVOL_REF     15U
#define VTX_TYPE_KEYBAG_ENTRY   16U /* DEFERRED */
#define VTX_TYPE_CHAIN_RECORD   17U /* DEFERRED */
#define VTX_TYPE_DIR_STATS      18U /* DEFERRED */
#define VTX_TYPE_EVICT_STATE    19U /* DEFERRED */
#define VTX_TYPE_XATTR_STREAM   20U /* DEFERRED */

/* Device-table item. Dev IDs 0..254, 0xFFFF = NONE; device generation
 * protects against reusing an old device ID with a new member.
 */
struct vtx_dev_item {
    __u8 dev_uuid[VTXFS_UUID_SIZE];
    __le16 dev_id;
    __le16 tier_id;
    __le32 state_flags;
    __le32 sector_size;
    __le32 generation;
    __le64 total_blocks;
    __le64 used_blocks;
    __le32 total_vag_slots;
    __le32 used_vag_slots;
    __le32 error_count;
    __le32 flags;
} __packed;

/* Separate mirror table: avoids repurposing primary vtx_vag_desc fields.
 * A mirror does not guarantee that user data can survive member loss.
 * Transactional mirror updates MUST be part of the physical VAG-root switch.
 */
struct vtx_vag_mirror_item {
    __le64 start_pblk;
    __le32 vag_id;
    __le32 generation;
    __le16 dev_id;
    __le16 flags;
    __le64 verified_xid;
    __le32 reserved;
} __packed;

/* Quiesced-copy first; no concurrent migration protocol is specified. */
enum vtx_evac_phase {
    VTX_EVAC_IDLE    = 0,
    VTX_EVAC_PREPARE = 1,
    VTX_EVAC_QUIESCE = 2,
    VTX_EVAC_COPY    = 3,
    VTX_EVAC_SWITCH  = 4,
    VTX_EVAC_RETIRE  = 5
};
enum vtx_evac_mode {
    VTX_EVAC_DEV_REMOVE   = 1,
    VTX_EVAC_DEV_REPLACE  = 2,
    VTX_EVAC_TIER_MIGRATE = 3
};
struct vtx_evac_state {
    __le64 job_id;
    __le32 vag_id;
    __le32 phase;    /* enum vtx_evac_phase */
    __le16 src_dev_id;
    __le16 tgt_dev_id;
    __le16 mode;
    __le16 flags;
    __le64 src_pblk;
    __le64 tgt_pblk;
    __le64 cursor_blocks;
    __le64 started_xid;
    __le64 durable_xid;
} __packed;

/* Directory reverse index by dir_seq: a stable key for readdir cookies.
 * Full names are stored in the primary dir item; name comparison is mandatory
 * for collision handling, regardless of hash width.
 */
struct vtx_dir_index_val {
    __le64 target_ino;
    __le64 dir_hash_offset;
    __le16 name_len;
    __u8 file_type;
    __u8 flags;
    __le32 reserved;
} __packed;

#define VTX_XATTR_FLAG_EMBEDDED     0x01U
#define VTX_XATTR_FLAG_DSTREAM_INO  0x02U
struct vtx_xattr_item {
    __le64 dstream_ino;
    __le16 name_len;
    __le16 value_len;
    __u8 flags;
    __u8 reserved[3];
    __u8 data[]; /* name bytes then value bytes, checked against node bounds */
} __packed;

/* A subvolume can be linked via directory items; name is not unique globally. */
struct vtx_subvol_ref_item {
    __le64 parent_subvol;
    __le64 child_subvol;
    __le64 parent_dir_ino;
    __le64 dir_sequence;
} __packed;

/* Keybag is only an on-disk wrapper definition. CRYPTO IS NOT SAFE until
 * nonce uniqueness under rollback, authenticated AAD, KDF and key lifecycle
 * are specified, reviewed and tested independently.
 */
struct vtx_crypto_keybag_entry {
    __u8 key_uuid[16];
    __le64 owning_subvol_id;
    __le32 crypto_key_id;
    __u8 enc_type;
    __u8 key_class;
    __le16 wrapped_key_len;
    __u8 kdf_salt[16];
    __u8 wrapped_key[];
} __packed;

/* Optional authentication metadata for a single immutable crypto object.
 * Actual nonce/tag payload is variable-size and is outside this fixed header.
 * Do not derive a nonce solely from physical address or inode/offset.
 */
struct vtx_crypto_extent_auth {
    __le64 crypto_object_id;
    __le64 key_version;
    __le64 reserved_nonce_epoch;
    __le32 auth_unit_bytes;
    __le32 tag_count;
    __le16 aead_algorithm;
    __le16 nonce_scheme;
    __le32 aad_version;
} __packed;

/* Verification descriptors; tree storage / digest protocol deferred. */
struct vtx_verity_desc {
    __le64 inode;
    __le64 merkle_root_vblk;
    __le64 file_size;
    __le32 hash_algorithm;
    __le32 block_size;
    __u8 root_hash[32];
} __packed;

/* Hash-chain provenance alone is NOT an attestation / anti-rollback
 * guarantee. It must be authenticated by an external trust anchor.
 */
struct vtx_chain_record {
    __u8 digest[32];
    __u8 previous_digest[32];
    __le64 xid;
    __le64 root_generation;
    __le32 algorithm;
    __le32 key_id; /* 0 = unauthenticated chain */
} __packed;

/* 7. Fast-WAL draft: entirely DISABLED in P0. -------------------------- */

#define VTXFS_WAL_MAGIC             0x314C5756U /* "VWL1" LE */
#define VTXFS_WAL_FRAME_BYTES       4096U
#define VTXFS_WAL_FRAME_FLAG_DATA   (1U << 0)
#define VTXFS_WAL_FRAME_FLAG_COMMIT (1U << 1)
#define VTXFS_WAL_OP_UPSERT_ITEM    1U
#define VTXFS_WAL_OP_DELETE_ITEM    2U
#define VTXFS_WAL_OP_ALLOC_EXTENT   3U
#define VTXFS_WAL_OP_SFQ_APPEND     4U

/* Checksum covers bytes [16..4095] of a fully initialized frame.
 * One transaction may span multiple frames. COMMIT record must commit
 * all frames and associated durable data/allocation dependencies.
 */
struct vtx_wal_frame_hdr {
    __u8 frame_digest[16];
    __le32 magic;
    __le32 frame_sequence;
    __le64 base_checkpoint_xid;
    __le64 transaction_id;
    __le64 subvol_id;
    __le32 frame_index;
    __le32 frame_count;
    __le32 payload_bytes;
    __le32 frame_flags;
} __packed;

/* Always 32 bytes before value bytes; value bytes follow and are padded to
 * 8B. val_len is ACTUAL payload length, excluding alignment padding.
 */
struct vtx_wal_delta_rec {
    __le64 target_oid;
    __le64 key_oid_and_type;
    __le64 key_offset;
    __le16 op_code;
    __le16 val_len;
    __le32 flags;
    __u8 val_payload[];
} __packed;

/* Transaction COMMIT is a separate framed record. Its txn_digest covers
 * every preceding frame in the transaction, in order, plus transaction_id
 * and frame_count. The exact crypto serialization is not frozen here.
 */
struct vtx_wal_commit {
    __u8 txn_digest[32];
    __le64 transaction_id;
    __le64 base_checkpoint_xid;
    __le32 frame_count;
    __le32 flags;
    __le64 dependency_xid;
} __packed;

/* Durable scrub cursor. Repair from an absent copy MUST NOT be attempted. */
struct vtx_scrub_state {
    __le64 last_checked_vblk;
    __le64 checkpoint_xid;
    __le64 verified_blocks;
    __le32 error_count;
    __le32 flags;
} __packed;

/* -----------------------------------------------------------------------
 * 7b. Btrfs-inspired descriptor/index types adapted to VTXFS OMAP/VAG.
 *
 * Every added item has EXACTLY ONE authority:
 *   allocation ownership -> vtx_ref_item and committed SFQ state;
 *   physical VAG placement -> checkpoint-selected VAG directory;
 *   file read mapping -> vtx_extent_item;
 *   system root publication -> checkpoint descriptor.
 * The extra indexes below are reconstructible, not parallel authorities.
 * ----------------------------------------------------------------------- */
#define VTX_TYPE_ALLOC_BY_VBLK    21U /* reverse location index */
#define VTX_TYPE_FREE_SPACE       22U /* cache */
#define VTX_TYPE_TREE_BLOCK       23U /* metadata-index descriptor */
#define VTX_TYPE_EXTENT_DATA      24U /* slice mapping index */
#define VTX_TYPE_DEV_EXTENT       25U /* device/VAG reverse index */
#define VTX_TYPE_ROOT_ITEM        26U /* tree descriptor */
#define VTX_TYPE_DEV_ITEM         27U /* member descriptor */

#define VTX_FREE_SPACE_EXTENT     1U
#define VTX_FREE_SPACE_BITMAP     2U
/* Key: (VAG ID, VTX_TYPE_FREE_SPACE, block offset within VAG).
 * This is a CoW checkpoint-generation cache: REF/SFQ still decide reuse.
 */
struct vtx_free_space_item {
    __le64 start_vblk;
    __le64 length_blocks;
    __le64 checkpoint_xid;
    __le32 vag_id;
    __le16 kind;
    __le16 flags;
} __packed;

/* Reverse lookup for scrub/evacuation; keyed by physical start_vblk.
 * Recoverable from authoritative ref items; never owns allocations.
 */
struct vtx_alloc_by_vblk_item {
    __le64 start_vblk;
    __le64 allocation_id;
    __le32 block_count;
    __le32 flags;
} __packed;

/* Describes a metadata node and its allocation; NOT a new refcount.
 * Metadata node pointers are interpreted according to tree type/OMAP view.
 */
struct vtx_tree_block_item {
    __le64 allocation_id;
    __le64 self_vblk;
    __le64 owner_tree_oid;
    __le64 birth_xid;
    __le32 node_bytes;
    __le16 level;
    __le16 flags;
} __packed;

/* Secondary slice index, not an alternative extent definition.
 * An extent slice must fall wholly inside the authoritative vtx_ref_item
 * [start_vblk, start_vblk + block_count), with no overflow.
 * Logical checksum/data mapping lives in vtx_extent_item.
 */
struct vtx_extent_data_item {
    __le64 inode;
    __le64 file_offset;
    __le64 allocation_id;
    __le64 physical_start_vblk;
    __le32 slice_off_blocks;
    __le32 slice_blocks;
    __le32 logical_bytes;
    __le32 flags;
} __packed;

/* Reverse physical member/VAG location. Physical VAG mapping remains
 * authoritative, and this is DEFERRED until multi-device protocol exists.
 */
struct vtx_dev_extent_item {
    __le64 physical_start_pblk;
    __le64 size_blocks;
    __le32 vag_id;
    __le32 generation;
    __le16 device_id;
    __le16 flags;
    __le32 reserved;
} __packed;

/* Uniform tree root metadata descriptor. CPD is still commit authority.
 * Subvolumes carry their own persistent OMAP view.
 */
struct vtx_root_item {
    __le64 tree_oid;
    __le64 root_vblk;
    __le64 omap_view_root_vblk;
    __le64 commit_xid;
    __le32 root_bytes;
    __le16 tree_type;
    __u8 level;
    __u8 flags;
    __le64 owning_subvol_id;
    __le64 reserved;
} __packed;

/* Existing vtx_inode_item is 96 bytes. A future RO_COMPAT_INODE_V2
 * enables 128-byte value and explicit generation/i_version/project IDs.
 * Signed timestamps are 2-complement ns since Unix epoch in all inode
 * variants. Readers MUST NOT reinterpret an old 96B value as 128B.
 */
struct vtx_inode_item_v2 {
    struct vtx_inode_item base;
    __le64 generation;
    __le64 change_sequence;
    __le32 project_id;
    __le32 reserved32;
    __le64 next_dir_seq;
} __packed;

enum vtx_file_type {
    VTX_FT_UNKNOWN=0, VTX_FT_REG=1, VTX_FT_DIR=2,
    VTX_FT_CHRDEV=3, VTX_FT_BLKDEV=4, VTX_FT_FIFO=5,
    VTX_FT_SOCK=6, VTX_FT_SYMLINK=7, VTX_FT_SUBVOL=8,
    VTX_FT_MAX=8
};

struct vtx_inode_ref {
    __le64 dir_seq;
    __le16 name_len;
    __u8 reserved[6];
    __u8 name[];
} __packed;

struct vtx_uuid_map_item {
    __u8 uuid[16];
    __le64 subvol_id;
    __le32 kind;    /* 1=normal UUID, 2=received UUID */
    __le32 reserved;
} __packed;

/* Four 4KiB blocks; header 64B and 255 entries of 64B. */
struct vtx_dev_table_hdr {
    __u8 checksum[32];
    __le32 magic;
    __le32 entry_count;
    __le64 generation;
    __u8 fsid_uuid[16];
} __packed;
struct vtx_dev_table {
    struct vtx_dev_table_hdr hdr;
    struct vtx_dev_item devs[VTXFS_MAX_DEVICES];
} __packed;

#define VTX_XATTR_MAKE_OFFSET(hash48, slot16) VTX_DIR_MAKE_OFFSET(hash48, slot16)

/* Compile-time layout is necessary, NOT sufficient for runtime safety. */
VTXFS_STATIC_ASSERT(sizeof(struct vtx_ref_item) == 48, "ref has allocation range");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_free_space_item) == 32, "free space cache");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_alloc_by_vblk_item) == 24, "reverse allocation index");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_tree_block_item) == 40, "tree block");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_extent_data_item) == 48, "extent data");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_dev_extent_item) == 32, "dev extent");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_root_item) == 56, "generic root item");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_inode_item_v2) == 128, "extended inode");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_inode_ref) == 16, "inode ref");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_uuid_map_item) == 32, "UUID map");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_dev_table_hdr) == 64, "dev table header");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_dev_table) == VTXFS_DEV_TABLE_BLOCKS * VTXFS_BLOCK_SIZE, "dev table bytes");
VTXFS_STATIC_ASSERT(sizeof(VTXFS_MAGIC_BYTES) == VTXFS_MAGIC_LEN, "FS magic length");
VTXFS_STATIC_ASSERT(sizeof(VTXFS_CPD_MAGIC) == VTXFS_MAGIC_LEN, "CPD magic length");

/* -----------------------------------------------------------------------
 * 8. Size and geometry compile-time checks; runtime invariants remain
 * the responsibility of the parser/driver and its crash-recovery tests.
 * ----------------------------------------------------------------------- */

VTXFS_STATIC_ASSERT(sizeof(struct vtx_phys_ref) == 24, "phys root size");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_super_header) == 256, "super header v2.1 layout");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_super_header, metadata_uuid) == 144, "metadata uuid offset");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_super_header, label) == 192, "label offset");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_checkpoint_header, fsid_uuid) == 184, "CPD fsid offset");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_checkpoint_header, omap_meta_tree_vblk) == 200, "CPD OMAP root offset");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_checkpoint_header) == 272, "checkpoint header v2.1 layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_super_block) == VTXFS_BLOCK_SIZE, "superblock size");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_super_header, magic) == 32, "superblock magic offset");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_checkpoint_desc) == VTXFS_BLOCK_SIZE, "checkpoint size");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_checkpoint_header, magic) == 32, "checkpoint magic offset");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_vag_root_header) == 64, "VAG root header");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_vag_dir_entry) == 16, "VAG root entry");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_vag_dir_root) == VTXFS_BLOCK_SIZE, "VAG root node size");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_vag_desc) == 32, "VAG descriptor");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_vag_desc) * VTXFS_VAG_LEAF_ENTRIES == 65536, "VAG leaf bytes");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_key) == 16, "B+tree key size");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_node_entry) == 24, "slotted B+tree entry");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_node_hdr) == 64, "node header");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_node_hdr, entries) == 64, "node entries offset");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_inode_item) == 96, "inode layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_extent_item) == 64, "extent layout");
VTXFS_STATIC_ASSERT(offsetof(struct vtx_extent_item, tail) == 64, "extent data follows header");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_subvol_root_item) == 256, "subvolume root layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_reaper_job) == 64, "reaper item layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_dev_item) == 64, "device item layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_vag_mirror_item) == 32, "VAG mirror layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_evac_state) == 64, "evacuation layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_dir_index_val) == 24, "readdir index");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_xattr_item) == 16, "xattr layout");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_subvol_ref_item) == 32, "subvolume link");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_crypto_keybag_entry) == 48, "keybag header");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_crypto_extent_auth) == 40, "auth header");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_verity_desc) == 64, "verity descriptor");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_chain_record) == 88, "hash chain item");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_wal_frame_hdr) == 64, "WAL frame header");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_wal_delta_rec) == 32, "WAL delta header");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_wal_commit) == 64, "WAL commit");
VTXFS_STATIC_ASSERT(sizeof(struct vtx_scrub_state) == 32, "scrub cursor");
VTXFS_STATIC_ASSERT(VTXFS_MAX_VAGS == 516096U, "maximum VAGs in two levels");
VTXFS_STATIC_ASSERT(VTXFS_MAX_VBLKS * 4096ULL == 63ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL, "63 TiB virtual limit");

#ifdef __cplusplus
}
#endif
#endif /* _VTXFS_DISK_H */
