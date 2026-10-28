# VTXFS — Lộ trình triển khai toàn diện (Master Roadmap)
**Ngày lập:** 2026-10-26  
**Định hướng:** Linux CoW filesystem độc lập, tương thích ngược có kiểm soát với VTXFS v2.1, tham khảo cơ chế từ Btrfs và driver APFS Linux RW.
**Phạm vi triển khai:** `/home/haui/Music/Downloads/VTXFS`; sử dụng terminal; dữ liệu thử ở `/dev/shm` hoặc máy ảo chuyên dụng.
**Chính sách:** Đây là kế hoạch, không có nghĩa các tính năng chưa triển khai đã sẵn sàng. Không tự chuyển format hoặc kích hoạt on-disk feature bits.

## 0. Đích đến và nguyên tắc

**Đích đến:** đưa VTXFS từ portable P0 single-device engine và module kernel build được thành hệ thống có bảo đảm rõ ràng về data integrity, crash recovery, POSIX semantics, snapshot correctness, observability, performance và khả năng nâng cấp định dạng. Không lấy số lượng file/hàm so với Btrfs/APFS làm KPI.

**Không thay đổi:** nền tảng `Superblock ring + CPD + VAG + OMAP view + CoW B+Tree + REF + SFQ/Reaper`. Phần mở rộng chỉ được tiến hành với capability negotiation, feature flag có phiên bản, migration/rollback và fsck tương ứng.

**Nguồn tham khảo đã đọc:**
- Btrfs: `/home/haui/Videos/linux-master/fs/btrfs/{disk-io.c,direct-io.c,dir-item.c,discard.c,fiemap.c,file-item.c,file.c,inode-item.c,fs.c,fs.h,free-space-tree.c}`.
- APFS Linux RW *third-party* (không phải mã Apple chính thức): `/home/haui/Videos/linux-apfs-rw-master/{btree.c,node.c,spaceman.c,extents.c,inode.c,dir.c,file.c}`.
- Nguồn VTXFS: `include/vtxfs_disk.h`, `include/vtxfs_validate.h`, `uapi/vtxfs.h`, `core/`, `kernel/`, `tools/`, `tests/`.
- Chỉ tiếp thu *mô hình thiết kế*, không sao chép khối mã Btrfs/APFS hoặc nhập metadata của hai định dạng khác. Xem giấy phép trước nếu có nhu cầu sử dụng lại mã thực sự.

**Trạng thái quan sát đến ngày lập** (là nền ban đầu, không phải chứng chỉ production):
1. P0 portable C và CLI single-device có CoW, checkpoint, snapshot/reflink, REF/SFQ/Reaper, CRC + fsck; `tests/run.sh` GCC/Clang pass, 7 fault stages.
2. `vtxfs.ko` build thành công; *chưa* có bằng chứng kiểm thử mount/runtime đầy đủ của kernel-native VFS/ctree/I/O ở lượt kiểm tra gần nhất.
3. `kernel/backref.c` có 61 khai báo API/symbol nhưng nhiều tính năng Btrfs-only trả `-EOPNOTSUPP`; owner enumeration chưa đủ mọi root/snapshot trên đĩa.
4. `kernel/free-space-cache.c` có RB cache in-memory, load on-disk mới là scaffold; `vtxfs-defrag` chỉ thống kê; `vtxfs-send/receive` là scaffold; `vtxfs-balance` chưa relocation.
5. Codec nén/AEAD đã có primitives, nhưng data-transform on-disk và nonce/key lifecycle chưa cho phép bật feature. Phần metadata checksum/cache, FIEMAP clipping, DIO guard, VAG double-free và SFQ bounds vừa được harden và build/test tĩnh.
6. Kernel KUnit VAG/backref đã **biên dịch**, chưa chứng minh chúng đã được thực thi. Mỗi mục mới phải có bằng chứng riêng.

### Định nghĩa trạng thái bắt buộc

- **S0 Thiết kế:** RFC, schema/contract, threat model, failure modes.
- **S1 Có mã:** đã commit/ghi source và có unit test.
- **S2 Biên dịch:** gcc + clang, kbuild, static audit thành công.
- **S3 Runtime cô lập:** chạy có bằng chứng (logs/exit code/fsck) trên RAM/loop trong VM.
- **S4 Fault-safe:** crash/fault injection, recovery, memory-safety/concurrency và dữ liệu đối chiếu đạt.
- **S5 Sẵn sàng theo profile:** tài liệu, công cụ sửa chữa, format upgrades, gating, matrix kernel và phát hành thử nghiệm.
- Tuyệt đối không gọi S2 là S4; không đổi capability/`ALLOWED_INCOMPAT` chỉ vì có hàm/API hoặc test mock.

## 1. Cấu trúc chương trình và thứ tự ưu tiên

| Mốc | Chủ đề | Phụ thuộc cứng | Ưu tiên | Bằng chứng kết thúc |
|---|---|---|---|---|
| **M0** | Baseline, hợp đồng dữ liệu, CI/test harness | hiện trạng | P0 | build + test reproducible; inventory/known risks |
| **M1** | Kernel runtime an toàn + VFS/locking | M0 | P0 | mount/unmount trong VM, workload POSIX, KUnit thực thi |
| **M2** | Checkpoint, CoW/OMAP, recovery, integrity | M1 | P0 | crash matrix, dual-root verification, no stale metadata |
| **M3** | Allocator, SFQ, REF, free-space cache/rebuild | M2 | P0 | no double allocation; rebuild kiểm tra được |
| **M4** | Backref đầy đủ qua mọi snapshot/root | M2 + M3 + format RFC | P0 | reversible owner map; refcount == owner sum |
| **M5** | POSIX, directory/inode, locking, VFS coherency | M1 + M4 cho shared ops | P1 | xfstests subset + race coverage |
| **M6** | Data path, buffered/direct I/O, delalloc/ordered extents | M2 + M3 + M5 | P1 | data durability + FIEMAP + benchmarks |
| **M7** | Scrub/fsck repair, backup/recovery, metadata redundancy | M4 + M6 | P1 | single corruption repaired safely, unrepairable => RO/error |
| **M8** | Snapshot/reflink, send/receive, quota, lifecycle | M4 + M6 + M7 | P1 | snapshot consistency / resume stream / quota reconciliation |
| **M9** | Defrag, discard, balance, allocator/performance tuning | M3 + M4 + M8 | P2 | relocation crash safe; benchmark non-regression |
| **M10** | Compression/encryption, multidevice/mirror, advanced features | M2–M9 tùy feature | P2/P3 | per-feature RFC, migration, fault & recovery matrix |
| **M11** | Release engineering, audit, compatibility, operations | tất cả feature mục tiêu | P0 xuyên suốt | release candidate gated; documented limits |

**Điểm cần chú ý:** M5 (POSIX hardening) có thể phát triển song song với M3/M4 đối với các đường không dùng shared extent. M8 giao thức send/receive có thể được thiết kế sớm nhưng chưa bật trước khi owner/recovery đầy đủ. M10 là nhóm *nhiều RFC riêng biệt* chứ không phải một lần bật tất cả.

## 2. M0 — Baseline, kiểm kê và hệ thống thử nghiệm

**Mục tiêu:** mọi thay đổi phải so sánh được với baseline, tái hiện được lỗi và không phá format hiện có.

**Hạng mục:**
- Chốt baseline source: hash commit hoặc source snapshot; bảng các tính năng `supported/partial/scaffold/deferred` từ implementation + call graph, thay vì dựa README.
- Ghi contract on-disk v2.1: block geometry, key order, OMAP identity, ownership refcount, SFQ safe XID, CPD durable cutover; test static_assert + golden images.
- Chuẩn hóa `make check` / `make check-kernel` / `make check-fuzz` / `make check-compat`; binary test reports theo compiler/kernel/seed/feature bits.
- CI matrix: GCC, Clang, sanitizers userspace (ASan/UBSan), 2 cấu hình tối ưu (debug/release); kernel upstream + LTS được hỗ trợ theo danh sách phiên bản cụ thể, không tuyên bố hỗ trợ mọi kernel.
- Thư mục ảnh test chỉ thuộc `/dev/shm` hoặc loop image riêng trong VM; test scripts in tên image, tự cleanup, không gọi `mkfs` lên thiết bị thật; có guard chống symlink/block device.
- Xây command mô phỏng crash ở từng điểm: DATA_WRITE → DATA_FLUSH → META_WRITE → VAG_WRITE → META_FLUSH → CPD_PRIMARY → CPD_MIRROR, thêm drop I/O/short read/bitflip/ENOMEM.
- Thiết lập số liệu chuẩn: thời gian mount, throughput 4 KiB/1 MiB/sequential/random, latency commit/snapshot/reaper, dung lượng metadata per 1k/100k files; lưu JSON/CSV kết quả.

**File và sản phẩm:** `tests/run.sh`, `tests/test_crash_fault.c`, `tests/kbuild/`, `kernel/kunit/`, `docs/P0_CONTRACTS.md`, `docs/ABI_V21_FREEZE.md` (mới), `tests/golden/` (mới), pipeline CI.
**Gate M0:** cùng nguồn build/tái tạo test trên cả hai compiler; golden v2.1 mount read-only/read-write trên engine; kết quả lỗi tái hiện bằng seed; không còn bài test chỉ in PASS nhưng không assert.

## 3. M1 — Kernel VFS runtime: tính đúng trước tốc độ

**Mục tiêu:** chứng minh `vtxfs.ko` hoạt động an toàn, tách biệt compile và runtime.

**Các phần:**
- Tạo kernel VM/QEMU snapshot với `ramdisk`/loop file mới được tạo bên trong guest, chạy `insmod`, `mount -t vtxfs`, `umount`, `fsck.vtxfs`; tuyệt đối không mount/format phân vùng host.
- Chạy KUnit thật: `vtxfs_ctree_test`, backref, VAG double-free; lưu TAP/KTAP log, exit status, kernel dmesg. Compile-only không tính thành công.
- Mở file, read/write/sync, rename, unlink, rmdir, symlink, xattr, chmod/chown, timestamp, mmap, `fallocate`, `copy_file_range`, NFS export (nếu bật), FIEMAP, O_DIRECT, freeze/remount/umount.
- Đối chiếu byte, inode nlink/size/mtime, directory cookies, sparse/EOF; so sánh kết quả VTXFS engine và mounted driver trên cùng tình huống.
- Kiểm tra VFS/page-cache coherency; bỏ trang cache đúng sau direct-write/truncate; `mmap` với snapshot/reflink không được viết nhầm shared block.
- Lockdep, KASAN/KCSAN trong VM; buộc tháo mount, giữ file descriptors, I/O bị hủy và lỗi allocate; chứng minh teardown không UAF/memory leak.
- Quy tắc: module native và portable engine phải có **một transaction owner duy nhất** trên một mount; lộ trình chuyển dần, không để hai pipeline tự publish CPD độc lập.

**File:** `kernel/{super,inode,file,dir-item,locking,direct-io,fiemap,mount-native,extent_io}.c`, `kernel/kunit/`, VM harness mới.
**Gate M1:** ≥ một bộ VFS smoke hoàn chỉnh chạy trong VM, từng thao tác trả errno đúng, kernel log sạch về oops/lockdep/KASAN, remount/fsck sạch sau mỗi workload, biết cách tái hiện failure.

## 4. M2 — Tăng độ tin cậy CoW, checkpoint và recovery

**Mục tiêu:** CPD chỉ công bố metadata bền vững, không mount trạng thái bị ghi dở.

- Rà soát lớp xác minh node: checksum + `self_vblk` + `owner_id` + tree_type/level + XID + key ranges/separator + parent hint XID; validate trước khi cache và trước khi dereference.
- Thống nhất `kernel/disk-io.c` cache với `kernel/extent_io.c`; cache key phải xét incarnation (VBLK, checkpoint generation, owner/identity khi cần); invalidate stale entries khi CoW/commit/snapshot.
- Tất cả allocation/reuse tuân `REF -> SFQ -> safe_xid -> VAG`; giữ pinning cho in-flight reader, mmap, reflink, checkpoint đang tham chiếu; không cho free nhầm block.
- Chứng minh ordering qua fault injection ở **mọi** bước nêu tại M0, thêm lỗi phát hành mirror, lỗi flush/FUA, corrupt selected root, power cut trước/sau CPD primary và recovery fallback.
- Cross-check reader/kernel native với portable engine ở các bản checksum và node schema; không để kernel chấp nhận layout mà fsck bác bỏ hoặc ngược lại.
- Định nghĩa chính xác freeze/unfreeze, fsync/fdatasync/syncfs, superblock fallback, readonly remount after unrecoverable metadata failure, rollback-to-last-committed XID.
- Tách trace events có correlation ID cho transaction/XID, allocation_id, VBLK, errno; trace opt-in và rate-limit, không rò file content/keys.

**File:** `core/{vtxfs_engine,vtxfs_btree_rw,vtxfs_vag}.c`, `kernel/{disk-io,ctree,extent_io,cpd,trans,omap}.c`, `core/vtxfs_fsck.c`.
**Gate M2:** 100% điểm cắt crash dự kiến có một trong 2 kết quả duy nhất: checkpoint trước nguyên vẹn hoặc checkpoint mới nguyên vẹn; không có torn root hợp lệ, không double-allocated VBLK; fsck ghi nhận đúng corruption; fallback mirror có test thực.

## 5. M3 — Free-space, REF, SFQ, Reaper: một nguồn sự thật

**Mục tiêu:** allocator vẫn đúng sau vô hạn chu kỳ tạo/xóa/snapshot/crash.

- VAG bitmap và REF/SFQ là **authority**; `FREE_SPACE` chỉ cache có thể tái tạo, không giải phóng từ entry cache mà chưa xác nhận authoritative state.
- Hoàn thiện `kernel/free-space-cache.c`: coalescing/splitting sorted extents, kiểm tra overlap/adjacency/overflow, eviction, cache generation XID, invalidation khi commit; cache lookup **không** đồng nghĩa reservation.
- Thực hiện rebuild deterministically từ VAG bitmap và cross-check với REF ∪ SFQ và reserved blocks, detect mismatched free counts; nếu bất nhất thì RO/error/repair offline, không tự sửa âm thầm.
- Đưa reserve/delalloc-space accounting vào một contract: bytes_reserved, bytes_pinned, bytes_reusable, bytes_shared, bytes_snapshot; hard ENOSPC vs temporary EDQUOT/pressure rõ ràng.
- Reaper resume cursor sau crash, giới hạn công việc từng transaction, xử lý unlink/open inode, snapshots, delayed refs, inode orphan; không thu hồi block đang pin.
- Stress: fill disk, alternate allocate/free, large sparse files, 100k dirents, 1k snapshots (khi owner infrastructure xong), pinned refs, simultaneous create/delete; compare bitmap/refcount mỗi commit.
- Cân nhắc FST persisted dạng extent-run/bitmap giống Btrfs nhưng chỉ khi có `RO_COMPAT` và reader cũ xử lý đúng; thử nghiệm in-memory trước on-disk.

**File:** `kernel/{vag-alloc,free-space-cache,sfq,reaper,delalloc,extent-tree}.c`, `core/{vtxfs_vag,vtxfs_engine,vtxfs_fsck}.c`.
**Gate M3:** không double-free/double-allocate/early reuse; toàn bộ refcount khớp; không cache entry stale sau remount; kết quả fsck độc lập và counters khớp qua ENOSPC/fault; có test KUnit bitmap/free thực thi.

## 6. M4 — Persistent reverse-backref theo allocation_id (RFC format)

**Mục tiêu:** trả lời đúng *mọi* tham chiếu của allocation, kể cả snapshot/root chưa load; không dựa `owner_hint` hoặc `fs->roots` in-memory.

**Trước khi lập trình phải khóa RFC:**
- Thiết kế backref keys độc lập Btrfs, hướng kiến nghị `(allocation_id, reference-kind, subvol_id/root_oid, inode_id, file_offset, parent metadata OID, view_xid)` nhưng đây là **bản đề xuất**, không phải định dạng được duyệt.
- Data owner refs cho file extents; metadata parent/root refs cho CoW B+Tree; quan hệ snapshot view phải bao gồm root bất hoạt/cached và OMAP view tại checkpoint.
- Quy định direct vs indirect, sharing, subvolume deletion, rename/reflink/truncate/snapshot và delayed ref delta, phân biệt owners của dữ liệu với node metadata.
- Transactional update cùng CPD: insert/delete owners + refcount + SFQ trong một ordering an toàn; invariant `REF.refcount == số owners hợp lệ` theo định nghĩa sharing (không được đếm lặp do aliases).
- On-disk mở rộng có feature incompat **mới** (không tái dùng bit đã deferred nếu semantics không trùng), schema endian/version, chống overflow; old v2.1 reader phải từ chối RW khi gặp incompat chưa biết.
- Migration tool `vtxfs-upgrade --dry-run / --apply` (đề xuất): backup toàn bộ, scan tất cả roots, tạo owner index, fsck, transactional switch, resume/rollback sau crash; không tự upgrade khi mount.
- API `find_all_leafs`, `find_all_roots_safe`, `resolve_indirect_refs`, `iterate_extent_inodes`, `inode_to_path` chỉ được chuyển từ diagnostic sang authoritative khi khớp toàn bộ snapshot graph.
- Full fsck cross-tree: owner record, extent item, REF, SFQ, OMAP view, root tree; injection missing/duplicate/cyclic backrefs và crashes trong upgrade.

**File:** `include/vtxfs_disk.h` (chỉ sau RFC), `uapi/vtxfs.h`, `core/vtxfs_engine.c`, `kernel/{backref,extent-tree,root,omap,trans}.c`, `core/vtxfs_fsck.c`; tools upgrade mới.
**Gate M4:** mọi allocation tìm được toàn bộ owner còn sống, bao gồm snapshot đã unmount; refcount reconciliation chính xác, không leak/early-free; migration và rollback được crash-test; giữ reader v2.1 an toàn. Không đạt gate => vẫn dùng backref như chẩn đoán.


## 7. M5 — POSIX semantics, directory/inode và đồng bộ VFS

**Mục tiêu:** mọi syscall được quảng bá hỗ trợ phải đúng errno, atomicity và tương tác VFS, kể cả khi có race.

- Kiểm thử đồng thời `create`, `link` (nếu bổ sung hardlink), `unlink`, `rename`, `RENAME_NOREPLACE`, `RENAME_EXCHANGE`, `rmdir`, `fsync`, `open-after-unlink`, `O_TRUNC`. Chỉ bật hardlink sau khi `nlink`/`INODE_REF`/gc/rename fsck đồng nhất.
- Tên file Unicode: byte-length giới hạn, normalized key policy cố định, tránh âm thầm case folding; hash collision16 trong VTXFS phải có property tests/forced collisions.
- Directory `DIR_ITEM` ↔ `DIR_INDEX` ↔ `INODE_REF` được cập nhật transactionally; xóa/snapshot/recovery không để dirent mồ côi hoặc parent cycle.
- Metadata `uid/gid/mode`, POSIX ACL (nếu thêm), permission enforcement qua VFS + userspace engine, mknod, xattr namespaces, timestamps và mount options.
- Hardlink count + delayed orphan cleanup; handle còn mở sau unlink, truncate/open races, sparse hole + SEEK_HOLE/SEEK_DATA, statx, quotactl khi bật.
- Rà soát `copy_file_range`/reflink xattr preservation, `fallocate` modes (punch-hole/zero-range) và quota sharing; return `-EOPNOTSUPP` thay vì thực thi một phần không atomic.
- Harness xfstests subset có danh sách pass/fail cụ thể (generic/ và btrfs-inspired semantics đã thích ứng), fsstress/fio random + dmesg lockdep; fuzz API POSIX theo trạng thái.

**File:** `core/vtxfs_posix.c`, `kernel/{inode,file,dir-item,inode-item,orphan,truncate,xattr,export}.c`, `tools/vtxfsctl.py` (chỉ hỗ trợ CLI, không thay POSIX test).
**Gate M5:** syscall matrix rõ ràng, no stale inode dentry/page cache, no directory cycles, invalid input atomic rollback; no unexpected ENOSPC or `-EIO`; crash-point replay cho directory operations.

## 8. M6 — High-performance I/O giữ nguyên CoW và checksums

**Mục tiêu:** tăng throughput/latency mà không có write path nào vượt REF/SFQ/CPD.

- **Read path:** review folio readahead, checksum-on-read, extent-map lookup invalidation theo snapshot/XID, concurrent read/truncate/reflink, buffer lifetime.
- **Buffered write:** delalloc reservations, dirty-page accounting, ordered extent completion, writeback error propagation, `errseq_t` semantics, fsync range.
- **Direct I/O:** alignment theo `bdev_logical_block_size`, page cache flush/invalidate trước/sau, unaligned policy minh bạch, partial user copies; không raw-write đè CoW. Kiểm chứng byte exact across 4 KiB, 64 KiB, VAG edges, EOF/hole.
- **FIEMAP:** half-open range clipping, physical offset đúng sau clipping, logical_bytes không phải padding, non-contiguous/shared/inline/hole flags, iterator position, maximum extents. Test kernel FIEMAP ioctl với image VM.
- **Iomap/native async:** đề xuất thiết kế rồi prototype read-only; chỉ mở write mapping khi có ordered data/writeback/durable commit contract đồng bộ. Quản lý lock hierarchy để tránh recursive pagefault/fiemap deadlock.
- **Prefetch and caches:** LRU keyed theo generation, bounded memory, eviction không free active EB refs; shrinker khi ref lifetime được chứng minh.
- **Performance:** fio seq/random 4K/64K/1MiB, fsync latency P50/P95/P99, reflink/snapshot, 1k/100k/1M small files, memory pressure; so sánh baseline VTXFS trước/sau và Btrfs cùng profile/thiết bị khi phù hợp (không coi kết quả là tương đương tuyệt đối).

**File:** `kernel/{file,writeback,direct-io,fiemap,extent_io,delalloc,ordered-extent,async-thread,lru_cache}.c`, `core/vtxfs_posix.c`.
**Gate M6:** không đọc stale page/extent, coherent mixed I/O, đúng fsync semantics, checksum/faults propagate, 0 corruption sau vmstress; metric regression threshold phải được thống nhất sau baseline M0, không tối ưu bằng cách bỏ flush.

## 9. M7 — fsck, scrub, sửa chữa và backup/restore

**Mục tiêu:** phát hiện chắc chắn corruption, sửa chỉ khi có đủ bằng chứng và không gây mất dữ liệu thứ cấp.

- Phân loại công cụ: `fsck.vtxfs --check` (chỉ đọc, độc lập), `--repair --dry-run` (mô tả), `--repair` (mặc định OFF, chỉ offline/unmounted) với transaction journal/resume; báo cáo item/path/allocation/XID, confidence và giới hạn.
- `fsck` kiểm tra tất cả roots đã hoặc chưa load: B+Tree separators, leaf overlap, checksum, owner/level/XID, OMAP snapshot, REF owners, SFQ, VAG bitmaps, orphan/INODE_REF/DIR indexes, allocation collision.
- `scrub` online: data CRC + metadata digest, hỗ trợ cursor/resume, progress, cancel, rate-limit, safe readonly mode; phân biệt detect với repair. Không “sửa bằng checksum” nếu không có bản sao tin cậy.
- Metadata redundancy **DUP_META**: định nghĩa failure domain, mirror lookup record, transaction CoW và CPD publish; chỉ repair từ verified good copy, scrub/repair có journal và audit.
- `vtxfs-image`: metadump `--sanitize` và `--full`; thêm kiểm tra integrity dump, manifest digest, restore-to-new-image, version negotiation; không hứa metadump-only khôi phục nội dung data.
- Simulation: flip checksum, corrupt key, orphaned inode, missing root, duplicate REF, wrong VAG bit, poisoned SFQ, torn mirror, interrupted upgrade; verify unrepairable returns fail-closed.
- Định nghĩa `read-only rescue` và forensic mode, không ghi metadata mới nếu mount chỉ đọc; process báo lỗi ra CLI, exit codes thống nhất.

**File:** `core/vtxfs_fsck.c`, `kernel/{scrub,disk-io,cpd}.c`, `tools/{fsck_vtxfs.c,vtxfs-image/}`.
**Gate M7:** corruption cases phát hiện đúng; repair chỉ chạy sau backup/dry-run và có test rollback; sau sửa fsck/byte comparison đạt; không làm tổn hại volume khi không đủ bản sao. Backup full-restored hoàn nguyên SHA-256 file.

## 10. M8 — Snapshots, reflink, send/receive, quota

**Mục tiêu:** chia sẻ extent an toàn trên quy mô nhiều snapshot và có thể chuyển dữ liệu giữa các volume.

- Snapshot/clone: CPD-consistent readonly/RW snapshot, OMAP view roots, refcount owner index; sửa file trong clone không thay snapshot; deletion/Reaper progress crash-resumable.
- Reflink (file ranges): clone, partial overwrite, truncate, splice/hole punches; validate extent checksums và sharing, inode ownership; stat/du có quy tắc tính shared blocks.
- Send/receive: định nghĩa **VTXFS stream protocol** riêng (magic, version, feature capabilities, sequence, chunk CRC/digest, path traversal safety, opaque IDs, base snapshot digest). Không sử dụng trực tiếp Btrfs send stream.
- Incremental send: diff giữa snapshot A→B, delete/rename/symlink/xattr/hardlink nếu hỗ trợ; receiver verify complete stream rồi atomic publish snapshot, abort/resume idempotent khi mất điện/mất kết nối; reject unexpected op.
- Quota/qgroup: thiết kế exclusive/shared bytes, snapshot owner semantics, reservation ENOSPC/EDQUOT; quota có thể rebuilt offline và có explicit `inconsistent` state, không được bán gian quota trước khi full owner protocol.
- Stress hàng nghìn snapshot ở nhiều mức nông/sâu, clone với nhiều writer, delete và resume, send/receive random interrupted streams, compare directory SHA tree và fsck.

**File:** `core/{vtxfs_posix,vtxfs_engine}.c`, `kernel/{root,backref,reaper,ioctl}.c`, `tools/{vtxfs-send.c,vtxfs-receive.c,vtxfs/}`, UAPI proposal.
**Gate M8:** mọi snapshot vẫn byte-đúng sau mutation và reclaim; streamed destination checksum/metadata tương thích; interrupted receive không công bố snapshot nửa chừng; quota statistics reconcile REF/backref.

## 11. M9 — Defrag, discard, balance và vận hành không gian

**Mục tiêu:** di chuyển dữ liệu/metadata mà không làm hỏng ảnh CoW, snapshot hoặc refcount.

- `vtxfs-defrag` từ scanner → planner → transactional executor; extent selection dựa fragmentation/cost; copy đến allocation mới, verify checksum, publish new extent owner, retire old extent bằng SFQ đúng safe XID.
- Chính sách shared extent: không tự phá sharing/snapshot; chỉ clone-on-write theo yêu cầu và báo chi phí; rebuild mapping/owner records.
- `vtxfs-balance`: relocation giữa VAG cùng thiết bị ở mức per-allocation, transaction cursor, bandwidth throttle, pause/resume, kiểm tra VAG boundary; không đợi multi-device.
- Discard/FITRIM/zeroing: chỉ issue sau SFQ promotion/last pinned reader, không chạy trong hot transaction; phân biệt thin provisioning, block device capabilities và discard granularity, đối chiếu Btrfs `discard.c`.
- Space insights: `vtxfs filesystem df` tách allocated/reserved/pinned/snapshot/shared/reclaimable/metadata, không đánh đồng free bitmap và khả năng ghi mới.
- Fault injection sau copy trước verify, sau mapping update trước CPD, crash trong metadata relocation, replay khi allocation VBLK cũ được giữ bởi snapshot.

**File:** `tools/{vtxfs-defrag.c,vtxfs-balance.c}`, `kernel/{vag-alloc,extent-tree,backref,sfq,trans}.c`, `core/vtxfs_engine.c`.
**Gate M9:** byte-identical, fsck sạch và rollback qua tất cả crash points; không snapshot corruption; không discard block pinned/shared; đo fragmentation và throughput trước/sau, báo overhead.

## 12. M10 — Các advanced features (mỗi tính năng một RFC + feature gate)

**Không triển khai trọn M10 trong một bản vá.** Chọn từng profile độc lập; yêu cầu format compatibility và thử nghiệm riêng.

| Nhánh | Đích | Điều kiện để bật |
|---|---|---|
| **M10-A Compression** | zstd/lz4/zlib extent compression | Frame format bền vững, codec ID/version, logical/payload/physical lengths, checksums plaintext/ciphertext, compressed extent split/reflink/snapshot, fallback/ENOSPC, cross-kernel decoder |
| **M10-B Encryption** | AEAD per-file/subvol | Keyslot/key rotation policy, nonce uniqueness across crash/rollback/reflink, authenticated AAD, key revocation, decrypted cache semantics, no plaintext traces, secure erase claims tuyệt đối không thiếu cơ sở |
| **M10-C fs-verity / integrity** | Merkle descriptor + immutable verified read | Persist toàn bộ Merkle tree, verify mỗi read, Linux fs-verity API, trusted root lifecycle và recovery tests |
| **M10-D Metadata mirror** | DUP_META và self-healing scrub | Persistent mirror map, placement tách failure domain, validate cả hai copy, atomic repair/tracked allocation |
| **M10-E Multidevice** | device attach/remove/replace, device-aware VAG map | Stable device IDs, CPD membership generation, multi-device mount policy, degraded RO/RW rules, member loss fault matrix |
| **M10-F RAID/profile** | DUP/RAID1 trước parity | Verified stripe mapping, reads from good mirror, write ordering/FUA across members, replace/resilver, crash/partial member failures; parity chỉ sau mirror verified |
| **M10-G Tiering** | relocation hot/cold data | Verified copy, atomic mapping switch, resumable cursor, owner count / snapshot pinning |
| **M10-H Dedupe** | content-addressed sharing (tùy chọn) | Strong digest + byte compare, transactional sharing refs, quotas, encryption/compression interactions |
| **M10-I Quotas/ACL/Unicode** | nâng cao policy | ABI negotiation, stable semantics, migration, permission enforcement, negative tests |

**Feature protocol (bắt buộc):**
1. RFC/ADR nêu schema mới, mount old/new, format version, feature-bit type (`COMPAT`/`RO_COMPAT`/`INCOMPAT`).
2. Tạo isolated on-disk golden corpus; old reader must fail safely or enter supported RO mode, không âm thầm RW.
3. Implement read + write + fsck + repair policy + migration + rollback + crash tests **trước** khi quảng bá capability.
4. Không tái sử dụng một bit UAPI chỉ vì tên tương tự khi semantics chưa trùng; registry feature ở `uapi/vtxfs.h` và document schema; trường reserved luôn được zero/validate.
5. Migrate bằng tool explicit `--dry-run`/`--apply` và backup, không auto convert khi mount; downgrade path rõ ràng hoặc documented irreversible.
6. Performance + memory + cross-kernel compatibility + negative fuzz trước release.

## 13. M11 — Release, vận hành và chất lượng sản phẩm

**Release profiles:**
- **R0 Developer-only:** RAM image, portable engine, CLI, không bảo đảm mount kernel; dùng để test/format thử.
- **R1 Experimental kernel:** VM/loop-only, có xfstests subset, coherent VFS, crash matrix, fsck, diagnostic và rollback; dữ liệu thử nghiệm.
- **R2 Single-device Beta:** format frozen, kernel test matrix, snapshot/backref/space accounting, repair/backup, fio/fsstress/stress, tài liệu hạn chế; thử trên dữ liệu không quan trọng có backup ngoài.
- **R3 Single-device Stable Candidate:** negative/fuzz fault coverage theo release gates, on-disk compatibility/migration, known-bugs triage, multi-month soak testing và independent review về data loss; không tự xưng stable chỉ nhờ pass unit.
- **R4 Advanced profiles:** multidevice, compression, encryption... mỗi tính năng được bật độc lập sau gate M10, không gộp vào R3 mặc định.

**Checklist phát hành:**
- Per-commit: style, sparse/clang-analyzer, compiler matrix, unit, P0 crash, API ABI regression.
- Daily/continuous integration: fuzz with seeds retained, ASan/UBSan userspace, KASAN/KCSAN/lockdep VM, fail-injection commit matrix, 10k–100k mixed operations.
- Release: xfstests supported subset, fio workload + fsstress, remount/fsck after every crash cycle, image backup/restore, no unexplained oops/hang/leak, performance regression review.
- Compatibility: v2.1 golden images RW/RO per supported flags, candidate feature formats read/write, unsupported flags correctly reject; documentation and exact supported kernel versions.
- Security: untrusted disk fuzzing, integer overflow, path traversal, user copy, mmap/pin lifetimes, key management if enabled; report audited limitations.
- Operations: recovery manual, fsck exit codes, scrub, trace controls, reproduction script, rollback note, feature readiness ledger, signed release notes.

**Gate M11:** báo cáo build + runtime + fault + portability + migration + exact capability matrix là bản ghi không sửa sau phát hành; bug data loss / UAF / double-free chưa giải quyết chặn release.

## 14. Định nghĩa nghiệm thu xuyên suốt (không được bỏ)

| Invariant | Yêu cầu kiểm chứng | Mức nghiêm trọng |
|---|---|---|
| Root consistency | CPD chọn root tree, OMAP, REF, SFQ, VAG thuộc **cùng** committed generation | Blocker |
| Ownership | Không free khi có owner/snapshot/pinned refs; refcount/backref khớp toàn bộ roots | Blocker |
| Space safety | No double allocation, no out-of-VAG range, no silent u64→u32 overflow; cache ≠ authority | Blocker |
| Read integrity | Metadata checksum+geometry/identity; file CRC / AEAD khi bật; invalid item fail closed | Blocker |
| POSIX durability | fsync/fdatasync/syncfs có contract; truncate/rename atomic theo profile | Blocker |
| Page-cache coherency | mixed mmap/direct/buffered không trả stale data, no UAF | Blocker |
| Corruption handling | Không auto “repair” khi không có bản sao/đủ bằng chứng; report lỗi/RO | Blocker |
| Compatibility | Unknown incompat flag không mount RW; migration crash resumable hoặc rollback-safe | Blocker |
| Observability | Error + XID + VBLK/alloc ID có log rate-limit, không rò nội dung dữ liệu | Required |
| Performance | Không regression nghiêm trọng dưới baseline đã chốt; có số liệu phân vùng workload | Required |

**Mức định lượng đề xuất để thiết lập sau baseline M0:** 0 silent corruption, 0 double-free/early-reuse, 0 unreproduced crash regressions; đủ số test lỗi ở mỗi transaction stage và ở mỗi feature gate; bao phủ snapshot+reflink+ENOSPC+remount. Không dùng tỷ lệ pass đơn thuần khi vẫn còn known data-loss bug. Không đặt TPS/MB/s phi thực tế trước khi có thiết bị/workload và số đo baseline.

## 15. Tiêu chí kỹ thuật bổ sung và chỗ cần tránh

- **Không copy Btrfs code trực tiếp**: VTXFS có key format, OMAP và CPD riêng. Tham khảo kiểm tra parent transid, free-space tree và iomap, nhưng sửa đúng theo schema VTXFS.
- **Không đồng nhất APFS với driver chính thức của Apple**: mã `linux-apfs-rw-master` là third-party GPL-2.0-only và có giới hạn của chính driver đó.
- **Không coi KUnit object build là KUnit test runtime**; thực thi trên disposable VM, thu KTAP.
- **Không nối `kernel/backref.c` diagnostic API vào free path** cho tới M4; `fs->roots` cache không phải tất cả snapshot roots.
- **Không quảng bá defrag/send/receive/RAID/compression/AEAD là hoàn chỉnh** khi source còn scaffold/deferred.
- **Không thử trên ổ vật lý** ở các mốc phát triển: hardware fault studies phải được phép và có backup, bắt đầu từ VM/image không giữ dữ liệu thật.
- **Không ghi đè định dạng hiện hữu trong quá trình thiết kế**. Chỉ migration explicit, backup trước, version/feature bit sau full end-to-end contract.
- **Không ưu tiên micro-optimization trước repair/crash**. Mọi thay đổi hiệu năng phải có data/integrity regression.
- **Đừng sửa hàng loạt mã source chỉ vì tồn tại hàm tương tự trong Btrfs/APFS**: trước mỗi patch cần source audit, invariant, design, negative test và rollback.

## 16. Danh mục PR/backlog đề xuất và thứ tự triển khai

Mỗi dòng là một đơn vị nhỏ có thể review; không trộn schema migration và optimization trong một thay đổi:

| PR ID | Milestone | Nội dung bản vá | Điều kiện hợp nhất |
|---|---|---|---|
| R-001 | M0 | Golden v2.1 image corpus + ABI feature policy contract | Test read & byte snapshots |
| R-002 | M0 | CI gcc/clang, sanitizers, kbuild và artifact logs | Reproducible script |
| R-003 | M0 | VM harness ramdisk/loop safe guard + test exit status | Không touching host disk |
| R-004 | M1 | KUnit VAG/backref runtime wiring | KTAP PASS thực |
| R-005 | M1 | Kernel VFS mount/unmount + fsck smoke | Repeated mount without oops |
| R-006 | M1 | POSIX errno/page-cache/open-after-unlink matrix | Byte/metadata comparison |
| R-007 | M2 | Tree block identity+XID/parent check unified | Corruption rejection |
| R-008 | M2 | Checkpoint cache invalidation and pin protocol | Old XID never reused silently |
| R-009 | M2 | Full CPD fault cutover matrix with mirrors | Deterministic recover |
| R-010 | M3 | Kernel VAG range preflight/reclaim stress | Double-free no mutation |
| R-011 | M3 | Free-space RB cache split/merge+rebuild | REF/SFQ cross-check |
| R-012 | M3 | Delalloc reservations/pinned/ENOSPC accounting | No over-commit without error |
| R-013 | M4 | Persistent backref RFC + schema golden | Spec + backward mount behavior |
| R-014 | M4 | Backref construction tool from full root set | Refcount reconciles |
| R-015 | M4 | Atomic backref delta + fsck verifier | Fault/crash-pass |
| R-016 | M4 | Explicit migration/rollback mode | Old image safe |
| R-017 | M5 | Directory collision/hash/index/inode-ref tests | No orphan/cycle |
| R-018 | M5 | xfstests supported subset + locking races | No deadlock |
| R-019 | M6 | FIEMAP / O_DIRECT VM ioctl regression | Clipped physical correct |
| R-020 | M6 | Ordered I/O + buffered/writeback errors | fsync and crash tests |
| R-021 | M6 | Folio/extent cache memory safety | KASAN/lockdep clean |
| R-022 | M7 | fsck dry-run/repair architecture | Zero unsafe repair |
| R-023 | M7 | Resumable scrub+dump/restore manifest | Checksum validation |
| R-024 | M7 | DUP_META RFC + mirror repair tests | Rollback-safe |
| R-025 | M8 | Full snapshot/reflink/GC stress | All owners coherent |
| R-026 | M8 | Send/receive stream RFC + parser fuzz | No traversal/partial publish |
| R-027 | M8 | Incremental send + receiver resume | Exact tree/bytes |
| R-028 | M9 | Transactional defrag single-device | Faulted relocation safe |
| R-029 | M9 | Async discard under SFQ/pinning | No discard live block |
| R-030 | M9 | VAG balance on single device | Byte-identical after resume |
| R-031 | M10 | Compression first feature-gated profile | on-disk decode+recovery |
| R-032 | M10 | AEAD key/nonce RFC and threat model | no nonce reuse after rollback |
| R-033 | M10 | Multidevice/DUP/RAID1 schema RFC | member loss/durable profile |
| R-034 | M11 | Release profile and compatibility report | gates sign-off |

**Ưu tiên mở đầu:** `R-001 → R-003 → R-004 → R-005 → R-007/R-008/R-009 → R-010/R-011/R-012`. Chỉ sau đó quyết định versioned backref RFC/M4. Không bắt đầu native RAID trước khi xác minh CPD và single-device data path.

## 17. Cách vận hành việc triển khai

Với mỗi PR, đi theo chu trình: **Design → Invariant → Code nhỏ → Unit/negative test → Runtime VM → Fault/corruption test → Benchmark → Document → Review**. Mỗi việc được ghi 5 trường: phạm vi file, phụ thuộc, ảnh thử, expected pass/fail, cách rollback. PR thay đổi on-disk phải ghi thêm feature flag, compatibility, migration, fsck/repair và tool đọc cũ.

**Definition of Done:**
- [ ] Không thêm warning khi build GCC/Clang/kbuild.
- [ ] Có bài thử làm hỏng tình huống trước vá, đạt sau vá; seed được lưu.
- [ ] Có test lỗi input/corruption, checksum, ENOSPC/ENOMEM và cleanup.
- [ ] Nếu đụng persistent state: có crash-point matrix và fsck sau remount.
- [ ] Nếu đụng kernel: có kết quả runtime cô lập, lockdep/KASAN khi áp dụng.
- [ ] Nếu đụng schema: có golden image, feature gate, migration + rollback.
- [ ] Nếu đụng hiệu năng: benchmark đối chiếu và chứng minh không bỏ integrity checks.
- [ ] Báo cáo “đã triển khai” phân biệt build-only với runtime PASS.
- [ ] Tài liệu cập nhật contract và limitations.

## 18. Chứng cứ nền đã có và bước tiếp theo

**Đã có:** `tests/run.sh` toàn bộ GCC/Clang P0 đạt; `tests/test_terminal_full_matrix.py` 22/22 nhóm với 596 lệnh; thử ghi/đọc 144 MiB qua VAG có SHA-256 và ENOSPC recovery sạch; `vtxfs.ko` build; bộ 100.000 FIEMAP clipping cùng SFQ range validator với hai compiler; API backref 61 symbol đã kiểm kê. Đó là bằng chứng cho **những đường đã thực thi**, không thay thế kernel runtime test.

**Chưa chứng minh:** P0 kernel mount/umount + xfstests trên VM; KUnit actual execution; full backref persisted qua tất cả snapshot; free-space on-disk rebuild; defrag thực; send/receive production; AEAD/compression on-disk; multi-device/RAID.

**Bước tiếp theo đúng thứ tự:** triển khai R-001/R-002/R-003 để tạo một bộ máy test tin cậy, rồi thực thi runtime KUnit + kernel mount R-004/R-005 trên VM, dùng kết quả để sửa M1/M2. Không bật bất kỳ feature incompat deferred nào tại thời điểm lập kế hoạch này.
