# VTXFS: Quản lý file và thư mục bằng terminal

Công cụ: `tools/vtxfsctl` (wrapper) và `tools/vtxfsctl.py`.
Dữ liệu chỉ đọc/ghi vào image VTXFS là **file thường trực tiếp ở /dev/shm
(tmpfs)**; không chạm block device thật. Lệnh sẽ từ chối file ảnh ngoài
/dev/shm, symlink của host và ổ vật lý.

## Khởi động

```sh
cd /home/haui/Music/Downloads/VTXFS
make -C tools vtxfs-inspect mkfs.vtxfs fsck.vtxfs
IMG="/dev/shm/vtxfs-files-$(id -u)-512MiB.img"
tools/vtxfsctl "$IMG" disk-space
tools/vtxfsctl "$IMG" fsck
```

**Chú ý:** Các lệnh dưới đây sẽ thay đổi image được chỉ định. Hãy dùng
image thử nghiệm trước, thay vì image chứa dữ liệu cần giữ.

## Create, Open, Edit, Properties, Rename

```sh
tools/vtxfsctl "$IMG" create /work --dir
tools/vtxfsctl "$IMG" create /work/notes.txt
printf 'Hello VTXFS\n' | tools/vtxfsctl "$IMG" edit /work/notes.txt
tools/vtxfsctl "$IMG" open /work/notes.txt
tools/vtxfsctl "$IMG" ls /work
tools/vtxfsctl "$IMG" properties /work/notes.txt
tools/vtxfsctl "$IMG" get-info /work/notes.txt
tools/vtxfsctl "$IMG" rename /work/notes.txt renamed.txt
```

Edit thay thế toàn bộ nội dung bằng luồng stdin nhị phân, không giới hạn
argv. Khi ghi lỗi giữa chừng, file có thể bị thay bằng dữ liệu chưa đầy đủ.
Open hiển thị nội dung file, mục thư mục hoặc target của symlink.

## Copy, Cut, Paste, Move

```sh
tools/vtxfsctl "$IMG" copy /work /work-copy
tools/vtxfsctl "$IMG" copy-select /work/renamed.txt
tools/vtxfsctl "$IMG" paste /work-copy
tools/vtxfsctl "$IMG" cut /work-copy/renamed.txt
tools/vtxfsctl "$IMG" paste /
tools/vtxfsctl "$IMG" move /renamed.txt /work/moved.txt
```

Copy hỗ trợ đệ quy thư mục (file thường và symlink). Cut/Copy-select
lưu clipboard vào `/.vtxfs-cli-clipboard` bên trong image, không dùng
clipboard của desktop. Paste nhận **thư mục đích** và không ghi đè file
trùng tên. Copy không atomic: lỗi có thể để lại bản sao một phần.

## Delete, Trash, Restore, Permanently Delete

```sh
tools/vtxfsctl "$IMG" delete /work/moved.txt
tools/vtxfsctl "$IMG" ls /.vtxfs-trash
# Dùng tên thực của file trong thùng rác thay cho TICKET-name:
tools/vtxfsctl "$IMG" restore TICKET-name
tools/vtxfsctl "$IMG" permanently-delete /work-copy --yes
tools/vtxfsctl "$IMG" empty-trash --yes
```

Trash lưu đường dẫn gốc vào `/.vtxfs-trash-meta` **trong image**, nhờ
đó Restore chuyển file về đúng thư mục ban đầu. Nếu thư mục cha bị
xóa, cần tạo lại trước khi Restore. Nếu đích có file cùng tên,
Restore từ chối ghi đè. File do GUI cũ chuyển vào Trash có thể
không có metadata mới. Các thao tác nhiều checkpoint chưa atomic.

Permanently Delete chỉ bỏ liên kết VTXFS, **không phải secure erase**;
các snapshot/CoW có thể còn giữ dữ liệu. Thao tác cần `--yes`.

## Search, Sort, Filter, Hide, Properties, Disk Space

```sh
tools/vtxfsctl "$IMG" search notes --in /
tools/vtxfsctl "$IMG" sort /work --by name
tools/vtxfsctl "$IMG" sort /work --by size
tools/vtxfsctl "$IMG" sort /work --by mtime
tools/vtxfsctl "$IMG" sort /work --by type
tools/vtxfsctl "$IMG" filter /work --ext .txt
tools/vtxfsctl "$IMG" hide /work/renamed.txt
tools/vtxfsctl "$IMG" unhide /work/.renamed.txt
tools/vtxfsctl "$IMG" readonly /work/renamed.txt
tools/vtxfsctl "$IMG" readonly /work/renamed.txt --off
tools/vtxfsctl "$IMG" disk-space
tools/vtxfsctl "$IMG" bitmap 0
```

Search duyệt thư mục đệ quy, không tự follow symlink. Sort/Filter chỉ
thay đổi thứ tự/kết quả in ra terminal. Hide dùng dấu chấm ở đầu tên,
không thêm flag ẩn trong inode. Readonly chỉnh mode metadata 0444/0644. Từ phiên bản này **vtxfsctl edit**
kiểm tra mode trước khi ghi, chặn mọi file thiếu cả ba bit write (0222),
bao gồm cùng inode truy cập bằng hardlink. Đây chỉ là quy tắc **cho CLI**,
không thay thế cơ chế DAC/ACL của VFS, và không chặn lời gọi trực tiếp vào
portable engine qua công cụ khác.

## Hard Link, Shortcut, Symlink, ZIP, Unzip

```sh
tools/vtxfsctl "$IMG" hardlink /work/renamed.txt /work/shared.txt
tools/vtxfsctl "$IMG" symlink /work/renamed.txt /quick-link
tools/vtxfsctl "$IMG" shortcut /work/renamed.txt /alias
tools/vtxfsctl "$IMG" readlink /quick-link
tools/vtxfsctl "$IMG" zip /work /work.zip
tools/vtxfsctl "$IMG" mkdir /extract
tools/vtxfsctl "$IMG" unzip /work.zip /extract
```

Hardlink tạo thêm một đường dẫn vào **cùng inode** (khác với copy). Việc
sửa/đổi mode qua tên nào cũng ảnh hưởng tên còn lại; xóa một tên giảm
`nlink`, không xóa nội dung khi vẫn còn tên khác. Hiện chỉ hỗ trợ hardlink
file thường, không hardlink thư mục/symlink. Mọi bước phải chạy trên
image thử trong /dev/shm trước.

Shortcut là symlink nằm **trong VTXFS**, không tạo file .desktop trên host.
Open symlink in target, không tự động mở nội dung đích.

ZIP hỗ trợ file/thư mục lồng nhau (tổng dữ liệu/file nén không vượt
8 MiB), chưa hỗ trợ nén symlink. Unzip chặn path tuyệt đối, `..`,
tên trùng, symlink độc hại, collision và ghi đè. Khi I/O lỗi giữa
chừng có thể còn kết quả giải nén một phần.

## Chạy kiểm thử (tạo image thử riêng trong RAM, tự dọn dẹp)

```sh
python3 tests/test_terminal_commands_extended.py
python3 tests/test_terminal_commands_negative.py
python3 tests/test_terminal_full_matrix.py
TMPDIR=/dev/shm bash tests/run.sh
```

Kết quả đã xác nhận: 11/11 nhóm mới (65 lần gọi terminal CLI);
các trường hợp Restore collision, ZIP traversal và duplicate đều bị
từ chối; 22/22 nhóm cũ (596 lệnh), P0 GCC/Clang đều PASS.

Giới hạn: chưa hỗ trợ chia sẻ cloud/Bluetooth/network, kéo-thả GUI,
secure erase, full ACL hoặc quyền thực thi của portable engine trực tiếp.
Kernel-native VFS có bài kiểm thử mount trong QEMU, xem
`docs/VTXFS_P0_P2_POSIX_REPAIR_2026-10-26.md`. Full transactional
hardlink/orphan recovery trên mọi subvolume vẫn chưa nghiệm thu.
