# VTXFS — BIO batching, readahead, group commit và dm-log-writes
Ngày: 26/10/2026

## Tình trạng mã

- `kernel/vtxfs_bio_batch.h` và `kernel/super.c`: khi engine yêu cầu hơn một block, chia request thành BIO đồng bộ tối đa 32 × 4096 byte = 128 KiB. Mỗi block dùng bounce page riêng vì engine có thể dùng địa chỉ không căn trang/vmalloc. Luôn `submit_bio_wait` và trả lỗi I/O. Yêu cầu CPD FUA một block tiếp tục dùng đường cũ, không thay đổi flush/FUA trong `vtx_sync_checkpoint`.
- `kernel/vtxfs_readahead.h` và `kernel/file.c`: `.readahead` đọc 128 KiB mỗi lượt bằng `vtx_read` (bao gồm kiểm CRC) rồi điền folio. Chỉ đánh dấu `uptodate` nếu engine đọc thành công, zero-fill phần ngoài EOF. Nếu lỗi, folio giữ trạng thái chưa hợp lệ để `read_folio` thử lại và trả lỗi.
- `kernel/vtxfs_drv.h`, `kernel/locking.c`, `kernel/file.c` và `kernel/super.c`: `op_seq` tăng khi kết thúc **mọi** engine operation (kể cả đọc/reaper), `durable_seq` chỉ cập nhật sau checkpoint trả thành công. `fsync` theo dõi tất cả thao tác hoàn tất, so lại dưới commit lock và gộp các `fsync` liên tiếp nếu đã bền. Mọi checkpoint thất bại giữ nguyên trạng thái dirty và lỗi, không đánh dấu durable. Đây là group commit **bảo thủ**, chưa có dirty-only per-inode counter và không có gom theo timer.
- `core/vtxfs_csum.c`: bản build kernel đã dùng `crc32c()` của Linux, bất biến checksum trên đĩa giữ `~crc32c(~crc, data, len)`.
- `tests/vm_guest_ra_group.inc` + `tests/vm_guest_human.inc`: kiểm tra 256 KiB đọc lại byte-exact sau DONTNEED, fsync lặp trên file sạch không tăng sector ghi.
- `tests/benchmark_dm_log_writes.py`: bộ thử root-only. **Chỉ dùng các image mới trong tmpfs**: backend, log, replay và replay-FUA; mkfs baseline, tạo loop devices riêng, lập target dm-log-writes, ghi 4 MiB entropy thực, file+directory fsync, ghi mark `fsync`, tháo mapper, replay đến mark; xác minh SHA-256, fsck và replay `--check fua --fsck` từng FUA, luôn cleanup mapper/loop. Chỉ báo PASS khi **replay thật** thành công. Không dùng tên thiết bị do người dùng nhập.
- `tests/benchmark_vtxfs_verified.py`: bộ fio lớn riêng; chưa có kết quả tốc độ đĩa hợp lệ vì giới hạn volume tối đa 8 GiB và máy hiện có gần 8 GiB RAM; dataset >2×RAM là không khả thi trên định dạng VTXFS hiện tại.

## Kết quả thử trực tiếp trên máy

1. Kernel module Arch 7.2.11-zen1-1-zen: **BUILD PASS**.
2. QEMU `tests/run_vm_human.sh`: human 0 defects/0 missing, cold remount/fsck PASS; `fsync_clean_no_extra_block_writes` và `sequential_cold_256KiB` PASS.
3. `VTXFS_VM_MODE=stress tests/run_vm_smoke.sh`: 4 luồng, 32 file, mmap + cold remount/fsck PASS.
4. `tests/run_vm_crash.py`: đã xác minh mất điện mô phỏng bằng ngắt QEMU và recovery CPD, không phải replay tầng block.
5. `tests/run_vm_orphan_crash.py`: orphan qua crash và fsck PASS.
6. `tests/benchmark_dm_log_writes.py` preflight: **BLOCKED**. Runtime đang là UID 1000, `/dev/mapper/control` từ chối truy cập và thiếu executable `replay-log`; vì thế **CHƯA CHẠY** dm-log-writes/FUA replay, không có kết quả được công nhận.

## Cách chạy và tiêu chuẩn nghiệm thu dm-log-writes

```bash
cd /home/haui/Music/Downloads/VTXFS
python3 tests/benchmark_dm_log_writes.py  # preflight, không tác động ổ
# Chỉ sau khi đã có quyền root, module log-writes và replay-log:
# python3 tests/benchmark_dm_log_writes.py --run
```

`replay-log` là công cụ userspace trong repo được Linux kernel docs giới thiệu:
`https://github.com/josefbacik/log-writes`. Thông số xác thực dựa trên
`/home/haui/Videos/linux-master/Documentation/admin-guide/device-mapper/log-writes.rst`.

Lưu ý: kể cả kết quả replay thành công, đây vẫn là **bài tính đúng đắn trên loop/tmpfs**, không phải băng thông hay p99 SSD. Chỉ nhận tốc độ thật sau khi chạy fio với dữ liệu đầy đủ lớn hơn RAM, không có fio error, đối chiếu thiết bị ghi, fsck, hash, dm replay + FUA và so với filesystem khác trong cùng điều kiện.

## Giới hạn chưa giải quyết

- BIO vẫn đồng bộ giữa các nhóm 128 KiB, chưa hỗ trợ nhiều request đồng thời/queue-depth lớn.
- Read-ahead 128 KiB vẫn chạy `vtx_read` và op_lock; checksum không bỏ qua, nhưng engine hiện có thể đọc từng block bên dưới.
- Group commit hiện coi cả thao tác đọc là mutation: an toàn nhưng sinh checkpoint không cần thiết trong workload đọc nhiều. Ngoài ra commit do DIO, ioctl, syncfs độc lập có thể gây commit thừa ở fsync tiếp theo.
- Chưa chạy thật `dm-log-writes` khi thiếu quyền và replay tool. Không được báo thử nghiệm mất điện thực hay bảo đảm production.
- Chưa có số MB/s, IOPS, p99, CPU/GiB trên thiết bị vật lý đủ điều kiện, nên chưa tuyên bố cải thiện hiệu năng.
