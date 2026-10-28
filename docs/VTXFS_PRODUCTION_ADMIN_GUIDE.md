# VTXFS v1.0.0 Production Administration & Deployment Guide

## 1. Giới thiệu
VTXFS (Virtual Tree eXtended Filesystem) là hệ thống tệp Linux 64-bit hiện đại thế hệ mới, hoạt động theo cơ chế Copy-on-Write (CoW) B+Tree với toàn vẹn dữ liệu và metadata dựa trên hàm băm mật mã CRC32C và BLAKE3-128.

Hệ thống được thiết kế với:
- **Crash Consistency tuyệt đối:** Cấu trúc Checkpoint Descriptor (CPD) vòng đôi (Primary/Mirror) luân phiên 8 slot, kết hợp FUA (Force Unit Access) bảo vệ trước sự cố mất điện.
- **Tự phục hồi (Self-Healing):** Cơ chế Scrub online quét và đối chiếu checksum từng block dữ liệu đối chiếu với mảng checksum lưu trong extent.
- **Sửa chữa ngoại tuyến đa cấp (`fsck.vtxfs --repair`):** Khôi phục Superblock mirror, VAG allocation bitmaps, chỉ mục tham chiếu REF/SFQ và reverse-mapping `ALLOC_BY_VBLK`.
- **Sao lưu toàn vẹn (`vtxfs-image`):** Hỗ trợ tạo ảnh metadata/data nén hoặc giải nén, phục hồi byte-exact.

---

## 2. Cài đặt hệ thống (Production Installation)

### 2.1. Yêu cầu hệ thống
- Linux Kernel: >= 6.1 LTS (hỗ trợ đầy đủ trên 6.6 LTS, 6.12 LTS, 7.x).
- CPU: x86_64 hoặc aarch64.
- Kích thước khối (Block size): 4096 bytes (4 KiB) mặc định.
- Dung lượng phân vùng tối thiểu: 256 MiB (65,536 blocks).

### 2.2. Biên dịch & Cài đặt
```bash
# 1. Biên dịch kernel module và các tiện ích userspace
make all

# 2. Cài đặt module kernel vào hệ thống
sudo make modules_install
sudo depmod -a

# 3. Cài đặt công cụ quản trị vào /usr/local/sbin và /usr/local/bin
sudo make install

# (Tùy chọn) Đăng ký DKMS để tự động cập nhật khi nâng cấp kernel:
sudo dkms add .
sudo dkms build vtxfs/1.0.0
sudo dkms install vtxfs/1.0.0
```

---

## 3. Quản trị phân vùng & Định dạng (Formatting)

### 3.1. Định dạng ổ đĩa hoặc phân vùng
Sử dụng lệnh `mkfs.vtxfs`:
```bash
# Định dạng phân vùng với nhãn volume
sudo mkfs.vtxfs -L "DATA_STORE" /dev/sdX1

# Tùy chọn chỉ định UUID thủ công (32 ký tự hex)
sudo mkfs.vtxfs -L "BACKUP" -U 12345678-1234-1234-1234-123456789abc /dev/sdX2
```

### 3.2. Kiểm tra trước khi Mount
```bash
# Kiểm tra nhanh trạng thái toàn vẹn (quiet verify)
sudo fsck.vtxfs -q /dev/sdX1

# Kiểm tra chi tiết và hiển thị thông tin cây
sudo fsck.vtxfs -v /dev/sdX1
```

---

## 4. Mount & Tùy chọn VFS (Mount Operations)

### 4.1. Mount thủ công
```bash
# Mount subvolume mặc định (subvol_id = 256)
sudo mount -t vtxfs /dev/sdX1 /mnt/vtxfs

# Mount chỉ đọc (Read-Only)
sudo mount -t vtxfs -o ro /dev/sdX1 /mnt/vtxfs

# Mount một snapshot hoặc subvolume cụ thể
sudo mount -t vtxfs -o subvol=257 /dev/sdX1 /mnt/vtxfs-snap
```

### 4.2. Cấu hình `/etc/fstab`
```fstab
# /etc/fstab entry
UUID=550e8400-e29b-41d4-a716-446655440000  /mnt/data  vtxfs  defaults,noatime  0  2
```

---

## 5. Quản lý Snapshot & Subvolume

VTXFS hỗ trợ subvolume và snapshot O(1) theo cơ chế CoW phân tầng:
```bash
# 1. Xem danh sách subvolume
sudo vtxfs subvolume list /mnt/vtxfs

# 2. Tạo subvolume mới
sudo vtxfs subvolume create /mnt/vtxfs/projects

# 3. Tạo snapshot Read-Only cho mục đích sao lưu
sudo vtxfs snapshot create -r /mnt/vtxfs /mnt/vtxfs/.snapshots/backup_$(date +%F)
```

---

## 6. Bảo trì định kỳ & Kiểm tra dữ liệu (Scrubbing)

### 6.1. Chạy Scrub Online (Không cần unmount)
Hệ thống hỗ trợ quét online thông qua ioctl `VTX_IOC_SCRUB`:
```bash
# Quét toàn bộ khối dữ liệu và metadata để phát hiện lỗi bit-rot
sudo vtxfs scrub start /mnt/vtxfs

# Kiểm tra trạng thái tiến trình scrub
sudo vtxfs scrub status /mnt/vtxfs
```

---

## 7. Khôi phục lỗi & Cứu hộ dữ liệu (Disaster Recovery)

### 7.1. Chẩn đoán khi có sự cố
Nếu hệ thống bị mất điện đột ngột hoặc kernel báo cờ `VTXFS_CP_FLAG_FSCK_REQUIRED`:
```bash
# Chạy fsck thử nghiệm không ghi đĩa (Dry-run)
sudo fsck.vtxfs --dry-run /dev/sdX1
```

### 7.2. Sửa chữa tự động (`fsck.vtxfs --repair`)
```bash
# Tự động sửa chữa: tái đồng bộ CPD/SB mirror, sửa VAG bitmap và REF tree
sudo fsck.vtxfs --repair -y /dev/sdX1
```

### 7.3. Sao lưu phục hồi ảnh đĩa (`vtxfs-image`)
```bash
# Tạo ảnh backup toàn bộ metadata và dữ liệu
sudo vtxfs-image create --full --compress /dev/sdX1 /path/to/backup.vimg

# Phục hồi ảnh sang thiết bị mới
sudo vtxfs-image restore /path/to/backup.vimg /dev/sdY1
```
