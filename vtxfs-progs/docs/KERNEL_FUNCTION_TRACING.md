# VTXFS kernel — nhật ký hàm nội bộ và kiểm kê đường gọi

## Mục tiêu

Dùng **2 lớp quan sát** để trả lời: hàm kernel nào thực sự tham gia
thao tác VTXFS, hàm nào chưa xuất hiện, và hàm nào là callback/hàm nội
bộ mà người viết code dễ bỏ qua.

**Không suy ra hàm không được sử dụng chỉ từ một lần đo.** Hàm có thể chỉ
chạy khi báo lỗi, restore, reaper, freeze, CoW hoặc khi kernel gọi qua
struct `file_operations`/`inode_operations`/`super_operations`. Mã nằm
trong `kernel/` không mặc nhiên có đường gọi từ I/O của VFS.

## Đã bổ sung vào source

- 51 file C ở `kernel/**/*.c` (bao gồm KUnit) và 2 header
  `kernel/ctree.h`, `kernel/vtxfs_drv.h`.
- **411 định nghĩa hàm** có `VTXFS_TRACE_ENTER();` tại đầu thân hàm,
  gồm 10 hàm `static inline` trong header,
  trong đó có các hàm `static` và callback VFS. Không chèn vào prototype,
  khai báo biến, hoặc struct initializer.
- **17 log sự kiện tham số/giai đoạn/kết quả** bổ sung ở
  `kernel/super.c`, `file.c`, `extent_io.c`, `trans.c`,
  `vag-alloc.c`, `mount-native.c`: mount stage, I/O folio, metadata
  tree read/write, extent state, alloc VAG, checkpoint commit/abort.
- Macro trong `kernel/vtxfs_trace.h` dùng
  `pr_debug_ratelimited()`, mặc định *tắt* vì dynamic debug; mỗi callsite
  rate limit độc lập. Không in nội dung file, mật khẩu, buffer data,
  địa chỉ kernel; chủ yếu in tên hàm/ID/mã khối.
- Có bản sao source trước khi sửa trong
  `.vtxfs-backup-20261026/kernel-trace-20261026/`.
  Generator `tests/instrument_kernel_calls.py` hỗ trợ chạy lặp lại
  mà không chèn trùng.

## Tĩnh: hàm nào liên kết với module?

Chạy mà KHÔNG cần root, KHÔNG cần module đang nạp:

```bash
cd /home/haui/Music/Downloads/VTXFS
tools/trace-vtxfs.sh inventory
```

Kết quả được ghi ra:

- `docs/KERNEL_FUNCTION_ENTRY_INVENTORY.csv` và `.json`:
  mỗi hàm, vị trí định nghĩa, `static`/external, tình trạng được chèn log.
- `docs/KERNEL_FUNCTION_BUILD_AUDIT.csv`:
  module chính hay test tùy chọn; symbol có xuất hiện trong `vtxfs.ko`;
  số lần tên hàm được nhắc đến trong C code (gợi ý, không phân tích
  được chính xác callback/macro/inlining); trạng thái runtime *chưa xác minh*.

Kiểm kê ban đầu: **379 định nghĩa trong các nguồn module chính** (một số
định nghĩa có thể bị compiler inline/optimize), **22 trong KUnit/e2e
tùy chọn**, **10 helper inline trong header**, và bảng symbol của
`vtxfs.ko` được cập nhật từ bản build hiện tại. Vắng symbol
**không chứng minh** hàm độc lập, vì compiler có thể inline/tối ưu.

## Runtime: bật log theo file trước (khuyến nghị)

**Chỉ dùng trên hệ thống thử nghiệm, với module VTXFS đã được load và
image/thiết bị RAM riêng khi cần mount thử.** Script kiểm tra
`/sys/module/vtxfs`; không tự nạp module, không tự format, không tự mount
bất kỳ block device nào.

```bash
cd /home/haui/Music/Downloads/VTXFS

# Bật log cho file super.c; ít log hơn bật cả dự án
sudo tools/trace-vtxfs.sh enable-file super.c

# Xem log kernel khi bạn thực hiện thao tác thử với VTXFS
sudo tools/trace-vtxfs.sh follow

# Bật tất cả 411 vị trí log (chỉ để điều tra ngắn)
sudo tools/trace-vtxfs.sh enable

# Tắt lại khi hoàn tất; không ảnh hưởng log hệ thống khác
sudo tools/trace-vtxfs.sh disable
```

Ví dụ log khi đường gọi chạy:
```text
vtxfs-call enter vtxfs_fill_super() pid=1234
vtxfs-event vtxfs_fill_super(): stage=engine-mount
vtxfs-call enter vtxfs_mount_native() pid=1234
vtxfs-event vtxfs_commit_transaction(): commit xid=... published=1
```
*Đây là ví dụ định dạng, KHÔNG phải log kernel thực tế thu được.*

## Runtime: ftrace cả hàm nội bộ và core helper

Kernel đang dùng được biên dịch với
`CONFIG_FUNCTION_TRACER=y`, `CONFIG_FUNCTION_GRAPH_TRACER=y`.
Bộ điều khiển dùng một instance **`vtxfs_audit`** riêng để không làm
thay đổi global tracer. Việc sử dụng ftrace cần quyền root và tracefs
đã mount; mặc định không bật.

```bash
sudo tools/trace-vtxfs.sh ftrace-start function
# Chạy workload VTXFS trong môi trường cô lập, bằng các lệnh/file API sẵn có
sudo tools/trace-vtxfs.sh ftrace-read
sudo tools/trace-vtxfs.sh ftrace-stop
```

Có thể dùng `ftrace-start function_graph` để có call graph. Bộ lọc
`*:mod:vtxfs` áp dụng cho các symbol traceable thuộc module VTXFS, bao
gồm hàm `static` trong module và các `core/*.c` được liên kết.
Các hàm inline/optimized hoặc không được liên kết sẽ không hiện ra.
Bật tracing cũng có overhead và không thay cho correctness testing.

## Tạo báo cáo "đã chạy / chưa thấy"

Chỉ sau khi có log runtime từ workload muốn xét, lưu bản ghi thành
một file văn bản rồi so với inventory:

```bash
# Ví dụ thu sau khi enable runtime debug (cần root)
sudo dmesg > /dev/shm/vtxfs-trace.log
python3 tests/summarize_kernel_trace.py /dev/shm/vtxfs-trace.log \
  --csv /dev/shm/vtxfs-coverage.csv
```

`SEEN` nghĩa là tìm thấy entry trong bản ghi, còn
`NOT_SEEN_IN_THIS_TRACE` nghĩa là chưa tìm thấy trong **workload này**.
Không được tự động xóa code dựa vào nhãn NOT_SEEN. Nên thực hiện
nhiều workload: mount/unmount, mkdir/rmdir, create/read/write/fsync,
rename, truncate, xattr, readdir, snapshot, kiểm tra lỗi/checksum, reaper.

## Kiểm thử build và giới hạn hiện tại

```bash
python3 tests/instrument_kernel_calls.py
python3 tests/audit_kernel_symbols.py
bash -n tools/trace-vtxfs.sh
make -C /lib/modules/`(uname -r)/build M="`PWD" modules -j2
```

Đã build `vtxfs.ko` thành công với các macro log; section
`__dyndbg` có trong output. **Chưa nạp module hay chạy workload kernel
để thu runtime trace**, do thao tác đó cần quyền quản trị và môi trường
mount VTXFS cô lập. Bộ test userspace trước đó không kích hoạt code
`kernel/*.c`, nên không thể dùng chúng để đánh dấu runtime.

Dự án có các nguồn `selftest_e2e.c` và KUnit tùy chọn; mặc định không
liên kết vào `vtxfs.ko` và chưa thể hiện `SEEN` trong log của module đó.

### Backref mới

Đã mở rộng `kernel/backref.c`, gồm các bộ duyệt extent, shared refcount,
preliminary ref RB-tree, inode path/iterator và các API báo rõ
`-EOPNOTSUPP` cho tính năng chỉ có trong metadata Btrfs.
Xem [KERNEL_BACKREF_API.md](KERNEL_BACKREF_API.md) để biết khả năng thực
sự đã triển khai và giới hạn chưa kiểm chứng ở runtime kernel.
