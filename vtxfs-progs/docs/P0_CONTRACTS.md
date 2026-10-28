# VTXFS v2.1 P0/P0.6/P1 — Implemented Contracts & Architecture Specification

Status: **Complete P0 Single-Device Read/Write Core Engine, Verifier, CLI Suite & Linux Kernel VFS / Native ctree Subsystem Stack (dual-mode portable C11/C17 + Linux kernel kbuild).**

## 1. Implemented and Verified Core & CLI Modules

| Module | Responsibilities & Enforced Contracts |
|---|---|
| `core/vtxfs_csum.c` | Castagnoli `CRC32C` (`0x82F63B78`), RFC-compliant `BLAKE3-256` (multi-chunk tree hashing), `metadata_uuid`-domain-separated `BLAKE3-128`, `SipHash-2-4` keyed directory/xattr hashing, and structure digest verification (`Superblock`, `DevTable`, `CPD`, `VAG`, `Node`). |
| `core/vtxfs_leaf.c`, `core/vtxfs_leaf_balance.c` | Slotted leaf operations (`init`, `find`, `insert`, `replace`, `delete`, `compact`) and byte-weighted sibling balancing (`split`, `split_insert`, `merge`, `rebalance`) with all-or-nothing scratch rebuild. |
| `core/vtxfs_internal.c`, `core/vtxfs_internal_balance.c` | Slotted internal node operations and two-sibling balancing (`split`, `split_insert`, `merge`, `rebalance`) for 24-byte `vtx_key_ptr_val` entries in both `VIRTUAL_OID` and `PHYSICAL_VBLK` modes. |
| `core/vtxfs_btree_ro.c` | Separator-verified read-only multi-level B+Tree walker with strict `owner_id`, `tree_type`, `level`, `xid`, and minimum-key verification. |
| `core/vtxfs_btree_rw.c` | Full multi-level CoW B+Tree write and cursor engine (`vtx_rw_lookup`, `vtx_rw_next`, `vtx_rw_insert`, `vtx_rw_replace`, `vtx_rw_upsert`, `vtx_rw_delete` across levels `0..7`) with path CoW, separator propagation, root splits, and root contraction. |
| `core/vtxfs_vag.c` | Sparse RAM block device, userspace file-backed block device, I/O fault injection, and two-level physical VAG directory & bitmap allocator (`vtx_vag_alloc_range`, `vtx_vag_mark_range`, per-CPD-slot physical persistence & 64 MiB mirror). |
| `core/vtxfs_engine.c` | Portable wall-clock source (`vtx_time_now_ns`), node cache, fixed-point delayed-reference drain (`VTX_TREE_REF`, `VTX_TYPE_ALLOC_BY_VBLK`, `VTX_TREE_SFQ`), ordered checkpoint commit protocol, bootstrap discovery & mount recovery, SFQ promotion (`vtx_sfq_promote`), bitmap cache rebuild (`vtx_vag_rebuild_bitmaps`), and crash-resumable Reaper (`vtx_reaper_run_steps`, `vtx_reaper_run_all`). |
| `core/vtxfs_posix.c` | Subvolume & RO/RW snapshot lifecycle, POSIX directory/file/symlink/mknod/xattr operations (`vtx_mknod`, `vtx_listxattr`, POSIX timestamp stamping), inline & multi-block extent read/write with per-block `CRC32C` verification, extent splitting/trimming, `truncate`, `reflink`, and UAPI ioctl query handlers. |
| `core/vtxfs_fsck.c` | Independent disk verifier (`vtx_fsck`) checking superblocks, CPD rings, VAG directories, all system and subvolume B+Trees, per-block data `CRC32C`, cross-tree `REF` reference counts, `SFQ` disjointness, and VAG bitmaps. |
| `tools/*.c` | `mkfs.vtxfs`, `fsck.vtxfs`, `vtxfs-inspect`, and `vtxfs-ioctl-test` userspace utilities. |

## 2. Authoritative Mapping and Node Semantics

- `vtx_key` is ordered as decoded numeric `(oid, type, offset)`; on-disk fields are encoded with `vtx_le16/32/64` and decoded using `vtx_cpu16/32/64`.
- Parent separators equal the exact minimum key (`entries[0].key`) in their selected child; internal search selects `upper_bound(separator, key) - 1`, falling back to slot 0.
- Every node binds `owner_id`: `0` for system trees (`VTX_TREE_ROOT`, `VTX_TREE_OMAP_META`, `VTX_TREE_REF`, `VTX_TREE_REAPER`, `VTX_TREE_SFQ`, `VTX_TREE_UUID`) and `subvol_id` for subvolume trees (`VTX_TREE_FS`, `VTX_TREE_OMAP_VIEW`).
- For `VIRTUAL_OID` trees, `child_oid` is resolved through the owning OMAP (`VTX_TREE_OMAP_META` for system trees, `VTX_TREE_OMAP_VIEW` for subvolume `VTX_TREE_FS`).
- For `PHYSICAL_VBLK` trees (`VTX_TREE_OMAP_META` and `VTX_TREE_OMAP_VIEW`), `hint_vblk` and `hint_xid` are authoritative physical child pointers updated bottom-up on CoW.

## 3. Ownership (`VTX_TREE_REF`), Safe-Free Queue (`VTX_TREE_SFQ`) & Reaper (`VTX_TREE_REAPER`)

- `vtx_ref_item` (keyed by `(allocation_id, VTX_TYPE_REF, 0)` with secondary lookup `(start_vblk, VTX_TYPE_ALLOC_BY_VBLK, block_count)`) is the sole authority for physical allocation ownership (`refcount >= 1`).
- During checkpoint commit, delayed references are drained to a fixed point so metadata node allocations/frees triggered by updating `VTX_TREE_REF`, `VTX_TREE_SFQ`, and `VTX_TREE_OMAP_META` are themselves fully accounted in `VTX_TREE_REF` and `VTX_TREE_SFQ`.
- When an allocation's `refcount` drops to `0`, its `vtx_ref_item` and `VTX_TYPE_ALLOC_BY_VBLK` index are removed and a `vtx_sfq_entry` is inserted into `VTX_TREE_SFQ` (`PENDING_REUSE`) while keeping its VAG bitmap bits set (`1`) so no uncommitted or pinned reader can observe premature block reuse.
- `vtx_sfq_promote` transactionally deletes eligible `vtx_sfq_entry` records (`retired_xid <= safe_xid`) and clears their VAG bitmap bits.
- `vtx_vag_rebuild_bitmaps` reconstructs all VAG bitmaps and `free_blocks` counts purely from `VTX_TREE_REF` $\cup$ `VTX_TREE_SFQ` plus permanently reserved `VBLK 0`.
- `vtx_subvol_destroy` marks the subvolume `VTX_SUBVOL_REAPING` and inserts a durable `vtx_reaper_job` in `VTX_TREE_REAPER`. `vtx_reaper_run_steps` advances the persistent cursor across transactions and remounts until all extents, nodes, and OMAP view entries are reclaimed.

## 4. Ordered Checkpoint Commit & Crash Recovery

1. **Step 1 (Data Write)**: Write all dirty data blocks to their allocated VAG physical blocks (`VTX_FAULT_DATA_WRITE`).
2. **Step 2 (Data Flush)**: Issue device flush barrier so data blocks are durable before any metadata references them (`VTX_FAULT_DATA_FLUSH`).
3. **Step 3 (Metadata Write)**: Compute `metadata_uuid`-domain-separated `BLAKE3-128` checksums on all dirty B+Tree nodes and write them (`VTX_FAULT_META_WRITE`).
4. **Step 4 (Physical VAG Directory Write)**: Persist the checkpoint slot's physical `vtx_vag_dir_root`, 64 KiB VAG leaf, and VAG bitmaps in `VTXFS_IP_BOOT_PBLK + (xid % 8) * 512` and its 64 MiB mirror (`VTX_FAULT_VAG_WRITE`).
5. **Step 5 (Metadata Flush)**: Issue device flush barrier for all metadata and VAG blocks (`VTX_FAULT_META_FLUSH`).
6. **Step 6 (CPD Publish with FUA + Mirror)**: Sign `vtx_checkpoint_desc` with `BLAKE3-256` and write to `VTXFS_CPD_RING_PBLK + (xid % 8)` with FUA (`VTX_FAULT_CPD_PRIMARY`), followed by the 64 MiB mirror CPD slot (`VTX_FAULT_CPD_MIRROR`).
7. **Mount & Recovery**: Scans primary and 64 MiB mirror superblocks/device tables, then evaluates all 8 CPD ring slots (primary and mirror) in descending `xid` order, selecting the highest `xid` whose checksum, UUIDs, `super_generation`, VAG directory, and system tree roots all validate.

## 8. P0.5 — Kernel VFS Adapter & Engine Time Source
- `vtx_time_now_ns()`: kernel uses `ktime_get_real_ns()`, userspace uses `CLOCK_REALTIME`. `create`/`mknod` stamps `atime`/`mtime`/`ctime`/`btime`; `write`/`truncate` stamps `mtime`/`ctime`; directory mutations (`dir_add_entry`/`dir_remove_entry`) stamp parent directory `mtime`/`ctime`.
- `vtx_listxattr()`: walks `VTX_TYPE_XATTR` keys in sorted order, emitting NUL-separated attribute names.
- `vtx_mknod()`: supports `FIFO`/`CHRDEV`/`BLKDEV`/`SOCK`, storing `rdev` in `vtx_inode_item.u.rdev`.
- Kernel adapter (`kernel/super.c`, `kernel/inode.c`, `kernel/file.c`, `kernel/xattr.c`): exposes the `"user."` xattr namespace via `sb->s_xattr`; handles `VTXFS_IOC_GET_ABI`, `VTXFS_IOC_GET_CAPABILITIES`, `VTX_IOC_START_SYNC`, and `VTX_IOC_WAIT_SYNC`; supports RO/RW reconfigure (`remount`) with clean checkpoint commit on RW->RO downgrade, `freeze_fs`/`unfreeze_fs`, and `show_options` (`label=`, `subvol=`).
- Subvolume/snapshot creation/destruction ioctls remain DEFERRED per the UAPI contract (`-EOPNOTSUPP`).

## 9. P0.6 & P1 — Kernel Subsystem Map (`kernel/`)
- **B+Tree Root & Subvolume Cache**: `kernel/root-tree.c` (`VTX_TREE_ROOT` scan) + `kernel/root.c` (per-subvolume root cache using `xarray` + `refcount_t` + RCU). Subvolumes/snapshots are mounted via `mount -o subvol=<id>`.
- **Directory & Inode Policy**: `kernel/dir-item.c` (filename validation, `RENAME_NOREPLACE` atomic check) and `kernel/inode-item.c` (`vtx_inode_item` <-> VFS `struct inode` mapping).
- **Block & Metadata Cache**: `kernel/lru_cache.c` (generic intrusive LRU) and `kernel/disk-io.c` (VBLK->PBLK resolution, bio read, `BLAKE3-128` verification, LRU cache).
- **Direct I/O, Writeback, FIEMAP & NFS Export**:
  - `kernel/direct-io.c`: `O_DIRECT` reads bypass the page cache via engine-verified bounce buffers (`CRC32C` verified per 4 KiB block); `O_DIRECT` writes fall back to buffered CoW.
  - `kernel/writeback.c`: batched `writepages` coalescing up to 32 contiguous dirty folios (128 KiB) per engine write.
  - `kernel/fiemap.c`: `FS_IOC_FIEMAP` over `VTX_TYPE_EXTENT` items (`FIEMAP_EXTENT_DATA_INLINE` and VAG physical block mapping).
  - `kernel/export.c`: NFS `export_operations` (`encode_fh`, `fh_to_dentry`, `fh_to_parent`, `get_parent`).
  - `kernel/ioctl.c`: centralized ioctl dispatcher returning `-EOPNOTSUPP` with ratelimited logging for reserved feature-gated ioctls.
  - `kernel/locking.c`: hierarchical locking (`VFS locks -> commit_sem -> op_lock`).
- **Native Kernel `ctree` & Transaction Subsystems (`include/vtxfs_mem.h`)**:
  - `kernel/extent_io.c`: `vtxfs_extent_buffer` cache/I/O/BLAKE3, `vtxfs_extent_io_tree` extent state/locking, and `vtxfs_extent_map` RB-tree cache.
  - `kernel/ctree.c`: path-based CoW B+Tree operations (`vtxfs_search_slot`, `vtxfs_search_old_slot`, `vtxfs_search_slot_for_read`, `vtxfs_search_forward`, `vtxfs_next_key`, `vtxfs_previous_item`, `vtxfs_cow_block`, `vtxfs_copy_root`, `vtxfs_insert_item`, `vtxfs_split_item`, `vtxfs_truncate_item`, `vtxfs_extend_item`, `vtxfs_del_item`, `vtxfs_del_ptr`).
  - `kernel/extent-tree.c` & `kernel/inode-ext.c`: tree block & extent allocation, `REF`/`SFQ` manipulation, inode/inode-ref helpers, inline extents, and async delalloc range CoW.
  - `kernel/trans.c` & `kernel/ordered-extent.c`: native transaction join/commit state machine and ordered extent completion tracking.
  - `kernel/delayed-inode.c`, `kernel/async-thread.c`, `kernel/compression.c`, `kernel/uuid-tree.c`, `kernel/orphan.c`, `kernel/print-tree.c`, `kernel/reaper.c`, `kernel/backref.c`, `kernel/sysfs.c`, `kernel/free-space-cache.c`.

## 10. Part 6 — Native Kernel Subsystems (`kernel/`)
- **Native Mount & Bootstrap (`kernel/mount-native.c`)**:
  - Scans primary and 64 MiB mirror Superblock rings, verifying geometry, version, CRC, and feature policy.
  - Scans primary and mirror CPD rings, selecting the latest valid checkpoint descriptor by generation and xid.
  - Loads physical VAG directory (`phys_ref -> root -> leaf -> bitmaps`) and validates checksums.
  - Verifies and loads all system tree roots (`VTX_TREE_OMAP_META`, `VTX_TREE_ROOT`, `VTX_TREE_REF`, `VTX_TREE_SFQ`, `VTX_TREE_REAPER`, `VTX_TREE_UUID`) directly into memory without engine wrappers.
- **Kernel-Native VAG Allocator (`kernel/vag.h`, `kernel/vag-alloc.c`)**:
  - Authoritative bitmap allocator (`vtxfs_vag_alloc_blocks`, `vtxfs_vag_free_blocks`) and virtual-to-physical translation (`vtxfs_vag_vblk_to_pblk`).
  - Implements `vtxfs_alloc_ops` (`fs->alloc_ops`) wired directly into `extent-tree.c` and `ctree.c`.
- **OMAP View Management (`kernel/omap.c`)**:
  - Implements `struct vtxfs_omap_view` resolution (`vtxfs_omap_view_resolve`), upsert (`vtxfs_omap_view_set`), and invalidation (`vtxfs_omap_view_invalidate`) for `VIRTUAL_OID` children.
  - Integrated into `ctree.c` child block resolution (`omap_resolve_vblk`) and `vtxfs_cow_block`.
- **Extent Drop & Truncate Control (`kernel/truncate.c`)**:
  - Native half-open interval extent dropping and splitting (`vtxfs_drop_extents`) using `vtxfs_drop_extents_args`.
  - Inode file truncation (`vtxfs_truncate_inode`) updating tree extents and disk inode size; wired into `vtxfs_setattr`.
- **Integrity Scrub Worker (`kernel/scrub.c`)**:
  - Dedicated background kthread (`vtxfs-scrub`) walking `VTX_TREE_REF` and verifying extents against disk blocks.
  - Tracks durable progress via `vtx_scrub_state` and exposed to userspace via `VTX_IOC_SCRUB`.
- **In-Kernel KUnit Test Suite (`kernel/kunit/vtxfs_ctree_test.c`)**:
  - Tests CRC32C, BLAKE3-256, slotted leaf item insert/find/delete, extent buffer refcounts, and `io_tree` bit locking.

## 11. Part 7 — Core Consolidation & In-RAM Multi-Level Tree Self-Test
- **Descent & CoW Sequencing (`kernel/ctree.c`)**:
  - Rewritten `vtxfs_search_slot` with downward descend-and-CoW protocol correctly updating parent/slot before traversing to child nodes.
  - Full leaf splitting (`vtx_leaf_split_insert`), recursive parent pointer insertion (`insert_ptr_up`), and root growth (`grow_root`) when nodes reach capacity.
  - Item and pointer deletion (`vtxfs_del_item`, `vtxfs_del_ptr`) with root contraction when nodes become empty.
- **Transaction Struct & Publishing (`kernel/trans.c`, `kernel/disk-io.c`)**:
  - Added authoritative `struct vtxfs_fs_info *fs` to `struct vtxfs_trans`.
  - Transaction commit pipeline publishes dual-mirror FUA signed CPD descriptors (`vtxfs_publish_cpd`).
- **Validated Extent Truncation (`kernel/truncate.c`)**:
  - Cleaned `vtxfs_drop_extents` verifying all extent items with `vtx_extent_p0_value_valid` and handling exact tail, head, middle split, and complete overlap cases.
- **In-RAM Multi-Level Tree Self-Test (`kernel/selftest.c`)**:
  - In-RAM synthetic mock allocator verifying 300 sequential inserts, multi-level node splits, root growth, search, and truncation directly on module load (`vtxfs_run_selftest`).

## 12. Part 8 — P2 Consolidation: Sibling Balance, OMAP Propagation, fsck Disk Compatibility, Real RAM Block Device, Delalloc & Ordered Extents
- **Sibling Merge & Rebalance (`kernel/ctree_balance.c`)**:
  - Implements `vtxfs_balance_after_delete`: underfull node detection, CoW sibling traversal, leaf and internal node merging (`vtx_leaf_merge`, `vtx_internal_merge`), and sibling rebalancing (`vtx_leaf_rebalance`, `vtx_internal_rebalance`).
  - Root contraction (`vtxfs_contract_root`) when internal root collapses to a single child.
- **OMAP CoW Propagation (`include/vtxfs_mem.h`, `kernel/omap.c`, `kernel/ctree.c`)**:
  - Added `resolve_root` to `struct vtxfs_root`.
  - Implemented `vtxfs_omap_publish` and `vtxfs_omap_resolve_vblk` for non-recursive physical OMAP mapping updates and resolution.
  - Updated `vtxfs_cow_block`, `grow_root`, and split paths in `ctree.c` to propagate new VBLK locations up to `resolve_root` for all system `VIRTUAL_OID` trees.
- **Engine Fsck Disk Compatibility (`kernel/ctree_compat.c`, `kernel/ctree.c`)**:
  - Strict ROOT flag management (`vtxfs_ctree_set_root_flag`) on root growth and contraction.
  - Delayed reference tracking (`vtxfs_ctree_delayed_ref`) during block allocation and deallocation, drained into the REF tree at commit (`vtxfs_run_delayed_refs`).
  - SFQ safe reuse promotion (`vtxfs_sfq_promote`) executed upon transaction commit to free retired blocks back to the VAG bitmap once previous transactions are durable.
- **In-Kernel RAM Block Device (`kernel/ramdisk.c`)**:
  - Bio-based in-memory block device (`vtxfs_ramdisk_create`, `vtxfs_ramdisk_destroy`) using `struct queue_limits` and `blk_alloc_disk` on Linux 7.2 / 6.8+.
- **End-to-End Kernel Self-Test (`kernel/selftest_e2e.c`)**:
  - Adapts `struct block_device` to engine `vtx_bdev_ops` (`kbdev_ops`).
  - Runs full lifecycle: RAM disk creation -> engine mkfs (`vtx_mkfs`) -> native kernel mount (`vtxfs_mount_native`) -> 400 B+Tree inserts -> 200 deletes exercising balance and root contraction -> commit and delayed-ref run -> engine verification (`vtx_fsck`).
  - Standalone build as `vtxfs_e2e.ko` (`CONFIG_VTXFS_E2E_TEST=m`).
- **Real Delayed Allocation & Ordered Extents (`kernel/delalloc.c`)**:
  - `vtxfs_delalloc_reserve` sets `VTXFS_EXT_DELALLOC` and reserves space.
  - `vtxfs_run_delalloc_range` allocates physical blocks at flush, collects page cache pages via `gather_pages`, computes per-block CRC32C, submits data bios via `submit_data`, tracks lifecycle via `struct vtxfs_ordered_extent` (`vtxfs_alloc_ordered`, `vtxfs_ordered_complete`), inserts extent items, logs delayed references, and clears delalloc bits.

## 13. Parts 9 & 10 — Kernel Consolidation: Lock-Coupling Descent, Insert Balance, Ordered Commit Pipeline, Authoritative CPD Publish & Lifecycle Teardown
- **Ordered Extent Lifecycle & Commit Integration (`kernel/ordered-extent.c`, `kernel/trans.c`)**:
  - `struct vtxfs_ordered_extent` tracks in-flight asynchronous data bios (`vtxfs_alloc_ordered`, `vtxfs_ordered_set_io_done`, `vtxfs_ordered_finish`, `vtxfs_submit_ordered_data`).
  - Commit pipeline (`vtxfs_commit_transaction`) executes the strict 5-step durability ordering:
    1. `vtxfs_flush_all_delalloc`: flush dirty delalloc folios to ordered extents.
    2. `vtxfs_wait_ordered_all`: block until all in-flight data I/Os complete.
    3. `vtxfs_run_delayed_refs`: drain delayed references to the `REF` and `SFQ` trees.
    4. `vtxfs_sfq_promote`: promote safe retired blocks back to the VAG bitmap.
    5. `vtxfs_write_dirty_ebs` & `blkdev_issue_flush`: write dirty metadata buffers followed by a barrier before CPD publish.
- **Kernel-Side Safe-Free Queue Promotion (`kernel/sfq.c`)**:
  - `vtxfs_sfq_promote` scans `VTX_TREE_SFQ` for entries with `retired_xid <= safe_xid`, verifies zero remaining references in `VTX_TREE_REF`, deletes the SFQ item, and frees the corresponding physical VAG blocks.
- **Descending Lock-Coupling (`kernel/ctree_locking.c`, `include/vtxfs_mem.h`, `kernel/ctree.h`)**:
  - Replaces the global `trans_lock` for tree descent mutations:
    - Reader descent: read-lock crabbing (`down_read(&child->lock)` before releasing parent).
    - Writer descent: top-down write-locking holding exclusive locks along the path to absorb splits and merges without deadlock (`vtxfs_search_slot_locked`, `vtxfs_release_path_locked`).
- **Asymmetric Insert Rebalancing (`kernel/ctree_balance.c`)**:
  - `vtxfs_balance_after_insert`: monitors node fullness after split/insert, evening out skewed inserts with adjacent underfull siblings via `vtx_leaf_rebalance` and `vtx_internal_rebalance`.
- **Authoritative Dual-Mirror CPD Publishing (`kernel/cpd.c`)**:
  - Signs `vtx_checkpoint_desc` with `BLAKE3-256` and writes FUA to the primary CPD ring slot (`VTXFS_CPD_RING_PBLK + (xid % 8)`) and the 64 MiB mirror slot (`VTXFS_BOOT_MIRROR0_PBLK + VTXFS_CPD_RING_PBLK + (xid % 8)`).
- **Extent Item CRC Builder (`kernel/extent-item.c`)**:
  - `vtxfs_build_extent_item` and alias `insert_extent_item_crc` build and insert standard `vtx_extent_item` entries with per-block CRC32C arrays directly into the tree under path locks.
- **Consolidated Super Lifecycle & Teardown (`kernel/mount-native.c`, `kernel/super.c`)**:
  - `vtxfs_mount_native_teardown` releases system roots, cleans up `fs->eb_cache`, frees the VAG directory, and releases `fs_info`.
  - `vtxfs_fill_super` and `vtxfs_put_super` integrate native mount, async threads, sysfs, reaper, and cleanup paths cleanly.

## 14. Parts 11 & 12 — Complete Userspace Tooling Suite & Metadump Architecture
- **Unified CLI `vtxfs` (`tools/vtxfs/`)**:
  - `vtxfs info <image>`: Reads filesystem geometry, UUID, total and free blocks, active commit XID, and incompat flags.
  - `vtxfs sync <image>`: Synchronously executes `vtx_sync_checkpoint` and commits an explicit transaction checkpoint.
  - `vtxfs subvolume <list|create|delete> <image>`:
    - `list`: Iterates through `VTX_TREE_ROOT` subvolume items, resolves parent subvolume directory entries of type `VTX_FT_SUBVOL` (`vtx_subvol_get_name`) to display human-readable names alongside IDs and flags (`ro`, `reaping`, `quota`).
    - `create`: Allocates a new subvolume root item in `VTX_TREE_ROOT` and creates a corresponding `VTX_FT_SUBVOL` directory entry in the parent root directory.
    - `delete`: Marks the subvolume with `VTX_SUBVOL_REAPING`, unlinks the dirent, and queues asynchronous background extents reclamation via `vtx_reaper_run_all`.
  - `vtxfs snapshot <list|create|delete> <image>`:
    - `list`: Filters subvolumes whose `last_snap_xid != 0`.
    - `create`: Atomically clones the source subvolume tree into a new snapshot root (RO or RW).
    - `delete`: Destroys snapshot root and invokes reaper.
- **Metadump & Image Tooling `vtxfs-image` (`tools/vtxfs-image/`)**:
  - `vtxfs-image create <image> -o <dump> [--sanitize] [--compress] [--full]`:
    - Dumps bootstrap blocks, two-level VAG directory, all 6 system trees, and subvolume trees by physical block (`pblk`).
    - `--sanitize`: Obfuscates file names and payloads in directory items, inline extents, and extended attributes while recomputing valid `BLAKE3-128` checksums.
    - `--full`: Traverses `VTX_TREE_REF` (`VTX_REF_FLAG_DATA`) to extract and dump all referenced data blocks (deduplicating shared extents).
    - `--compress`: Compresses each 4 KiB block with a self-contained sliding-window LZSS algorithm ($N=4096, F=18, T=3$), bounding worst-case expansion to $\le +12.5\%$ without external library dependencies.
  - `vtxfs-image restore <dump> <target>`:
    - Reads the `VTXMD21` dump header, provisions the target image, and restores all physical blocks, decompressing LZSS blocks transparently when `VTXFS_MD_FLAG_COMPRESSED` is set.
- **Mount Helper & Tool Scaffolds (`tools/`)**:
  - `mount.vtxfs`: Helper for `mount -t vtxfs`, parsing `ro`/`rw` and `subvol=` options into `mount(2)` flags and options string.
  - `vtxfs-balance`: Single-device verification and space usage report; checks `VTXFS_FEAT_INCOMPAT_MULTIDEV` (DEFERRED).
  - `vtxfs-defrag`: Extent fragmentation analyzer traversing `VTX_TYPE_EXTENT` items.
  - `vtxfs-send` & `vtxfs-receive`: Scaffold checking `VTXFS_FEAT_INCOMPAT_SEND_RECEIVE`.
  - `tools/Makefile`: Integrated build compiling all tools under `-std=c17 -O2 -Wall -Wextra -Werror -pedantic`.

## 15. Deferred (P2+)
`O_DIRECT` writes with engine checksums, `RENAME_EXCHANGE`, writable mmap over DAX, multi-device striping/mirroring, compression codecs (`zstd`/`lz4`), and VFS shrinker integration.


