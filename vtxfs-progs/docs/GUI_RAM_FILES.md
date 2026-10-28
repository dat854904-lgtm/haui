# VTXFS Files — ứng dụng thử nghiệm ổ đĩa ảo trong RAM

## Khởi động
Từ thư mục dự án:

```sh
sh tools/run-vtxfs-files.sh
```

Ứng dụng Gtk4 giống GNOME Files có các chức năng:
- Duyệt thư mục, đi lên, làm mới.
- Tạo thư mục và tệp văn bản.
- Mở, đọc và sửa văn bản UTF-8 tới 4 MiB trong GTK (giới hạn trình sửa, không phải giới hạn file VTXFS).
- Nhập tệp lớn: chỉ đọc file nguồn do người dùng chọn, ghi vào image VTXFS trên RAM qua stdin.
- Tạo dữ liệu thử 1, 4 hoặc 16 MiB để quan sát Block Bitmap.
- Ghi từng chunk 64 KiB, checkpoint mỗi 256 KiB. Nếu lỗi giữa chừng, file có thể chỉ được ghi một phần; cần kiểm tra thông báo và fsck.
- Đổi tên, di chuyển (qua lệnh CLI `rename`), xóa tệp/thư mục rỗng.
- Kiểm tra tính toàn vẹn image qua nút fsck.

**An toàn dữ liệu:** Image được đặt cứng tại
`/dev/shm/vtxfs-files-<uid>.img`. Chương trình xác minh
`/dev/shm` thực sự là filesystem `tmpfs` trước khi khởi tạo.
Nó sử dụng VTXFS POSIX engine trong userspace qua các công cụ
`mkfs.vtxfs`, `vtxfs-inspect` và `fsck.vtxfs`. Nó KHÔNG mount
phân vùng vật lý, KHÔNG cần sudo, không gọi losetup hay format ổ hệ thống.

Chú ý: Dữ liệu trong image RAM sẽ mất khi reboot hoặc khi xóa image.
Ứng dụng không cung cấp quyền quản lý file thực trong Linux/Nautilus;
đây là giao diện chuyên biệt cho volume VTXFS ảo, không phải FUSE hay
bản mount kernel-native. Image mặc định 256 MiB, nhưng chỉ khoảng 128 MiB là vùng VAG có thể cấp phát (trừ metadata). Có thể tạo image RAM riêng 512 MiB hoặc 1024 MiB mà không ghi đè image cũ.

## Test
```sh
# Chỉ sử dụng RAM-backed tmpfs để lưu image và tệp test tạm
TMPDIR=/dev/shm bash tests/run.sh
python3 tests/test_files_gui_backend.py

# Kiểm tra image đã tồn tại
tools/fsck.vtxfs -q "/dev/shm/vtxfs-files-$(id -u).img"

# Build module kernel (không tự insmod / mount ổ vật lý)
make -C /lib/modules/$(uname -r)/build M="$PWD" modules
```

## Các phần kernel đã cập nhật
- `kernel/extent_io.c`: VAG translation (kiểm tra cả run và kích thước thiết bị);
  interval-state canonical RB-tree với split/gap/set/clear/merge và waitqueue
  chung chống sử dụng state đã bị giải phóng; lookup/cleanup extent_map.
- `kernel/vag-alloc.c` + `kernel/vag.h`: helper chặn run crossing VAG.
- `kernel/inode.c`: giải phóng reference tree-owned của extent_map ngoài lock.
- `kernel/kunit/vtxfs_ctree_test.c`: các trường hợp overlap, gap, dirty_bytes,
  map lookup và lifetime. KUnit chỉ có thể chạy khi kernel hỗ trợ KUnit và các
  phụ thuộc kbuild được cấu hình; việc biên dịch `.o` không có nghĩa KUnit đã chạy.

Có bản sao lưu mã gốc trong `.vtxfs-backup-20261026/`. 

## Block Bitmap trực quan

Chuyển sang tab **Block Bitmap** trong VTXFS Files. Giao diện đọc
bitmap VAG thật bằng lệnh `vtxfs-inspect bitmap` ở chế độ **chỉ đọc**.
Nút **Cập nhật bitmap** lấy snapshot mới từ volume RAM.

- Chọn VAG, số block/trang (256/512/1024/2048), mức phóng to.
- Nhấp một ô để xem offset trong VAG, VBLK, PBLK và byte offset trong image.
- Nhấp mini-map để chuyển tới vùng dữ liệu; ô thay đổi giữa hai lần tải
  được viền cam. Bitmap không phân biệt dữ liệu file với metadata.
- Block ảo của VAG: `VBLK = VAG_ID * 32768 + offset`.
- Block trong image: `PBLK = VAG.start_pblk + offset`.
- Byte offset trong image: `PBLK * 4096`.
- PBLK 0..32767 là vùng bootstrap nằm ngoài VAG, không hiển thị trong bitmap.

Có thể xem bằng CLI mà không cần GUI:
```sh
tools/vtxfs-inspect "/dev/shm/vtxfs-files-$(id -u).img" bitmap 0 0 64
```
Kết quả `bitmap_hex` có thứ tự bit thấp trước trong mỗi byte (bit 0
ứng với VAG offset đầu tiên của đoạn); `allocated` và `free_blocks`
là bộ đếm của cả VAG. Dữ liệu chỉ được đọc, không thay đổi bit.

Kiểm thử:
```sh
python3 tests/test_bitmap_gui_backend.py
python3 tests/test_bitmap_widget.py
```

## Ghi file lớn và chọn image RAM lớn hơn

Giới hạn 64 KiB trước đây là do trình sửa GTK và việc truyền nội dung
qua argv. Từ phiên bản này, lệnh `put-stdin` nhận dữ liệu nhị phân
và checkpoint theo từng cụm; trình sửa GTK hỗ trợ văn bản tới 4 MiB,
còn chức năng Nhập tệp có thể đưa file lớn hơn vào VTXFS.

VTXFS có 128 MiB bootstrap trước vùng VAG. Image 256 MiB chỉ có
khoảng 128 MiB vùng block cấp phát; image 512 MiB có 384 MiB,
image 1024 MiB có 896 MiB (chưa trừ CoW/metadata).
Để giữ nguyên dữ liệu volume cũ, các dung lượng khác tạo image RIÊNG:

```sh
# Image cũ/mặc định 256 MiB
sh tools/run-vtxfs-files.sh

# Image RAM mới 512 MiB (không thay thế image cũ)
VTXFS_RAM_MIB=512 sh tools/run-vtxfs-files.sh

# Image RAM mới 1 GiB (không thay thế image cũ)
VTXFS_RAM_MIB=1024 sh tools/run-vtxfs-files.sh
```

Các image đều đặt trong `/dev/shm` là tmpfs, không format thiết bị vật lý.
Image 512 và 1024 MiB tách theo tên chứa kích thước, mỗi kích thước
có ứng dụng GTK riêng. Không dùng RAM image để lưu dữ liệu quan trọng.

Thử lệnh nhị phân qua stdin:

```sh
# Ví dụ: dữ liệu từ pipeline đi vào một file bên trong image VTXFS
# source | tools/vtxfs-inspect /dev/shm/vtxfs-files-$(id -u).img put-stdin /example.bin
```

Kiểm thử bổ sung:

```sh
python3 tests/test_large_write_stream.py
python3 tests/test_large_write_multi.py 4
python3 tests/test_large_gui_integration.py
```

## Bảng Nhật ký hoạt động bên cạnh File List

VTXFS Files có cột **Nhật ký hoạt động** ngay bên phải danh sách file
trong tab **Tệp và thư mục**. Nút biểu tượng thanh bên ở góc phải
thanh công cụ dùng để ẩn hoặc hiện cột log. Log không phải cửa sổ
riêng: có thể quan sát file và quá trình ghi cùng lúc.

Trạng thái hiển thị: `CHẠY` (bắt đầu lệnh), `OK` (lệnh đã kết thúc
thành công), `GHI` (tiến độ chuyển byte vào stdin), `LỖI` (lỗi),
và `INFO` (thông tin giao diện). Có dấu thời gian HH:MM:SS.
Với file lớn, mỗi 1 MiB đã chuyển sang stdin sẽ có một dòng; chỉ
khi CLI trả về exit code 0 mới ghi **Hoàn tất**. Bởi vì CLI có
checkpoint riêng, dòng tiến độ không có nghĩa dữ liệu đã commit.

- **Xóa log:** nút chổi phía trên cột.
- **Sao chép log:** nút copy phía trên cột.
- Log chỉ ở trong RAM phiên ứng dụng, giữ 750 sự kiện gần nhất.
- KHÔNG ghi nội dung văn bản/binary vào log; chỉ tên lệnh, đường dẫn
  nội bộ VTXFS, số byte, thời gian và kết quả.
- Log này ghi nhận hoạt động của ứng dụng/CLI userspace, không phải
  log kernel-native printk, tracepoint hay chi tiết nội bộ CoW.

Kiểm thử bảng log GTK và tiến độ ghi file:

```sh
python3 tests/test_activity_log_panel.py
python3 tests/test_large_gui_integration.py
python3 tests/test_large_gui_import.py
```

## Menu chuột phải và các thao tác kiểu GNOME Files

Trong tab **Tệp và thư mục**, nhấp chuột phải vào một file hoặc thư mục
để mở menu tương tự GNOME Files. Các thao tác hỗ trợ:

- Mở thư mục hoặc file văn bản trong trình sửa VTXFS.
- Sao chép / Cắt / Dán trong image VTXFS, có thể chuyển sang thư mục khác
  trước khi dán. Đường sao chép dữ liệu sử dụng `copy-file`; cắt dùng
  `move-safe` để ngăn ghi đè file đích.
- Nhân bản file với tên mới `(bản sao)`; Sao chép tới… thư mục bất kỳ
  bên trong VTXFS; Chuyển tới… thư mục VTXFS; Đổi tên…
- Thuộc tính: inode, kích thước, allocated bytes, nlink, mode, XID.
- Nén file thành ZIP trong VTXFS (chỉ file thường tối đa 8 MiB,
  xử lý trong RAM); không đụng tới filesystem của host.
- Đưa vào Thùng rác VTXFS: di chuyển tới `/.vtxfs-trash` trong image.
  Có thể vào thùng rác bằng nút trong sidebar và **Khôi phục về /**;
  phiên bản này chưa nhớ đường dẫn gốc và không phục hồi tới thư mục ban đầu.
- Xóa vĩnh viễn có xác nhận, không thể hoàn tác.
- Sao chép đường dẫn nội bộ VTXFS vào clipboard văn bản host (không
  sao chép nội dung file ra host).
- Tìm tên file qua ô tìm kiếm, bộ lọc không đổi dữ liệu on-disk.

Menu chỉ sao chép/nhân bản/nén **file thường**, chưa sao chép cây thư mục.
Cắt và di chuyển thư mục đã được hỗ trợ; move-safe từ chối di chuyển
thư mục vào trong chính nó. `copy-file` và `move-safe` không ghi đè
đường dẫn đích; thao tác có thể thất bại khi hết block hoặc metadata.
Với thao tác sao chép lỗi giữa chừng, đích có thể là file chưa hoàn chỉnh.

Phím tắt: `Ctrl+C` (copy), `Ctrl+X` (cut), `Ctrl+V` (paste),
`Ctrl+D` (duplicate), `Ctrl+F` (search), `Ctrl+R` (refresh),
`F2` (rename), `Delete` (delete with confirmation),
`Backspace` (go up), `Alt+Enter` (properties),
`Ctrl+Shift+N` (new directory).

Kiểm thử trên image VTXFS trong `/dev/shm`:

```sh
python3 tests/test_files_extra_actions.py
python3 tests/test_files_actions_gui.py
python3 tests/test_activity_log_panel.py
tools/fsck.vtxfs -q /dev/shm/vtxfs-files-$(id -u).img
```
