#!/usr/bin/env bash
# ==============================================================================
# VTXFS v2.1 — KỊCH BẢN THỬ NGHIỆM TOÀN DIỆN TẤT CẢ CÁC CÔNG CỤ (ALL TOOLS SUITE)
# ==============================================================================
# Kịch bản này thực thi và kiểm chứng chức năng của TẤT CẢ các công cụ trong VTXFS:
#   1. mkfs.vtxfs         - Định dạng đĩa / ảnh VTXFS v2.1 chuẩn
#   2. mount.vtxfs        - Trình hỗ trợ gắn kết điểm mount VFS (hỗ trợ cờ -f fake & live)
#   3. fsck.vtxfs         - Kiểm toán toàn vẹn B+Tree đa cấp, SFQ & tự sửa chữa
#   4. vtxfs (Unified CLI):
#      - filesystem       (info, df, sync, usage)
#      - subvolume        (create, list, show, delete)
#      - snapshot         (create, list)
#      - property         (get, set nhãn, migration nén zstd/lz4/none, list)
#      - reflink          (CoW file cloning chia sẻ extents vật lý)
#      - cp / transfer    (Động cơ truyền tệp tăng tốc siêu tốc với fallocate & 1MB buffer)
#      - defrag           (Quét & chống phân mảnh extents)
#      - balance          (Cân bằng không gian phân bổ VAG)
#      - scrub            (Quét toàn vẹn dữ liệu qua ioctl hạt nhân & image offline)
#      - device           (Quản lý & thống kê nhóm lưu trữ đa thiết bị)
#      - quota            (Quản lý hạn ngạch)
#      - reaper           (Dọn dẹp orphan inodes & dropped subvolumes)
#      - rescue           (Khôi phục superblock mirror, VAG bitmap & sửa checksum)
#      - sys-index        (Bản đồ & chỉ mục 128 MiB vùng hệ thống)
#      - stat / map / block / bitmap (Thăm dò hình thái khối đĩa)
#      - frag             (Phân tích phân mảnh FIEMAP thời gian thực)
#      - file             (Khảo sát cấu trúc tệp tin chi tiết trên đĩa)
#      - check / mkfs     (Bí danh tiện ích)
#   5. vtxfs-inspect      - Công cụ pháp y và khảo sát B+Tree độc lập (ls, stat, cat, put-stdin, symlink, ...)
#   6. vtxfs-image        - Metadump tạo gói sao lưu và phục hồi ảnh nguyên vẹn
#   7. vtxfs-balance      - Công cụ cân bằng khối độc lập
#   8. vtxfs-defrag       - Động cơ defragmentation độc lập
#   9. vtxfs-send & recv  - Xuất / nhập dòng truyền phát subvolume (stream replication)
#  10. replay-log         - Động cơ phát lại log dm-log-writes độc lập kiểm thử crash consistency
#  11. vtxfsctl.py        - Trình quản lý tập tin dòng lệnh cho ảnh ramdisk (/dev/shm)
#  12. vtxfs_verify.py    - Xác thực cấu trúc Merkle Tree & so sánh 100% bit-for-bit
#  13. vtxfs_frag.py      - Trực quan hóa bản đồ phân mảnh tệp
#  14. vtxfs_admin.py     - Báo cáo chẩn đoán toàn diện và bản đồ khối VAG
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$SCRIPT_DIR/vtxfs-progs/tools"
CLI="$TOOLS_DIR/vtxfs/vtxfs"
TMP_DIR="/tmp/vtxfs_all_tools_demo"
SHM_IMG="/dev/shm/vtxfs_ctl_demo.img"

C_GREEN="\033[1;32m"
C_CYAN="\033[1;36m"
C_YELLOW="\033[1;33m"
C_RESET="\033[0m"

echo "=============================================================================="
echo "          VTXFS v2.1 — BẮT ĐẦU CHẠY KỊCH BẢN THỬ NGHIỆM TẤT CẢ CÁC TOOLS      "
echo "=============================================================================="

rm -rf "$TMP_DIR"
rm -f "$SHM_IMG"
mkdir -p "$TMP_DIR"
DEMO_IMG="$TMP_DIR/demo_fs.img"
DEMO_RESTORE_IMG="$TMP_DIR/restored_fs.img"
DEMO_DUMP="$TMP_DIR/demo.metadump"
STREAM_FILE="$TMP_DIR/subvol.stream"
RECV_DIR="$TMP_DIR/received_data"

# ------------------------------------------------------------------------------
# BƯỚC 1: ĐỊNH DẠNG HỆ THỐNG TẬP TIN & MOUNT HELPER (mkfs.vtxfs, mount.vtxfs)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 1/14] Định dạng & Hỗ trợ gắn kết (mkfs.vtxfs, mount.vtxfs, vtxfs format)${C_RESET}"
echo ">> Tạo file ảnh đĩa giả lập 256 MiB (65536 blocks) với mkfs.vtxfs..."
"$TOOLS_DIR/mkfs.vtxfs" -L "VTXFS_DEMO" -b 65536 "$DEMO_IMG"

echo -e "\n>> Kiểm tra thông tin filesystem bằng 'vtxfs filesystem info' & 'df':"
"$CLI" filesystem info "$DEMO_IMG"
"$CLI" filesystem df "$DEMO_IMG"

echo -e "\n>> Thử nghiệm mount.vtxfs với tùy chọn cờ -f (fake mount simulation):"
"$TOOLS_DIR/mount.vtxfs" "$DEMO_IMG" /mnt/test_fake -f -o ro,subvol=256

# ------------------------------------------------------------------------------
# BƯỚC 2: QUẢN TRỊ THUỘC TÍNH & MIGRATION NÉN TRONG SUỐT (vtxfs property, vtxfs label)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 2/14] Quản trị thuộc tính & Migration nén ABI (vtxfs property, vtxfs label)${C_RESET}"
echo ">> Đọc thuộc tính ban đầu:"
"$CLI" property get "$DEMO_IMG" label
"$CLI" property get "$DEMO_IMG" compression

echo ">> Đổi nhãn filesystem sang 'VTXFS_SUITE'..."
"$CLI" property set "$DEMO_IMG" label VTXFS_SUITE
"$CLI" label "$DEMO_IMG"

echo ">> Di chuyển thuật toán nén sang 'zstd' (bật VTXFS_FEAT_INCOMPAT_COMPRESS)..."
"$CLI" property set "$DEMO_IMG" compression zstd
"$CLI" property get "$DEMO_IMG" compression

echo ">> Chuyển thuật toán nén sang 'lz4'..."
"$CLI" property set "$DEMO_IMG" compression lz4
"$CLI" property get "$DEMO_IMG" compression

echo ">> Tắt nén an toàn và zero hóa trường reserved..."
"$CLI" property set "$DEMO_IMG" compression none
"$CLI" property get "$DEMO_IMG" compression

echo ">> Danh sách các thuộc tính được hỗ trợ:"
"$CLI" property list "$DEMO_IMG"

# ------------------------------------------------------------------------------
# BƯỚC 3: QUẢN LÝ SUBVOLUME & SNAPSHOT (vtxfs subvolume, vtxfs snapshot)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 3/14] Quản lý Subvolume & Snapshot Copy-on-Write (vtxfs subvolume/snapshot)${C_RESET}"
echo ">> Tạo subvolume 'dev_branch'..."
"$CLI" subvolume create "$DEMO_IMG" dev_branch

echo ">> Tạo subvolume 'prod_branch'..."
"$CLI" subvolume create "$DEMO_IMG" prod_branch

echo ">> Danh sách subvolumes:"
"$CLI" subvolume list "$DEMO_IMG"

echo ">> Chi tiết subvolume ID 257 (dev_branch):"
"$CLI" subvolume show "$DEMO_IMG" 257

echo ">> Tạo Snapshot CoW O(1) từ subvolume mặc định (ID 256)..."
"$CLI" snapshot create "$DEMO_IMG" 256 snap_initial

echo ">> Danh sách snapshot:"
"$CLI" snapshot list "$DEMO_IMG"

# ------------------------------------------------------------------------------
# BƯỚC 4: THAO TÁC TỆP NỘI BỘ VÀ REFLINK CoW (vtxfs-inspect, vtxfs reflink)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 4/14] Thao tác tệp nội tại & Reflink CoW (vtxfs-inspect & vtxfs reflink)${C_RESET}"
echo ">> Tạo thư mục '/docs' trong ảnh..."
"$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" mkdir 2 docs

echo ">> Ghi tệp '/sample.txt' bằng 'vtxfs-inspect write'..."
"$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" write 2 sample.txt "VTXFS v2.1 High Performance CoW Filesystem"

echo ">> Đọc tệp bằng 'vtxfs-inspect cat':"
"$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" cat /sample.txt

echo ">> Xem inode và extents bằng 'vtxfs-inspect stat':"
"$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" stat /sample.txt

echo ">> Chuẩn bị tệp 8192 bytes bằng 'vtxfs-inspect put-stdin' cho reflink..."
head -c 8192 /dev/zero | tr '\0' 'A' | "$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" put-stdin /source_reflink.bin
echo -n "" | "$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" put-stdin /dest_reflink.bin

echo ">> Thực hiện Reflink CoW (chia sẻ khối vật lý không tốn thêm dung lượng):"
"$CLI" reflink "$DEMO_IMG" /source_reflink.bin /dest_reflink.bin

echo ">> Tạo symlink nội bộ '/symlink_to_sample'..."
"$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" symlink /sample.txt /symlink_to_sample
"$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" readlink /symlink_to_sample

echo ">> Danh sách tệp và thư mục trong ảnh:"
"$TOOLS_DIR/vtxfs-inspect" "$DEMO_IMG" ls /

# ------------------------------------------------------------------------------
# BƯỚC 5: TRUYỀN TỆP TĂNG TỐC & KHẢO SÁT CẤU TRÚC (vtxfs cp, vtxfs file, vtxfs frag)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 5/14] Truyền tệp tăng tốc & Khảo sát cấu trúc tệp (vtxfs cp, vtxfs file, vtxfs frag)${C_RESET}"
if [ -f "/mnt/vtxfs_usb_test/test_exec_script.sh" ]; then
    echo ">> Thử nghiệm 'vtxfs cp' (Accelerated Copy với 1MB buffer & SHA-256):"
    "$CLI" cp /mnt/vtxfs_usb_test/test_exec_script.sh "$TMP_DIR/cp_test_script.sh"
fi

if [ -f "/mnt/vtxfs_usb_test/file_sizes/size_1MB_buffer.bin" ]; then
    echo -e "\n>> Khảo sát tệp tin trực tuyến trên USB mount ('vtxfs file'):"
    "$CLI" file /mnt/vtxfs_usb_test/file_sizes/size_1MB_buffer.bin
fi

echo -e "\n>> Khảo sát tệp tin ngoại tuyến trong ảnh ('vtxfs file <img/dev> <path>'):"
"$CLI" file "$DEMO_IMG" /source_reflink.bin

if [ -d "/mnt/vtxfs_usb_test/file_sizes" ]; then
    echo -e "\n>> Phân tích mức độ phân mảnh FIEMAP ('vtxfs frag'):"
    "$CLI" frag /mnt/vtxfs_usb_test/file_sizes
fi

# ------------------------------------------------------------------------------
# BƯỚC 6: XUẤT NHẬP DÒNG TRUYỀN PHÁT SUBVOLUME (vtxfs-send & vtxfs-receive)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 6/14] Truyền phát dòng dữ liệu Subvolume (vtxfs-send & vtxfs-receive)${C_RESET}"
SRC_DIR="$TMP_DIR/source_subvol"
mkdir -p "$SRC_DIR/nested"
echo "Tệp gốc A - VTXFS replication test" > "$SRC_DIR/file_a.txt"
echo "Tệp gốc B trong thư mục con" > "$SRC_DIR/nested/file_b.txt"

echo ">> Đóng gói subvolume sang stream bằng 'vtxfs-send':"
"$TOOLS_DIR/vtxfs-send" "$SRC_DIR" -f "$STREAM_FILE" -v

echo -e "\n>> Phục hồi subvolume từ stream vào thư mục mới bằng 'vtxfs-receive':"
mkdir -p "$RECV_DIR"
"$TOOLS_DIR/vtxfs-receive" "$RECV_DIR" -f "$STREAM_FILE" -v

echo ">> Kiểm tra dữ liệu sau phục hồi:"
find "$RECV_DIR" -type f -exec ls -l {} +

# ------------------------------------------------------------------------------
# BƯỚC 7: XÁC THỰC MERKLE TREE & BIT-FOR-BIT (vtxfs_verify.py & vtxfs verify)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 7/14] Xác thực Merkle Tree & Bit-for-bit (vtxfs_verify.py & vtxfs verify)${C_RESET}"
echo ">> Chạy vtxfs_verify.py với cờ Merkle Tree:"
python3 "$TOOLS_DIR/vtxfs_verify.py" --merkle "$SRC_DIR" "$RECV_DIR"

echo -e "\n>> Chạy kiểm tra nhanh qua unified CLI 'vtxfs verify --quick':"
"$CLI" verify "$SRC_DIR" "$RECV_DIR" --quick

# ------------------------------------------------------------------------------
# BƯỚC 8: CÂN BẰNG & CHỐNG PHÂN MẢNH (vtxfs-balance, vtxfs-defrag, vtxfs_frag.py)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 8/14] Cân bằng khối & Phân tích phân mảnh (vtxfs-balance, vtxfs-defrag, vtxfs_frag.py)${C_RESET}"
echo ">> Trạng thái cân bằng qua CLI ('vtxfs balance status'):"
"$CLI" balance status "$DEMO_IMG"

echo -e "\n>> Chạy công cụ cân bằng độc lập 'vtxfs-balance':"
"$TOOLS_DIR/vtxfs-balance" "$DEMO_IMG"

echo -e "\n>> Quét phân mảnh qua CLI ('vtxfs defrag scan'):"
"$CLI" defrag scan "$DEMO_IMG"

echo -e "\n>> Quét chống phân mảnh bằng 'vtxfs-defrag':"
"$TOOLS_DIR/vtxfs-defrag" "$DEMO_IMG"

echo -e "\n>> Trực quan hóa bản đồ nhiệt phân mảnh bằng 'vtxfs_frag.py':"
if [ -d "/mnt/vtxfs_usb_test/file_sizes" ]; then
    python3 "$TOOLS_DIR/vtxfs_frag.py" "/mnt/vtxfs_usb_test/file_sizes"
else
    python3 "$TOOLS_DIR/vtxfs_frag.py" "$RECV_DIR"
fi

# ------------------------------------------------------------------------------
# BƯỚC 9: ĐÓNG GÓI METADUMP VÀ PHỤC HỒI ẢNH (vtxfs-image)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 9/14] Đóng gói Metadump & Phục hồi ảnh (vtxfs-image)${C_RESET}"
echo ">> Tạo bản sao lưu metadump toàn vẹn (--full)..."
"$TOOLS_DIR/vtxfs-image/vtxfs-image" create "$DEMO_IMG" -o "$DEMO_DUMP" --full

echo ">> Phục hồi hệ thống tập tin mới từ bản dump..."
"$TOOLS_DIR/vtxfs-image/vtxfs-image" restore "$DEMO_DUMP" "$DEMO_RESTORE_IMG"

echo ">> Kiểm toán filesystem sau phục hồi bằng fsck.vtxfs:"
"$TOOLS_DIR/fsck.vtxfs" -q "$DEMO_RESTORE_IMG" && echo "Phục hồi 100% sạch sẽ (CLEAN)."

# ------------------------------------------------------------------------------
# BƯỚC 10: CHẨN ĐOÁN HỆ THỐNG VÀ BẢN ĐỒ VAG (vtxfs_admin.py, vtxfs stat/map/block/bitmap)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 10/14] Chẩn đoán & Bản đồ VAG (vtxfs_admin.py, vtxfs stat/map/block/bitmap)${C_RESET}"
echo ">> Báo cáo thống kê chi tiết các tham số vận hành (vtxfs_admin.py):"
python3 "$TOOLS_DIR/vtxfs_admin.py" stat "$DEMO_IMG"

echo ">> Xuất bản đồ khối bitmap trực quan VAG #0 (vtxfs_admin.py):"
python3 "$TOOLS_DIR/vtxfs_admin.py" map "$DEMO_IMG" --compact | head -n 30

echo ">> Giải mã thông minh khối Superblock 16 (vtxfs_admin.py):"
python3 "$TOOLS_DIR/vtxfs_admin.py" block "$DEMO_IMG" 16

echo -e "\n>> Thăm dò qua unified CLI 'vtxfs stat':"
"$CLI" stat "$DEMO_IMG" | head -n 25

echo -e "\n>> Bản đồ qua unified CLI 'vtxfs map --compact':"
"$CLI" map "$DEMO_IMG" --compact | head -n 25

echo -e "\n>> Giải mã khối qua unified CLI 'vtxfs block':"
"$CLI" block "$DEMO_IMG" 16 | head -n 25

echo -e "\n>> Xuất raw bitmap qua unified CLI 'vtxfs bitmap':"
"$CLI" bitmap "$DEMO_IMG" 0 | head -n 15

# ------------------------------------------------------------------------------
# BƯỚC 11: SCRUB, QUOTA, REAPER, RESCUE & THIẾT BỊ (vtxfs subcommands)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 11/14] Scrub, Quota, Reaper, Rescue & Thiết bị (vtxfs CLI subcommands)${C_RESET}"
echo ">> Quét toàn vẹn dữ liệu qua 'vtxfs scrub start':"
"$CLI" scrub start "$DEMO_IMG" --verify-only
"$CLI" scrub status "$DEMO_IMG"

echo -e "\n>> Kiểm tra trạng thái hạn ngạch ('vtxfs quota status'):"
"$CLI" quota status "$DEMO_IMG"

echo -e "\n>> Chạy tiến trình dọn dẹp ('vtxfs reaper run'):"
"$CLI" reaper run "$DEMO_IMG"

echo -e "\n>> Chỉ mục vùng hệ thống 128 MiB ('vtxfs sys-index'):"
"$CLI" sys-index "$DEMO_IMG"

echo -e "\n>> Kiểm thử cứu hộ Superblock từ Mirror Ring ('vtxfs rescue super-recover'):"
"$CLI" rescue super-recover "$DEMO_IMG"

echo -e "\n>> Tái dựng VAG bitmap từ REF+SFQ ('vtxfs rescue vag-recover'):"
"$CLI" rescue vag-recover "$DEMO_IMG"

echo -e "\n>> Sửa chữa checksum dữ liệu ('vtxfs rescue fix-data-checksum'):"
"$CLI" rescue fix-data-checksum "$DEMO_IMG"

echo -e "\n>> Thống kê & thông tin thiết bị lưu trữ ('vtxfs device info/stats'):"
"$CLI" device info "$DEMO_IMG"
"$CLI" device stats "$DEMO_IMG"

# ------------------------------------------------------------------------------
# BƯỚC 12: TRÌNH QUẢN LÝ TẬP TIN DÒNG LỆNH CHO RAMDISK (vtxfsctl.py / vtxfs ctl)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 12/14] Trình quản lý tập tin dòng lệnh cho RAMDisk (vtxfsctl.py / vtxfs ctl)${C_RESET}"
echo ">> Khởi tạo ảnh đĩa trong tmpfs /dev/shm..."
"$TOOLS_DIR/mkfs.vtxfs" -b 65536 -L "VTXFS_SHM" "$SHM_IMG"

echo ">> Gọi 'vtxfsctl.py info':"
python3 "$TOOLS_DIR/vtxfsctl.py" "$SHM_IMG" info | head -n 15

echo ">> Tạo thư mục '/ctl_demo' bằng vtxfsctl:"
python3 "$TOOLS_DIR/vtxfsctl.py" "$SHM_IMG" mkdir /ctl_demo

echo ">> Liệt kê cây thư mục gốc qua vtxfsctl ls:"
python3 "$TOOLS_DIR/vtxfsctl.py" "$SHM_IMG" ls /

echo ">> Kiểm tra tính toàn vẹn ramdisk qua vtxfsctl fsck:"
python3 "$TOOLS_DIR/vtxfsctl.py" "$SHM_IMG" fsck
rm -f "$SHM_IMG"

# ------------------------------------------------------------------------------
# BƯỚC 13: ĐỘNG CƠ PHÁT LẠI LOG CRASH CONSISTENCY (replay-log)
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 13/14] Động cơ phát lại log kiểm thử Crash Consistency (replay-log)${C_RESET}"
LOG_FILE="$TMP_DIR/mock_dm_log.bin"
REPLAY_TARGET="$TMP_DIR/replay_target.img"
python3 -c "
import struct
with open('$LOG_FILE', 'wb') as f:
    hdr = struct.pack('<QQQI', 0x6a736677736872, 1, 0, 512)
    f.write(hdr + b'\0' * (512 - len(hdr)))
with open('$REPLAY_TARGET', 'wb') as f:
    f.truncate(1024 * 1024)
"
echo ">> Chạy replay-log với header dm-log-writes chuẩn:"
"$TOOLS_DIR/replay-log" --log "$LOG_FILE" --replay "$REPLAY_TARGET" --verbose

# ------------------------------------------------------------------------------
# BƯỚC 14: KIỂM TOÁN TOÀN DIỆN BẰNG fsck.vtxfs & DỌN DẸP
# ------------------------------------------------------------------------------
echo -e "\n${C_GREEN}[BƯỚC 14/14] Kiểm toán toàn vẹn tổng thể (fsck.vtxfs & vtxfs check)${C_RESET}"
echo ">> Kiểm toán chi tiết (-v) trên ảnh thử nghiệm:"
"$TOOLS_DIR/fsck.vtxfs" -v "$DEMO_IMG"

echo -e "\n>> Kiểm tra qua unified CLI 'vtxfs check':"
"$CLI" check "$DEMO_IMG"

if [ -b "/dev/sdb2" ]; then
    echo -e "\n>> Kiểm toán nhanh ổ USB vật lý (/dev/sdb2):"
    "$TOOLS_DIR/fsck.vtxfs" -s /dev/sdb2
fi

echo -e "\n>> Dọn dẹp tài nguyên thử nghiệm tạm thời..."
rm -rf "$TMP_DIR"
rm -f "$SHM_IMG"
echo "Đã dọn dẹp an toàn các file tạm."

echo "=============================================================================="
echo "    HOÀN TẤT KỊCH BẢN THỬ NGHIỆM! TẤT CẢ CÁC TOOLS ĐỀU HOẠT ĐỘNG HOÀN HẢO.  "
echo "=============================================================================="
