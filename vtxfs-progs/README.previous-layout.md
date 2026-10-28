# VTXFS v2.1 — Complete P0 Core Engine, CoW B+Tree, Snapshots, Reaper, SFQ, CLI Tooling & Linux Kernel VFS Driver

VTXFS v2.1 implements the complete **P0 Single-Device Read/Write Profile** in portable dual-mode C (compiling cleanly under both userspace C11/C17 with `gcc`/`clang` `-Wall -Wextra -Werror -pedantic` and Linux kernel `kbuild`), together with the out-of-tree Linux kernel module (`vtxfs.ko`) providing the VFS driver and native kernel `ctree`/transaction subsystems.

## Repository Architecture

| File | Subsystem & Responsibility |
|---|---|
| `uapi/vtxfs.h` | Linux UAPI header: feature gates, capability flags, ioctl structs and command numbers |
| `include/vtxfs_disk.h` | On-disk v2.1 wire format: Superblock, Device Table, 8-slot CPD ring, VAG Directory, B+Tree nodes, Inodes, Extents, Dir/Xattr, Subvolumes, `vtx_ref_item`, `vtx_sfq_entry`, `vtx_reaper_job` |
| `include/vtxfs_validate.h` | Endian-aware key comparator, VBLK geometry, slotted node bounds, extent P0 payload and directory entry validators |
| `include/vtxfs_mem.h` | Kernel-mode in-memory data structures and layout definitions |
| `include/vtxfs_csum.h`, `core/vtxfs_csum.c` | Cryptographic & integrity primitives: Castagnoli `CRC32C`, RFC-compliant `BLAKE3-256`, domain-separated `BLAKE3-128`, `SipHash-2-4` directory/xattr hashing, and on-disk structure digest helpers |
| `core/vtxfs_compress.c`, `core/vtxfs_crypto.c` | P2 standalone data-transform foundations using real zstd/LZ4/zlib and ChaCha20-Poly1305/AES-256-GCM. On-disk feature gates remain disabled pending full extent and recovery protocols; see `docs/P2_STATUS.md`. |
| `include/vtxfs_leaf.h`, `core/vtxfs_leaf.c`, `core/vtxfs_leaf_balance.c` | Slotted leaf node operations (`init`, `find`, `insert`, `replace`, `delete`, `compact`) and byte-weighted sibling balancing (`split`, `split_insert`, `merge`, `rebalance`) |
| `include/vtxfs_internal.h`, `core/vtxfs_internal.c`, `core/vtxfs_internal_balance.c` | Slotted internal node operations and two-sibling balancing (`split`, `split_insert`, `merge`, `rebalance`) for both `VIRTUAL_OID` and `PHYSICAL_VBLK` modes |
| `include/vtxfs_btree_ro.h`, `core/vtxfs_btree_ro.c` | Separator-verified read-only multi-level B+Tree walker |
| `include/vtxfs_btree_rw.h`, `core/vtxfs_btree_rw.c` | Full multi-level CoW B+Tree write & cursor engine (`lookup`, `next`, `insert`, `replace`, `upsert`, `delete` across levels `0..7` with separator propagation, root split and root contraction) |
| `include/vtxfs_engine.h`, `core/vtxfs_vag.c` | Block I/O abstraction (sparse RAM disk + userspace file-backed block device + fault injection) and two-level physical VAG directory & bitmap allocator |
| `core/vtxfs_engine.c` | CoW transaction & ordered checkpoint engine (`data -> flush -> meta -> VAG -> flush -> CPD FUA + 64 MiB mirror`), bootstrap scanner, delayed-reference fixed-point drain (`VTX_TREE_REF`, `VTX_TREE_SFQ`), bitmap cache rebuild, and crash-resumable Reaper (`VTX_TREE_REAPER`) |
| `core/vtxfs_posix.c` | Subvolumes, RO/RW snapshots, POSIX operations (`mkdir`, `create`, `symlink`, `readlink`, `mknod`, `lookup`, `lookup_path`, `stat`, `chmod`, `chown`, `utimes`, `readdir`, inline & block `write`/`read` with per-block CRC32C verification, extent splitting, `truncate`, `unlink`, `rmdir`, `rename`, `xattr`/`listxattr`, `reflink`) and UAPI ioctl query handlers |
| `core/vtxfs_fsck.c` | Independent disk verifier (`vtx_fsck`) checking superblocks, CPD rings, VAG directories, B+Tree invariants, per-block data CRC32C, cross-tree `REF` reference counts, `SFQ` disjointness, and VAG bitmaps |
| `kernel/super.c`, `kernel/inode.c`, `kernel/file.c`, `kernel/xattr.c` | Linux VFS driver (`fs_context`, `super_operations`, `inode_operations`, `file_operations`, `address_space_operations`, `"user."` xattr handlers, `fallocate`, `copy_file_range`, writable `mmap`) |
| `kernel/locking.c`, `kernel/lru_cache.c`, `kernel/root-tree.c`, `kernel/root.c`, `kernel/dir-item.c`, `kernel/inode-item.c`, `kernel/disk-io.c`, `kernel/direct-io.c`, `kernel/fiemap.c`, `kernel/ioctl.c`, `kernel/export.c`, `kernel/writeback.c` | P0.6/P2-foundation kernel subsystems: hierarchical locking, metadata LRU cache, subvolume root cache (`mount -o subvol=`), page-cache-bypassing `O_DIRECT` read/write through the verified CoW engine, `RENAME_EXCHANGE`, `FS_IOC_FIEMAP`, NFS `export_operations`, batched `writepages`, centralized ioctl dispatch |
| `kernel/extent_io.c`, `kernel/ctree.c`, `kernel/extent-tree.c`, `kernel/inode-ext.c` | Native kernel `ctree` path/CoW operations, extent buffer management, `io_tree` extent state locking, `extent_map` cache, inline extents, and async delalloc range CoW |
| `kernel/trans.c`, `kernel/ordered-extent.c`, `kernel/delayed-inode.c`, `kernel/async-thread.c`, `kernel/compression.c`, `kernel/uuid-tree.c`, `kernel/orphan.c`, `kernel/print-tree.c`, `kernel/reaper.c`, `kernel/backref.c`, `kernel/sysfs.c`, `kernel/free-space-cache.c` | Native kernel transaction state machine, ordered extents, delayed directory items, async CoW workqueue, compression policy, UUID tree, orphan cleanup, tree printer, kernel reaper kthread, backref walker, `/sys/fs/vtxfs` sysfs exposure, and free-space RB-tree cache |
| `kernel/vag.h`, `kernel/vag-alloc.c`, `kernel/omap.c`, `kernel/mount-native.c`, `kernel/truncate.c`, `kernel/scrub.c`, `kernel/selftest.c`, `kernel/kunit/vtxfs_ctree_test.c` | Parts 6 & 7 kernel-native subsystems: authoritative VAG bitmap allocator (`vtxfs_alloc_ops`), OMAP view (`vtxfs_omap_view`), native mount path (CPD ring + 64 MiB mirror scan), native drop-extents & truncate, background scrub worker (`vtx_scrub_state`), in-RAM multi-level ctree self-test (`vtxfs_run_selftest`), and in-kernel KUnit test suite |
| `kernel/ctree_balance.c`, `kernel/ctree_compat.c`, `kernel/ramdisk.c`, `kernel/delalloc.c`, `kernel/selftest_e2e.c` | Part 8 P2 consolidation: sibling merge/rebalance and root contraction on delete, OMAP CoW propagation via `resolve_root`, engine fsck disk compatibility (`ROOT` flags, `delayed_refs`), bio-based in-kernel RAM disk (`vtxfs_ramdisk_create/destroy`), real delayed allocation with `vtxfs_ordered_extent` lifecycle, and kernel-side end-to-end self-test (`vtxfs_e2e.ko`) |
| `kernel/ctree_locking.c`, `kernel/ctree_export.c`, `kernel/cpd.c`, `kernel/extent-item.c`, `kernel/sfq.c` | Parts 9 & 10 kernel consolidation: descending lock-coupling (`vtxfs_search_slot_locked`, `vtxfs_release_path_locked`), sibling rebalance on asymmetric insert (`vtxfs_balance_after_insert`), consolidated ctree accessors, authoritative dual-mirror CPD publish (`vtxfs_publish_cpd`), extent item CRC builder (`vtxfs_build_extent_item`), Safe-Free Queue promotion at commit (`vtxfs_sfq_promote`), and complete super lifecycle teardown (`vtxfs_mount_native_teardown`) |
| `tools/mkfs_vtxfs.c`, `tools/fsck_vtxfs.c`, `tools/vtxfs_inspect.c`, `tools/vtxfs_ioctl_test.c` | Standard userspace utilities (`mkfs.vtxfs`, `fsck.vtxfs`, `vtxfs-inspect`, `vtxfs-ioctl-test`) |
| `tools/vtxfs/` (`main.c`, `common.c`, `subvolume.c`, `snapshot.c`) | Unified userspace CLI `vtxfs` (`info`, `sync`, `subvolume`, `snapshot`) with subvolume name resolution via `VTX_FT_SUBVOL` |
| `tools/vtxfs-image/` (`main.c`, `common.c`, `sanitize.c`, `image-create.c`, `image-restore.c`) | Metadump & image tool `vtxfs-image` (`create`/`restore` with self-contained sliding-window LZSS compression and `--sanitize` obfuscation) |
| `tools/mount.vtxfs.c`, `tools/vtxfs-balance.c`, `tools/vtxfs-defrag.c`, `tools/vtxfs-send.c`, `tools/vtxfs-receive.c` | Mount helper (`mount.vtxfs`) and management scaffolds (`vtxfs-balance`, `vtxfs-defrag`, `vtxfs-send`, `vtxfs-receive`) |



## VTXFS Files — GUI thử nghiệm trên ổ RAM ảo

Giao diện GTK4 dạng Files dùng trực tiếp VTXFS userspace engine, hỗ trợ duyệt thư mục, tạo tệp/thư mục, sửa tệp văn bản, đổi tên và xóa. Tab **Block Bitmap** hiển thị trạng thái trống/đã cấp phát từ VAG bitmap thật, kèm tổng quan, trang, zoom, vị trí VBLK/PBLK và đánh dấu block thay đổi. Mặc định tạo image **256 MiB ở `/dev/shm` (tmpfs)**; có tùy chọn image riêng 512 MiB hoặc 1024 MiB, không mount hoặc định dạng ổ vật lý. Chạy:

```sh
sh tools/run-vtxfs-files.sh
```

**Giới hạn giao diện 64 KiB đã được loại bỏ:** editor tối đa 4 MiB, nhập file nhị phân dung lượng lớn bằng đường ghi `put-stdin` 64 KiB/chunk và checkpoint định kỳ. Thử nghiệm ghi/đọc 16 MiB đã đạt; liệt kê thư mục hỗ trợ trên 64 file. Để tạo một volume RAM riêng 512 MiB (không format volume hiện tại), chạy `VTXFS_RAM_MIB=512 sh tools/run-vtxfs-files.sh`.

**Bảng Nhật ký hoạt động:** ở bên phải danh sách file, cập nhật lệnh chạy, kết quả, tiến độ từng MiB, lỗi, có nút ẩn/hiện, xóa, sao chép; không ghi nội dung file hoặc ghi log ra ổ vật lý.

**Menu chuột phải kiểu GNOME Files:** mở file/thư mục, copy/cut/paste, nhân bản, sao chép tới, di chuyển tới, đổi tên an toàn, nén ZIP (file ≤8 MiB), chuyển vào thùng rác VTXFS, khôi phục về thư mục gốc, xóa vĩnh viễn, thuộc tính, tìm kiếm và phím tắt. Mọi thao tác chỉnh sửa chỉ tác động lên image VTXFS RAM.

Chi tiết, giới hạn và kiểm thử: [docs/GUI_RAM_FILES.md](docs/GUI_RAM_FILES.md).

## Build & Run All Tests

```sh
cd /home/haui/Music/Downloads/VTXFS
bash tests/run.sh

# Build the complete out-of-tree Linux kernel filesystem module (vtxfs.ko):
make -C /lib/modules/$(uname -r)/build M="$PWD" modules

# Build the end-to-end kernel self-test module (vtxfs_e2e.ko):
make -C /lib/modules/$(uname -r)/build M="$PWD" CONFIG_VTXFS_E2E_TEST=m modules
make -C /lib/modules/$(uname -r)/build M="$PWD" clean

# Dual-mode Linux kernel core build probe (tests/kbuild/vtxfs_probe.ko):
make -C /lib/modules/$(uname -r)/build M="$PWD/tests/kbuild" modules
make -C /lib/modules/$(uname -r)/build M="$PWD/tests/kbuild" clean
```

## Test Suite Coverage

| Test Binary | Coverage |
|---|---|
| `tests/test_abi.c` | Static ABI struct size/offset assertions, 100,000 deterministic key ordering cases, VBLK geometry, feature gates |
| `tests/test_validation.c` & `tests/test_validation_extra.c` | Slotted node validation, extent P0 invariants, malformed/overlapping slot rejection, reference bounds |
| `tests/test_csum.c` | Official `CRC32C`, `BLAKE3-256` (multi-chunk tree hashing), domain-separated `BLAKE3-128`, `SipHash-2-4` vectors, and structure checksum tamper detection |
| `tests/test_leaf.c` & `tests/test_leaf_balance.c` | 20,000-op differential leaf fuzzing, byte-weighted leaf splits, merges, and sibling rebalancing |
| `tests/test_internal.c` & `tests/test_internal_balance.c` | Internal node operations and two-sibling splits, merges, and rebalancing (`VIRTUAL_OID` & `PHYSICAL_VBLK`) |
| `tests/test_wire_endian.c` | Wire-format endianness and aliasing safety |
| `tests/test_btree_ro.c` & `tests/test_btree_rw.c` | Multi-level B+Tree lookups, forward cursor iteration, root splits to depth 3 (`level 2`), and root contraction back to `level 0` |
| `tests/test_engine_posix.c` | End-to-end `mkfs`, `mount`, POSIX directories/files/symlinks/mknod/xattrs/listxattr, inline-to-extent promotion, middle-of-extent overwrite splitting, data `CRC32C` bit-flip detection (`-EUCLEAN`), `truncate`, `reflink`, RO/RW snapshots, Reaper across remount, SFQ promotion, bitmap rebuild, and `vtx_fsck` |
| `tests/test_crash_fault.c` | Ordered commit fault injection across all 7 I/O stages (`DATA_WRITE`, `DATA_FLUSH`, `META_WRITE`, `VAG_WRITE`, `META_FLUSH`, `CPD_PRIMARY`, `CPD_MIRROR`), 64 MiB mirror Superblock/CPD fallback, and checkpoint ring rollback |
| `tools/vtxfs/` (in `tests/run.sh`) | Unified CLI testing (`info`, `sync`, subvolume `create`/`list` with `VTX_FT_SUBVOL` directory names, snapshot `create`/`list`, scaffolds) |
| `tools/vtxfs-image/` (in `tests/run.sh`) | End-to-end metadump round-trip (`mkfs -> write data -> dump(--full) -> restore -> fsck`) in both uncompressed and self-contained LZSS compressed modes |
| `tests/test_uapi_cpp.cpp` | C++17 UAPI compatibility and ABI checks |

## Theo dõi toàn bộ hàm kernel (opt-in)

Đã bổ sung `VTXFS_TRACE_ENTER()` vào 401 hàm trong 51 file C và
10 inline helpers thuộc 2 header của `kernel/` (tổng cộng 411), cùng các trace sự kiện
mount, VAG allocation, extent, CoW metadata, commit, file I/O. Debug logs
mặc định **tắt** và giới hạn tốc độ khi bật; không chạy trên ổ vật lý.
Danh sách hàm, symbol có mặt trong module, hướng dẫn dynamic-debug/ftrace:
[docs/KERNEL_FUNCTION_TRACING.md](docs/KERNEL_FUNCTION_TRACING.md).

```sh
# Không cần quyền root: kiểm kê C source và symbol của module đã build
tools/trace-vtxfs.sh inventory

# Nếu module VTXFS đã được nạp trên môi trường thử nghiệm an toàn:
sudo tools/trace-vtxfs.sh enable-file super.c
sudo tools/trace-vtxfs.sh follow
sudo tools/trace-vtxfs.sh disable
```

Các trạng thái "chưa thấy hàm chạy" chỉ phản ánh workload đã ghi log, không
chứng minh hàm đó không tham gia hoặc có thể xóa khỏi dự án.

## VTXFS Terminal File Manager

Sử dụng `tools/vtxfsctl` để quản lý file/thư mục trực tiếp trong image
VTXFS RAM: create/open/edit, rename, copy/cut/paste, move, search/sort/filter,
stat/properties, symlink/shortcut, readonly/hide, ZIP/unzip, disk-space,
trash/restore đúng đường dẫn gốc và permanently-delete/empty-trash có xác nhận.
Công cụ không cần GTK, không format ổ vật lý. Hướng dẫn và lưu ý an toàn:
[docs/TERMINAL_FILE_MANAGER.md](docs/TERMINAL_FILE_MANAGER.md).

```sh
IMG="/dev/shm/vtxfs-files-$(id -u)-512MiB.img"
tools/vtxfsctl "$IMG" disk-space
tools/vtxfsctl "$IMG" search .txt --in /
```


## VTXFS Backref kernel

Backref metadata và helper nội bộ theo VTXFS ABI trong `kernel/backref.c`,
`kernel/vtxfs_backref.h`. Các thao tác phụ thuộc Btrfs-only item trả
`-EOPNOTSUPP`, không giả tạo owner refs.
Xem [docs/KERNEL_BACKREF_API.md](docs/KERNEL_BACKREF_API.md).


## Tiếp thu kiểm tra metadata/extent từ Btrfs/APFS (2026-10-26)

Đã đối chiếu nguồn Linux Btrfs `disk-io`, `fiemap`, `direct-io`,
`free-space-tree`, `dir-item` và Linux APFS RW `btree`, `node`,
`spaceman`, `extents`, `dir`, `inode`. Áp dụng **safety hardening** vào
VTXFS: metadata node checksum/geometry/identity; cache theo checkpoint,
FIEMAP clipping, DIO alignment/partial-copy, SFQ 64-bit range guard
trong fsck và VAG bitmap atomic preflight trước free. Không đổi on-disk ABI.
Thử nghiệm GCC/Clang và image `/dev/shm`; kernel KUnit mới chỉ compile.
Xem [docs/BTRFS_APFS_ADOPTION_2026-10-26.md](docs/BTRFS_APFS_ADOPTION_2026-10-26.md).


## Lộ trình triển khai tổng thể M0–M11

Bản master roadmap toàn diện gồm 12 mốc, 34 PR nhỏ, các ràng buộc ABI, on-disk migration,
kernel runtime, REF/SFQ/backref, recovery, I/O, send/receive, defrag, multidevice và release gates:
[docs/VTXFS_MASTER_ROADMAP.md](docs/VTXFS_MASTER_ROADMAP.md).


## VTXFS VM crash/mirror & multithread stress (2026-10-26)

Bổ sung kernel QEMU power-cut-after-fsync, CPD/Superblock mirror fallback, dual CPD fail-closed, concurrent 4-thread/32-file workload, mmap+msync, readonly remount byte-verification. Root CoW dynamic OID validation đã sửa; M4 persisted reverse-backref vẫn chưa triển khai.

Đọc [docs/M2_VM_CRASH_MIRROR_AND_STRESS_2026-10-26.md](docs/M2_VM_CRASH_MIRROR_AND_STRESS_2026-10-26.md), chạy `tools/vtxfs-dev-check quick`.


## VTXFS POSIX P0/P1/P2 repairs (2026-10-26)

Opened-FD unlink and rename-over-target now use durable orphan markers and `evict_inode`, with VM-kill recovery regression. Multi-volume sysfs and `statfs` have VM coverage; true hardlink/nlink supported in the VFS and `vtxfsctl hardlink` CLI. The CLI blocks edits on 0444 files. `fallocate(mode=0)` supports eager CRC-verified 4 KiB-aligned zero-backed allocation up to 1 MiB/call; KEEP_SIZE/punch-hole remain unsupported. A single 8 MiB tmpfs checkpoint benchmark showed 1.44x higher write throughput with 1 MiB checkpoint batching. Tests use disposable QEMU `/dev/shm` volumes only. See `docs/VTXFS_P0_P2_POSIX_REPAIR_2026-10-26.md` for exact scope and remaining production blockers.


## OMAP / Snapshot / Reflink — giai đoạn sửa an toàn 2026-10-26

Đã đảo thứ tự delayed ref trong reflink/split extent để tránh đếm thiếu; snapshot không ghi cờ SHARED vào nguồn RO, VFS mount RO snapshot thực sự chỉ đọc; OMAP v1 kiểm tra lỗi giá trị ánh xạ như EUCLEAN; fsck dùng bộ đếm ref/subvolume cấp phát động thay cho giới hạn 4096/128. Kernel copy_file_range hiện dùng sao chép byte qua splice (đã bổ sung splice_write) thay cho clone extent đến khi remap/pagecache locking được cài đầy đủ. Xem `docs/VTXFS_OMAP_SNAPSHOT_REFLINK_STAGE1_2026-10-26.md` và 2 bài test mới `tests/test_refcount_snapshot_safety.c`, `tests/test_fsck_dynamic_subvols.c`. OMAP v2, OMAP DUP/rebuild và snapshot O(1) chưa được triển khai; không đổi on-disk format.

## Benchmark có xác minh tính đúng đắn (2026-10-26)

Chạy `python3 tests/benchmark_vtxfs_verified.py --help` để xem benchmark fio fail-closed. Bộ đo cần thiết bị scratch riêng, ghi đủ byte + fsync, cold remount + fsck, fio CRC32C, SHA-256, thống kê I/O block và kiểm tra lỗi dmesg. Chưa chạy power-cut/dm-log-writes thì trạng thái là `INCOMPLETE_POWER_CUT` (không được công nhận là benchmark độ bền). Tham khảo `docs/VTXFS_VERIFIED_BENCHMARK_2026-10-26.md`. CRC32C trong kernel đã chuyển sang thư viện Linux, không thay checksum trên đĩa; gộp BIO vẫn chưa triển khai.

## BIO batching / readahead / group commit và dm-log-writes (26/10/2026)

Module kernel đã có BIO 128 KiB đồng bộ với bounce pages, readahead 128 KiB qua đường CRC, và group commit bảo thủ theo `op_seq`. QEMU kiểm tra đọc lại byte-exact sau DONTNEED và fsync sạch không tạo sector ghi mới. `tests/benchmark_dm_log_writes.py` là bộ replay tới mark fsync và mọi điểm FUA, nhưng trên máy hiện tại chưa chạy được vì UID thường không có quyền device-mapper và thiếu `replay-log`; không có tuyên bố PASS mất điện thật. Xem `docs/VTXFS_BIO_READAHEAD_GROUP_DMLOG_2026-10-26.md`.
