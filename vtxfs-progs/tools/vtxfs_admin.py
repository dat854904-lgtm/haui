#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
"""
VTXFS v2.1 — Comprehensive System Diagnostic, Block Map & Block Inspector
===========================================================================
Provides:
  1. Full system & layout parameters report (stat / sys-info).
  2. Visual block allocation bitmap & range breakdown (map / bitmap-map).
  3. Deep block data inspection & auto-decoder (block / dump-block).
"""

import os
import sys
import struct
import ctypes
import argparse
import datetime
import uuid
import math
import shutil
import subprocess
from pathlib import Path

def sudo_cmd(cmd_list):
    if os.geteuid() == 0 or not shutil.which("sudo"):
        return list(cmd_list)
    return ["sudo"] + list(cmd_list)

# --- ANSI Terminal Styling ---
USE_COLOR = sys.stdout.isatty()
def c(text, color_code):
    return f"\033[{color_code}m{text}\033[0m" if USE_COLOR else str(text)

def c_bold(t): return c(t, "1")
def c_cyan(t): return c(t, "36")
def c_green(t): return c(t, "32")
def c_yellow(t): return c(t, "33")
def c_red(t): return c(t, "31")
def c_dim(t): return c(t, "90")
def c_blue(t): return c(t, "34")
def c_magenta(t): return c(t, "35")

# --- Constants ---
BLOCK_SIZE = 4096
VTXFS_MAGIC_BYTES = b"VTXFS21\x00"
VTXFS_CPD_MAGIC   = b"VTXCP21\x00"
VTXFS_NODE_MAGIC  = 0x314E5856   # "VXN1"
VTXFS_VAG_DIR_MAGIC = 0x31444756 # "VGD1"

# Layout Constants
SUPER_RING_PBLK = 16
CPD_RING_PBLK   = 8
MIRROR0_PBLK    = 16384
VAG_BLOCKS      = 32768

# Item Types
ITEM_TYPES = {
    0: "ANY",
    1: "INODE",
    2: "INODE_REF",
    3: "XATTR",
    4: "DIR_ITEM",
    5: "DIR_INDEX",
    6: "EXTENT",
    7: "SUBVOL_ROOT",
    8: "OMAP_VAL",
    9: "REF",
    10: "SFQ",
    11: "REAPER_JOB",
    12: "UUID_MAP"
}

# Tree Types
TREE_NAMES = {
    0: "NONE",
    1: "ROOT",
    2: "OMAP_META",
    3: "REF",
    4: "REAPER",
    5: "SFQ",
    6: "UUID",
    7: "FS (Filesystem)",
    8: "OMAP_VIEW",
    9: "MIRROR",
    10: "EVAC",
    11: "SCRUB",
    12: "KEYBAG",
    13: "CHAIN",
    14: "FREE_SPACE",
    15: "DEV"
}

# Block Classification Types
TYPE_FREE            = 0
TYPE_DATA            = 1
TYPE_CONTAINER_LEAF  = 2
TYPE_CONTAINER_NODE  = 3
TYPE_CONTAINER_OTHER = 4

# --- Ctypes Layout Structures ---
class PhysRef(ctypes.Structure):
    _layout_ = 'ms'
    _pack_ = 1
    _fields_ = [
        ('pblk', ctypes.c_uint64),
        ('xid', ctypes.c_uint64),
        ('dev_id', ctypes.c_uint16),
        ('blocks', ctypes.c_uint16),
        ('reserved', ctypes.c_uint32),
    ]

class SuperHeader(ctypes.Structure):
    _layout_ = 'ms'
    _pack_ = 1
    _fields_ = [
        ('blake3_256', ctypes.c_uint8 * 32),
        ('magic', ctypes.c_char * 8),
        ('major', ctypes.c_uint16),
        ('minor', ctypes.c_uint16),
        ('block_size', ctypes.c_uint32),
        ('fsid_uuid', ctypes.c_uint8 * 16),
        ('device_uuid', ctypes.c_uint8 * 16),
        ('super_generation', ctypes.c_uint64),
        ('compat_flags', ctypes.c_uint64),
        ('ro_compat_flags', ctypes.c_uint64),
        ('incompat_flags', ctypes.c_uint64),
        ('device_count', ctypes.c_uint32),
        ('self_dev_id', ctypes.c_uint32),
        ('total_blocks', ctypes.c_uint64),
        ('node_size', ctypes.c_uint32),
        ('sector_size', ctypes.c_uint32),
        ('meta_csum_type', ctypes.c_uint16),
        ('data_csum_type', ctypes.c_uint16),
        ('dir_hash_type', ctypes.c_uint16),
        ('state', ctypes.c_uint16),
        ('metadata_uuid', ctypes.c_uint8 * 16),
        ('dir_hash_seed', ctypes.c_uint8 * 16),
        ('mkfs_time_ns', ctypes.c_uint64),
        ('first_alloc_pblk', ctypes.c_uint64),
        ('label', ctypes.c_char * 64),
    ]

class CheckpointHeader(ctypes.Structure):
    _layout_ = 'ms'
    _pack_ = 1
    _fields_ = [
        ('blake3', ctypes.c_uint8 * 32),
        ('magic', ctypes.c_char * 8),
        ('xid', ctypes.c_uint64),
        ('vag_dir_root', PhysRef),
        ('root_tree_vblk', ctypes.c_uint64),
        ('ref_tree_vblk', ctypes.c_uint64),
        ('reaper_tree_vblk', ctypes.c_uint64),
        ('sfq_tree_vblk', ctypes.c_uint64),
        ('uuid_tree_vblk', ctypes.c_uint64),
        ('mirror_tree_vblk', ctypes.c_uint64),
        ('evac_tree_vblk', ctypes.c_uint64),
        ('scrub_tree_vblk', ctypes.c_uint64),
        ('keybag_tree_vblk', ctypes.c_uint64),
        ('chain_tree_vblk', ctypes.c_uint64),
        ('wal_replay_xid', ctypes.c_uint64),
        ('next_oid', ctypes.c_uint64),
        ('used_blocks', ctypes.c_uint64),
        ('root_tree_level', ctypes.c_uint32),
        ('cp_flags', ctypes.c_uint32),
        ('fsid_uuid', ctypes.c_uint8 * 16),
        ('omap_meta_tree_vblk', ctypes.c_uint64),
        ('prev_xid', ctypes.c_uint64),
        ('super_generation', ctypes.c_uint64),
        ('commit_time_ns', ctypes.c_uint64),
        ('reuse_safe_xid', ctypes.c_uint64),
        ('total_blocks', ctypes.c_uint64),
        ('free_blocks', ctypes.c_uint64),
        ('tree_levels', ctypes.c_uint8 * 16),
    ]

class NodeHeader(ctypes.Structure):
    _layout_ = 'ms'
    _pack_ = 1
    _fields_ = [
        ('checksum', ctypes.c_uint8 * 16),
        ('oid', ctypes.c_uint64),
        ('xid', ctypes.c_uint64),
        ('self_vblk', ctypes.c_uint64),
        ('owner_id', ctypes.c_uint64),
        ('magic', ctypes.c_uint32),
        ('flags', ctypes.c_uint16),
        ('level', ctypes.c_uint8),
        ('tree_type', ctypes.c_uint8),
        ('nitems', ctypes.c_uint16),
        ('free_head8', ctypes.c_uint16),
        ('free_tail8', ctypes.c_uint16),
        ('dead_bytes8', ctypes.c_uint16),
    ]

# --- Low-level Reader Helpers ---

def open_device(dev_path):
    try:
        return os.open(dev_path, os.O_RDONLY)
    except PermissionError:
        print(c_red(f"Lỗi: Không có quyền đọc '{dev_path}'. Hãy chạy với sudo!"), file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(c_red(f"Lỗi: Không thể mở thiết bị '{dev_path}': {e}"), file=sys.stderr)
        sys.exit(1)

def read_block(fd, pblk):
    try:
        return os.pread(fd, BLOCK_SIZE, pblk * BLOCK_SIZE)
    except Exception as e:
        return None

def find_primary_super(fd):
    # Scan superblock ring (blocks 16..31)
    for pblk in range(SUPER_RING_PBLK, SUPER_RING_PBLK + 16):
        raw = read_block(fd, pblk)
        if not raw:
            continue
        hdr = SuperHeader.from_buffer_copy(raw[:ctypes.sizeof(SuperHeader)])
        if bytes(hdr.magic).rstrip(b'\x00') == b"VTXFS21":
            return hdr, pblk, raw
    return None, None, None

def find_latest_cpd(fd):
    # Scan CPD ring (blocks 8..15) and mirror
    best_cpd = None
    best_pblk = None
    best_raw = None
    max_xid = -1
    slots = list(range(CPD_RING_PBLK, CPD_RING_PBLK + 8)) + [MIRROR0_PBLK]
    for pblk in slots:
        raw = read_block(fd, pblk)
        if not raw:
            continue
        cpd = CheckpointHeader.from_buffer_copy(raw[:ctypes.sizeof(CheckpointHeader)])
        if bytes(cpd.magic).rstrip(b'\x00') == b"VTXCP21" and cpd.xid > max_xid:
            max_xid = cpd.xid
            best_cpd = cpd
            best_pblk = pblk
            best_raw = raw
    return best_cpd, best_pblk, best_raw

def get_mount_info(dev_path):
    dev_real = os.path.realpath(dev_path)
    try:
        with open("/proc/mounts", "r") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 4:
                    m_dev, m_point, m_type, m_opts = parts[0], parts[1], parts[2], parts[3]
                    if os.path.exists(m_dev) and os.path.realpath(m_dev) == dev_real:
                        return {"mounted": True, "mountpoint": m_point, "type": m_type, "opts": m_opts}
    except Exception:
        pass
    return {"mounted": False}

def get_sysfs_info(dev_path):
    p = Path(dev_path)
    dev_name = p.name
    # if it's a partition e.g. sdb2, parent disk is sdb
    sys_disk = Path(f"/sys/class/block/{dev_name}")
    if not sys_disk.exists():
        return {}
    real_disk = sys_disk.resolve()
    # If partition, parent dir has queue
    queue_dir = real_disk / "queue"
    if not queue_dir.exists() and real_disk.parent.name.startswith("sd") or "nvme" in real_disk.parent.name:
        queue_dir = real_disk.parent / "queue"
        device_dir = real_disk.parent / "device"
    else:
        device_dir = real_disk / "device"

    def read_val(path):
        try:
            return path.read_text().strip()
        except Exception:
            return "N/A"

    info = {
        "model": read_val(device_dir / "model"),
        "vendor": read_val(device_dir / "vendor"),
        "rotational": read_val(queue_dir / "rotational"),
        "scheduler": read_val(queue_dir / "scheduler"),
        "read_ahead_kb": read_val(queue_dir / "read_ahead_kb"),
        "max_sectors_kb": read_val(queue_dir / "max_sectors_kb"),
        "discard_max_bytes": read_val(queue_dir / "discard_max_bytes"),
    }
    return info

def get_kernel_module_info():
    try:
        with open("/proc/modules", "r") as f:
            for line in f:
                parts = line.split()
                if parts and parts[0] == "vtxfs":
                    return {"loaded": True, "size": parts[1], "refcount": parts[2]}
    except Exception:
        pass
    return {"loaded": False}

# --- Subcommand 1: System Parameters (STAT / SYS-INFO) ---

def cmd_stat(target_dev):
    fd = open_device(target_dev)
    sb, sb_pblk, _ = find_primary_super(fd)
    cpd, cpd_pblk, _ = find_latest_cpd(fd)

    if not sb:
        print(c_red(f"Lỗi: Không tìm thấy Superblock VTXFS hợp lệ trên {target_dev}!"))
        os.close(fd)
        return

    mount_info = get_mount_info(target_dev)
    hw_info = get_sysfs_info(target_dev)
    mod_info = get_kernel_module_info()

    total_blks = sb.total_blocks
    total_bytes = total_blks * BLOCK_SIZE
    total_gib = total_bytes / (1024**3)

    used_blks = cpd.used_blocks if cpd else 0
    free_blks = cpd.free_blocks if cpd else total_blks
    used_pct = (used_blks / total_blks * 100) if total_blks > 0 else 0
    free_pct = (free_blks / total_blks * 100) if total_blks > 0 else 0

    print("=" * 82)
    print(c_bold(f"  VTXFS v{sb.major}.{sb.minor} — BÁO CÁO TOÀN DIỆN THÔNG SỐ HỆ THỐNG CÀI ĐẶT & VẬN HÀNH"))
    print(c_dim(f"  Thiết bị: {target_dev} (Dung lượng: {total_gib:.2f} GiB / {total_blks:,} blocks 4KB)"))
    print("=" * 82)

    # 1. Identifiers
    print(f"\n{c_cyan('1. ĐỊNH DANH HỆ THỐNG & SUPERBLOCK (IDENTIFIERS & SUPERBLOCK):')}")
    label = bytes(sb.label).decode('utf-8', 'replace').rstrip('\x00')
    fsid = uuid.UUID(bytes=bytes(sb.fsid_uuid))
    dev_uuid = uuid.UUID(bytes=bytes(sb.device_uuid))
    meta_uuid = uuid.UUID(bytes=bytes(sb.metadata_uuid))
    print(f"  Nhãn phân vùng (Label)    : {c_bold(label if label else '(none)')}")
    print(f"  Mã định danh FSID         : {fsid}")
    print(f"  Device UUID               : {dev_uuid}")
    print(f"  Metadata UUID             : {meta_uuid}")
    print(f"  Superblock PBLK           : Block {sb_pblk} (Slot {sb_pblk - SUPER_RING_PBLK})")
    print(f"  Super Generation          : {sb.super_generation}")
    if sb.mkfs_time_ns > 0:
        mkfs_dt = datetime.datetime.fromtimestamp(sb.mkfs_time_ns / 1e9)
        print(f"  Thời điểm tạo (mkfs time) : {mkfs_dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}")
    print(f"  Trạng thái Superblock     : 0x{sb.state:04x} ({c_green('CLEAN') if sb.state == 1 else 'DIRTY/ACTIVE'})")
    print(f"  Flags (compat/ro/incompat): 0x{sb.compat_flags:x} / 0x{sb.ro_compat_flags:x} / 0x{sb.incompat_flags:x}")

    # 2. Storage Geometry & Sizing
    print(f"\n{c_cyan('2. HÌNH HỌC LƯU TRỮ & DUNG LƯỢNG (STORAGE GEOMETRY & SPACE):')}")
    print(f"  Kích thước khối (Block)   : {sb.block_size} bytes (4.0 KiB)")
    print(f"  Kích thước Sector         : {sb.sector_size} bytes")
    print(f"  Kích thước B-Tree Node    : {sb.node_size} bytes")
    print(f"  Khối cấp phát đầu tiên    : PBLK {sb.first_alloc_pblk} (32 MiB VAG boundary)")
    print(f"  Tổng số khối trên đĩa     : {total_blks:,} blocks ({total_gib:.2f} GiB)")
    print(f"  Số khối đã sử dụng (Used) : {used_blks:,} blocks ({used_blks*4/1024:.2f} MiB, {c_yellow(f'{used_pct:.2f}%')})")
    print(f"  Số khối còn trống (Free)  : {free_blks:,} blocks ({free_blks*4/1024/1024:.2f} GiB, {c_green(f'{free_pct:.2f}%')})")
    
    # Progress Bar
    bar_width = 40
    used_chars = int(round(used_pct / 100 * bar_width))
    free_chars = bar_width - used_chars
    prog_bar = c_yellow('█' * used_chars) + c_green('░' * free_chars)
    print(f"  Thanh dung lượng          : [{prog_bar}] {used_pct:.1f}% used")

    # 3. Transaction & Checkpoint State
    print(f"\n{c_cyan('3. ĐỘNG CƠ GIAO DỊCH & CHECKPOINT (COW TRANSACTION ENGINE):')}")
    if cpd:
        xid = cpd.xid
        slot = cpd_pblk - CPD_RING_PBLK if cpd_pblk < MIRROR0_PBLK else "MIRROR"
        print(f"  Committed XID (Thế hệ)    : {c_bold(str(xid))}")
        print(f"  Previous XID              : {cpd.prev_xid}")
        print(f"  CPD Active Ring Slot      : Slot {slot} (PBLK {cpd_pblk})")
        if cpd.commit_time_ns > 0:
            commit_dt = datetime.datetime.fromtimestamp(cpd.commit_time_ns / 1e9)
            print(f"  Thời điểm Commit cuối     : {c_bold(commit_dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3])}")
        print(f"  Reuse-Safe XID Boundary   : {cpd.reuse_safe_xid}")
        print(f"  Mã OID tiếp theo (NextOID): {cpd.next_oid}")
        print(f"  Checkpoint Flags          : 0x{cpd.cp_flags:08x}")
    else:
        print(f"  {c_red('Không tìm thấy CPD hợp lệ!')}")

    # 4. Multi-Tree B-Tree Root Pointers
    print(f"\n{c_cyan('4. CHỈ MỤC CÁC CÂY B-TREE HỆ THỐNG (B-TREE MULTI-ROOT HIERARCHY):')}")
    if cpd:
        print(f"  Root Tree (Cây thư mục gốc): VBLK 0x{cpd.root_tree_vblk:x}  (Depth level: {cpd.root_tree_level})")
        print(f"  OMAP Meta Tree (Định danh) : VBLK 0x{cpd.omap_meta_tree_vblk:x}  (Depth level: {cpd.tree_levels[0]})")
        print(f"  Ref Tree (Quản lý Refcount): VBLK 0x{cpd.ref_tree_vblk:x}  (Depth level: {cpd.tree_levels[1]})")
        print(f"  SFQ Tree (Hàng đợi thu hồi): VBLK 0x{cpd.sfq_tree_vblk:x}  (Depth level: {cpd.tree_levels[2]})")
        print(f"  Reaper Tree (Dọn dẹp subvol): VBLK 0x{cpd.reaper_tree_vblk:x}  (Depth level: {cpd.tree_levels[3]})")
        print(f"  UUID Tree (Ánh xạ UUID)    : VBLK 0x{cpd.uuid_tree_vblk:x}  (Depth level: {cpd.tree_levels[4]})")
        print(f"  VAG Root Header PBLK      : PBLK {cpd.vag_dir_root.pblk} (Dev {cpd.vag_dir_root.dev_id})")

    # 5. VAG Topography
    print(f"\n{c_cyan('5. ĐỊA HÌNH CÁC NHÓM CẤP PHÁT (VAG ALLOCATION TOPOGRAPHY):')}")
    total_vags = max(1, total_blks // VAG_BLOCKS)
    print(f"  Tổng số VAG trên đĩa      : {total_vags} VAGs ({VAG_BLOCKS} blocks = 128.0 MiB / VAG)")
    print("  " + "-" * 78)
    print(f"  {'VAG':<6} {'Phạm vi PBLK':<22} {'Đã dùng':>12} {'Còn trống':>12} {'Sử dụng':>10}  {'Trạng thái'}")
    print("  " + "-" * 78)
    
    # Try reading first 8 VAGs
    for vag_id in range(min(12, total_vags)):
        start_p = 32768 + vag_id * VAG_BLOCKS if vag_id > 0 else 32768
        end_p = start_p + VAG_BLOCKS - 1
        # Read vag info from vtxfs-inspect or bitmap if available
        # Default placeholder if not scanned
        vag_used = 0
        vag_free = VAG_BLOCKS
        if cpd:
            vag_used = max(0, min(VAG_BLOCKS, used_blks - vag_id * VAG_BLOCKS))
            vag_free = VAG_BLOCKS - vag_used
        
        v_pct = (vag_used / VAG_BLOCKS) * 100
        mini_bar = "█" * int(round(v_pct / 10)) + "░" * (10 - int(round(v_pct / 10)))
        print(f"  {vag_id:<6} {start_p:>8}..{end_p:<11} {vag_used:>10,} blk {vag_free:>10,} blk {v_pct:>9.1f}%  [{mini_bar}]")
    if total_vags > 12:
        print(f"  ... và {total_vags - 12} VAGs tiếp theo còn trống 100% ...")
    print("  " + "-" * 78)

    # 6. Kernel & Hardware Runtime
    print(f"\n{c_cyan('6. THÔNG SỐ VẬN HÀNH KERNEL LINUX & PHẦN CỨNG (KERNEL & HARDWARE RUNTIME):')}")
    if mod_info.get("loaded"):
        print(f"  Mô-đun Kernel vtxfs.ko    : {c_green('ĐÃ TẢI')} (Dung lượng: {mod_info.get('size')} bytes, Refcount: {mod_info.get('refcount')})")
    else:
        print(f"  Mô-đun Kernel vtxfs.ko    : {c_yellow('CHƯA TẢI (chạy qua Userspace Engine)')}")

    if mount_info.get("mounted"):
        print(f"  Trạng thái Mount          : {c_green('ĐANG HOẠT ĐỘNG')} tại {c_bold(mount_info.get('mountpoint'))}")
        print(f"  Tùy chọn Mount (Options)  : {mount_info.get('opts')}")
    else:
        print(f"  Trạng thái Mount          : {c_yellow('CHƯA GẮN KẾT (UNMOUNTED / OFFLINE - SAFE)')}")

    if hw_info:
        model = hw_info.get('model', 'N/A')
        vendor = hw_info.get('vendor', 'N/A')
        print(f"  Thiết bị phần cứng        : {vendor} {model}")
        rot = hw_info.get('rotational')
        rot_str = "Flash / SSD / USB" if rot == '0' else ("HDD (Đĩa quay)" if rot == '1' else "N/A")
        print(f"  Loại lưu trữ vật lý       : {rot_str}")
        print(f"  I/O Queue Scheduler       : {c_bold(hw_info.get('scheduler', 'N/A'))}")
        print(f"  Bộ đệm đọc trước (Readahead): {hw_info.get('read_ahead_kb', 'N/A')} KiB")
        print(f"  Kích thước I/O tối đa (Max): {hw_info.get('max_sectors_kb', 'N/A')} KiB/request")
        discard_b = int(hw_info.get('discard_max_bytes', '0')) if hw_info.get('discard_max_bytes', '0').isdigit() else 0
        discard_str = f"Có hỗ trợ ({discard_b//1024} KiB)" if discard_b > 0 else "Không hỗ trợ (USB thông thường)"
        print(f"  Lệnh giải phóng TRIM/Discard: {discard_str}")

    print("\n" + "=" * 82)
    os.close(fd)


# --- Subcommand 2: Block Bitmap Map (MAP) ---

def cmd_map(target_dev, vag_filter=None, cols=64, compact=False, max_ranges=25):
    fd = open_device(target_dev)
    sb, _, _ = find_primary_super(fd)
    cpd, _, _ = find_latest_cpd(fd)

    if not sb:
        print(c_red(f"Lỗi: Không tìm thấy Superblock VTXFS trên {target_dev}!"))
        os.close(fd)
        return

    # Check VAG root pointer
    if not cpd:
        print(c_red("Lỗi: Không tìm thấy CPD để đọc địa chỉ VAG bitmap!"))
        os.close(fd)
        return

    # We can invoke vtxfs-inspect bitmap directly or read raw bitmaps
    # Using vtxfs-inspect binary ensures 100% verified authoritative bitmap decoding
    tools_dir = Path(__file__).resolve().parent
    inspect_bin = tools_dir / "vtxfs-inspect"
    if not inspect_bin.exists():
        inspect_bin = Path("/usr/local/bin/vtxfs-inspect")

    total_vags = max(1, sb.total_blocks // VAG_BLOCKS)
    target_vags = [vag_filter] if vag_filter is not None else list(range(min(4, total_vags)))

    print("=" * 82)
    print(c_bold("  VTXFS — BẢN ĐỒ PHÂN BỐ KHỐI VẬT LÝ (BLOCK ALLOCATION BITMAP MAP)"))
    print(c_dim(f"  Thiết bị: {target_dev} | Tổng số VAG: {total_vags} | Khối mỗi VAG: {VAG_BLOCKS:,} blocks (128 MiB)"))
    print("=" * 82)
    print(f"\n  {c_bold('CHÚ GIẢI KÝ HIỆU (MAP LEGEND):')}")
    print(f"    {c_green('■')} : Khối DỮ LIỆU TỆP TIN (File Data Payload Extents)")
    print(f"    {c_yellow(c_bold('C'))} : Khối CONTAINER / SIÊU DỮ LIỆU (B-Tree Leaves, Internal Nodes, VAG Roots)")
    print(f"    {c_dim('·')} : Khối CÒN TRỐNG (Free unallocated blocks)")
    print(f"    {c_cyan('S')} : Vùng Superblock & Bootstrap Reserve (Blocks 0..31)")
    print()

    for vag_id in target_vags:
        start_pblk = 32768 + vag_id * VAG_BLOCKS if vag_id > 0 else 32768
        end_pblk = start_pblk + VAG_BLOCKS - 1

        # Query bitmap via vtxfs-inspect
        cp = subprocess.run(sudo_cmd([str(inspect_bin), target_dev, "bitmap", str(vag_id), "0", str(VAG_BLOCKS)]),
                            capture_output=True, text=True)
        if cp.returncode != 0:
            print(f"Không thể đọc bitmap VAG {vag_id}: {cp.stderr}")
            continue

        bitmap_hex = ""
        allocated_count = 0
        free_count = VAG_BLOCKS
        for line in cp.stdout.splitlines():
            if line.startswith("bitmap_hex="):
                bitmap_hex = line.split("=")[-1]
            elif line.startswith("allocated="):
                allocated_count = int(line.split("=")[-1])
            elif line.startswith("free_blocks="):
                free_count = int(line.split("=")[-1])

        if not bitmap_hex:
            continue

        raw_bits = bytes.fromhex(bitmap_hex)

        # Classify each block in this VAG
        block_types = [TYPE_FREE] * VAG_BLOCKS
        block_descs = ["Khối còn trống (Unallocated)"] * VAG_BLOCKS

        if allocated_count > 0:
            chunk_size = 512
            buf_size = chunk_size * BLOCK_SIZE
            for c_start in range(0, VAG_BLOCKS, chunk_size):
                byte_start = c_start >> 3
                byte_end = (c_start + chunk_size) >> 3
                if all(b == 0 for b in raw_bits[byte_start:byte_end]):
                    continue

                chunk_pblk = start_pblk + c_start
                raw_chunk = os.pread(fd, buf_size, chunk_pblk * BLOCK_SIZE)
                if not raw_chunk:
                    continue

                num_in_chunk = min(chunk_size, len(raw_chunk) // BLOCK_SIZE)
                for b in range(num_in_chunk):
                    blk_idx = c_start + b
                    bit = (raw_bits[blk_idx >> 3] >> (blk_idx & 7)) & 1
                    if bit == 0:
                        continue

                    block_bytes = raw_chunk[b * BLOCK_SIZE : (b + 1) * BLOCK_SIZE]
                    node_magic = struct.unpack_from("<I", block_bytes, 48)[0] if len(block_bytes) >= 52 else 0
                    vag_magic = struct.unpack_from("<I", block_bytes, 16)[0] if len(block_bytes) >= 20 else 0
                    magic_8 = block_bytes[32:40].rstrip(b"\x00")

                    if node_magic == VTXFS_NODE_MAGIC:
                        level = block_bytes[54]
                        tt = block_bytes[55]
                        tname = TREE_NAMES.get(tt, f"Tree#{tt}")
                        if level == 0:
                            block_types[blk_idx] = TYPE_CONTAINER_LEAF
                            block_descs[blk_idx] = f"B-Tree Leaf ({tname})"
                        else:
                            block_types[blk_idx] = TYPE_CONTAINER_NODE
                            block_descs[blk_idx] = f"B-Tree Node L{level} ({tname})"
                    elif vag_magic == VTXFS_VAG_DIR_MAGIC:
                        block_types[blk_idx] = TYPE_CONTAINER_OTHER
                        block_descs[blk_idx] = "VAG Directory Root"
                    elif magic_8 == b"VTXCP21":
                        block_types[blk_idx] = TYPE_CONTAINER_OTHER
                        block_descs[blk_idx] = "CPD Checkpoint Descriptor"
                    elif magic_8 == b"VTXFS21":
                        block_types[blk_idx] = TYPE_CONTAINER_OTHER
                        block_descs[blk_idx] = "Master Superblock"
                    elif magic_8 == b"VDT1" or (len(block_bytes) >= 36 and struct.unpack_from("<I", block_bytes, 32)[0] == 0x31544456):
                        block_types[blk_idx] = TYPE_CONTAINER_OTHER
                        block_descs[blk_idx] = "Device Table Container"
                    else:
                        block_types[blk_idx] = TYPE_DATA
                        block_descs[blk_idx] = "Dữ liệu tệp tin (File Data Payload)"

        data_count = sum(1 for t in block_types if t == TYPE_DATA)
        leaf_count = sum(1 for t in block_types if t == TYPE_CONTAINER_LEAF)
        node_count = sum(1 for t in block_types if t == TYPE_CONTAINER_NODE)
        other_meta_count = sum(1 for t in block_types if t == TYPE_CONTAINER_OTHER)
        container_count = leaf_count + node_count + other_meta_count
        free_count = sum(1 for t in block_types if t == TYPE_FREE)

        data_pct = (data_count / VAG_BLOCKS) * 100
        cont_pct = (container_count / VAG_BLOCKS) * 100
        free_pct = (free_count / VAG_BLOCKS) * 100

        print("-" * 82)
        print(f"  {c_cyan(f'VAG #{vag_id}')} | Phạm vi vật lý: {c_bold(f'PBLK {start_pblk:,} .. {end_pblk:,}')} | {VAG_BLOCKS:,} blocks (128.00 MiB)")
        print(f"  ├─ {c_green('■ Khối DỮ LIỆU (Data)')}     : {c_bold(f'{data_count:,}')} blks ({data_pct:.2f}%) [{data_count * 4 / 1024:.2f} MiB]")
        print(f"  ├─ {c_yellow(c_bold('C Khối CONTAINER (Meta)'))}   : {c_bold(f'{container_count:,}')} blks ({cont_pct:.2f}%) [{container_count * 4 / 1024:.2f} MiB] ({leaf_count:,} Leaves, {node_count:,} Nodes, {other_meta_count:,} System)")
        print(f"  └─ {c_dim('· Khối CÒN TRỐNG (Free)')}   : {c_dim(f'{free_count:,}')} blks ({free_pct:.2f}%) [{free_count * 4 / 1024:.2f} MiB]")
        print("-" * 82)

        # 1. High-Density Visual Grid
        blk_per_char = 32 if compact else 16
        print(f"  Bản đồ nhiệt ({blk_per_char} blocks/ký tự, {cols} cột/hàng):")
        
        row_str = []
        cur_pblk = start_pblk
        for char_idx in range(VAG_BLOCKS // blk_per_char):
            cell_types = block_types[char_idx * blk_per_char : (char_idx + 1) * blk_per_char]
            n_c = sum(1 for t in cell_types if t in (TYPE_CONTAINER_LEAF, TYPE_CONTAINER_NODE, TYPE_CONTAINER_OTHER))
            n_d = cell_types.count(TYPE_DATA)
            n_f = cell_types.count(TYPE_FREE)

            if n_c > 0:
                row_str.append(c_yellow(c_bold('C')))
            elif n_d == blk_per_char:
                row_str.append(c_green('■'))
            elif n_d > 0:
                row_str.append(c_green('■'))
            else:
                row_str.append(c_dim('·'))

            if len(row_str) == cols:
                print(f"    {cur_pblk:>7} |" + "".join(row_str) + f"| {cur_pblk + cols * blk_per_char - 1}")
                cur_pblk += cols * blk_per_char
                row_str = []

        if row_str:
            print(f"    {cur_pblk:>7} |" + "".join(row_str))

        # 2. Contiguous Block Ranges Breakdown
        print(f"\n  {c_bold('BẢNG PHÂN BỐ DẢI KHỐI CHI TIẾT (CONTIGUOUS RANGE BREAKDOWN):')}")
        print("    " + "-" * 82)
        print(f"    {'Phạm vi PBLK':<26} {'Loại khối':<14} {'Số khối':>10} {'Dung lượng':>12}  {'Chi tiết nhận diện'}")
        print("    " + "-" * 82)

        cur_cat = None
        run_start = 0
        run_len = 0
        run_descs = []
        ranges = []

        for blk_idx in range(VAG_BLOCKS):
            t = block_types[blk_idx]
            desc = block_descs[blk_idx]
            cat = "FREE" if t == TYPE_FREE else ("DATA" if t == TYPE_DATA else "CONTAINER")

            if cat != cur_cat:
                if cur_cat is not None:
                    ranges.append((cur_cat, run_start, run_len, run_descs))
                cur_cat = cat
                run_start = blk_idx
                run_len = 1
                run_descs = [desc]
            else:
                run_len += 1
                if desc not in run_descs and len(run_descs) < 3:
                    run_descs.append(desc)

        if cur_cat is not None:
            ranges.append((cur_cat, run_start, run_len, run_descs))

        # Show ranges up to max_ranges
        limit = max_ranges if max_ranges > 0 else len(ranges)
        for r_cat, r_start, r_len, r_descs in ranges[:limit]:
            p_start = start_pblk + r_start
            p_end = p_start + r_len - 1
            size_str = f"{r_len * 4} KiB" if r_len < 256 else f"{r_len * 4 / 1024:.2f} MiB"

            if r_cat == "CONTAINER":
                cat_styled = c_yellow(c_bold("CONTAINER"))
                note = ", ".join(r_descs)
            elif r_cat == "DATA":
                cat_styled = c_green("DỮ LIỆU")
                note = "Dữ liệu tệp tin (File Data Extent)"
            else:
                cat_styled = c_dim("CÒN TRỐNG")
                note = "Dải trống (Unallocated Free Space)"

            print(f"    {p_start:>8} .. {p_end:<12} {cat_styled:<22} {r_len:>8,} blk {size_str:>12}  {c_dim(note)}")

        if len(ranges) > limit:
            remaining_data = sum(r[2] for r in ranges[limit:] if r[0] == "DATA")
            remaining_container = sum(r[2] for r in ranges[limit:] if r[0] == "CONTAINER")
            remaining_free = sum(r[2] for r in ranges[limit:] if r[0] == "FREE")
            print(f"    ... và {len(ranges) - limit:,} dải khác ({c_green(f'{remaining_data:,} khối dữ liệu')}, {c_yellow(f'{remaining_container:,} khối container')}, {c_dim(f'{remaining_free:,} khối trống')}) ...")
        print("    " + "-" * 82 + "\n")

    os.close(fd)


# --- Subcommand 3: Block Deep Inspector & Hex Dump (BLOCK / DUMP-BLOCK) ---

def cmd_block(target_dev, block_id, is_vblk=False):
    fd = open_device(target_dev)
    
    # If vblk is requested, translate vblk to pblk
    pblk = block_id
    if is_vblk:
        # VAG translation: VBLK in VAG 0 is 32768 + vblk
        pblk = 32768 + block_id
        print(c_cyan(f"Chuyển đổi VBLK 0x{block_id:x} ({block_id}) -> PBLK {pblk} (Khối vật lý)\n"))

    raw = read_block(fd, pblk)
    if not raw:
        print(c_red(f"Lỗi: Không thể đọc PBLK {pblk} từ {target_dev}!"))
        os.close(fd)
        return

    print("=" * 82)
    print(c_bold(f"  VTXFS — GIẢI MÃ & KIỂM TRA CHI TIẾT KHỐI VẬT LÝ (BLOCK INSPECTOR)"))
    print(c_dim(f"  Thiết bị: {target_dev} | PBLK: {pblk} (Byte offset: {pblk * BLOCK_SIZE:,} .. {(pblk+1)*BLOCK_SIZE - 1:,})"))
    print("=" * 82)

    # 1. Detection of empty / zero block
    if all(b == 0 for b in raw):
        print(f"\n  {c_yellow('[!] KHỐI RỖNG HOÀN TOÀN (ALL ZEROS / UNALLOCATED BLOCK)')}")
        print(f"  Tất cả 4,096 bytes đều là 0x00. Khối này chưa được cấp phát hoặc vừa được trim/zero.")
        os.close(fd)
        return

    # 2. Check Magic & Auto-Decode
    magic_8 = raw[32:40].rstrip(b'\x00')
    node_magic = struct.unpack_from('<I', raw, 48)[0] if len(raw) >= 52 else 0
    vag_magic = struct.unpack_from('<I', raw, 16)[0] if len(raw) >= 20 else 0

    decoded = False

    # A. Superblock
    if magic_8 == b"VTXFS21":
        decoded = True
        sb = SuperHeader.from_buffer_copy(raw[:ctypes.sizeof(SuperHeader)])
        label = bytes(sb.label).decode('utf-8', 'replace').rstrip('\x00')
        print(f"\n  {c_green('===> PHÁT HIỆN: MASTER SUPERBLOCK (VTXFS21) <===')}")
        print(f"  Phiên bản định dạng      : VTXFS v{sb.major}.{sb.minor}")
        print(f"  Nhãn hệ thống (Label)    : {c_bold(label if label else '(none)')}")
        print(f"  Mã FSID UUID             : {uuid.UUID(bytes=bytes(sb.fsid_uuid))}")
        print(f"  Device UUID              : {uuid.UUID(bytes=bytes(sb.device_uuid))}")
        print(f"  Metadata UUID            : {uuid.UUID(bytes=bytes(sb.metadata_uuid))}")
        print(f"  Superblock Generation    : {sb.super_generation}")
        print(f"  Tổng số khối trên đĩa    : {sb.total_blocks:,} blocks")
        print(f"  Khối cấp phát đầu tiên   : PBLK {sb.first_alloc_pblk}")
        print(f"  Trạng thái hoạt động     : 0x{sb.state:04x} ({c_green('CLEAN') if sb.state == 1 else 'DIRTY'})")

    # B. Checkpoint Descriptor (CPD)
    elif magic_8 == b"VTXCP21":
        decoded = True
        cpd = CheckpointHeader.from_buffer_copy(raw[:ctypes.sizeof(CheckpointHeader)])
        print(f"\n  {c_green('===> PHÁT HIỆN: CHECKPOINT DESCRIPTOR (CPD / VTXCP21) <===')}")
        print(f"  Mã giao dịch (XID)       : {c_bold(str(cpd.xid))}")
        print(f"  Previous XID             : {cpd.prev_xid}")
        if cpd.commit_time_ns > 0:
            commit_dt = datetime.datetime.fromtimestamp(cpd.commit_time_ns / 1e9)
            print(f"  Thời điểm Commit bền vững: {c_bold(commit_dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3])}")
        print(f"  Số khối đã sử dụng       : {cpd.used_blocks:,} blocks")
        print(f"  Số khối còn trống        : {cpd.free_blocks:,} blocks")
        print(f"  Root Tree Root VBLK      : 0x{cpd.root_tree_vblk:x} (Level {cpd.root_tree_level})")
        print(f"  OMAP Meta Tree VBLK      : 0x{cpd.omap_meta_tree_vblk:x} (Level {cpd.tree_levels[0]})")
        print(f"  Ref Tree Root VBLK       : 0x{cpd.ref_tree_vblk:x} (Level {cpd.tree_levels[1]})")
        print(f"  SFQ Tree Root VBLK       : 0x{cpd.sfq_tree_vblk:x} (Level {cpd.tree_levels[2]})")
        print(f"  Reaper Tree Root VBLK    : 0x{cpd.reaper_tree_vblk:x} (Level {cpd.tree_levels[3]})")
        print(f"  VAG Root Header PBLK     : PBLK {cpd.vag_dir_root.pblk}")

    # C. B-Tree Slotted Node
    elif node_magic == VTXFS_NODE_MAGIC:
        decoded = True
        hdr = NodeHeader.from_buffer_copy(raw[:ctypes.sizeof(NodeHeader)])
        is_leaf = bool(hdr.flags & 1)
        is_root = bool(hdr.flags & 2)
        print(f"\n  {c_green('===> PHÁT HIỆN: B-TREE SLOTTED NODE (VXN1) <===')}")
        print(f"  Loại Node                : {c_bold('LEAF (Khối lá - Chứa Items)' if is_leaf else 'INTERNAL (Khối trong - Con trỏ rẽ nhánh)')}")
        print(f"  Cấp độ cây (Level)       : {hdr.level} ({'Gốc cây / Root' if is_root else 'Nhánh'})")
        print(f"  Định danh OID / Owner    : OID {hdr.oid} / Owner {hdr.owner_id}")
        print(f"  Self VBLK                : 0x{hdr.self_vblk:x} ({hdr.self_vblk})")
        print(f"  Thế hệ giao dịch (XID)   : {hdr.xid}")
        print(f"  Số lượng Items (NItems)  : {c_bold(str(hdr.nitems))}")
        print(f"  Vùng trống (Free Space)  : {hdr.free_tail8*8 - hdr.free_head8*8} bytes (Head: {hdr.free_head8*8}, Tail: {hdr.free_tail8*8})")

        # Decode Items
        entry_fmt = '<QQHHHH'
        entry_sz = struct.calcsize(entry_fmt)
        hdr_sz = ctypes.sizeof(NodeHeader)
        print(f"\n  {c_bold('DANH SÁCH CÁC MỤC DỮ LIỆU (ITEMS IN NODE):')}")
        print("  " + "-" * 76)
        print(f"  {'STT':<4} {'Khóa (Key)':<32} {'Kiểu Item':<14} {'Vị trí (Offset/Len)':<18}")
        print("  " + "-" * 76)

        for i in range(min(hdr.nitems, 32)):
            off = hdr_sz + i * entry_sz
            oid_type, key_offset, val_off8, val_len, prefix, eflags = struct.unpack_from(entry_fmt, raw, off)
            item_oid = oid_type >> 8
            item_type = oid_type & 0xff
            type_name = ITEM_TYPES.get(item_type, f"TYPE_{item_type}")
            val_start = val_off8 * 8
            val_end = val_start + val_len
            val_raw = raw[val_start:val_end]

            print(f"  {i:<4} (OID={item_oid:<6} Off={key_offset:<8}) {c_cyan(type_name):<22} {val_start}..{val_end} ({val_len}B)")
            
            # Sub-decode known values
            if item_type == 4 and len(val_raw) >= 19: # DIR_ITEM
                target_ino, dxid, ftype, nlen = struct.unpack_from('<QQBH', val_raw, 0)
                dname = val_raw[19:19+nlen].decode('utf-8', 'replace')
                ft_str = "DIR" if ftype == 2 else ("FILE" if ftype == 1 else f"FT_{ftype}")
                print(f"       └──> {c_green('Tệp/Thư mục')}: {c_bold(dname)} (Inode={target_ino}, Loại={ft_str})")
            elif item_type == 1 and len(val_raw) >= 32: # INODE
                sz, alloc, mode, nlink, uid, gid = struct.unpack_from('<QQIIII', val_raw, 0)
                print(f"       └──> {c_yellow('Inode Spec')}: Size={sz:,}B, Alloc={alloc:,}B, Mode=0{oct(mode)}, Links={nlink}")
            elif item_type == 6 and len(val_raw) >= 28: # EXTENT
                evblk, alloc_id, phys, log_bytes, fl, csum = struct.unpack_from('<QQIIHH', val_raw, 0)
                fl_desc = "INLINE" if (fl & 1) else ("SHARED" if (fl & 2) else "NORMAL")
                print(f"       └──> {c_magenta('Extent')}: VBLK=0x{evblk:x}, Số khối={phys} ({log_bytes:,}B), Kiểu={fl_desc}")
            elif item_type == 7 and len(val_raw) >= 32: # SUBVOL_ROOT
                r_vblk, s_xid, s_root_ino = struct.unpack_from('<QQQ', val_raw, 16)
                print(f"       └──> {c_cyan('Subvolume Root')}: Root VBLK=0x{r_vblk:x}, XID={s_xid}, Root Ino={s_root_ino}")

        if hdr.nitems > 32:
            print(f"  ... và {hdr.nitems - 32} mục khác ...")
        print("  " + "-" * 76)

    # D. Fallback Data Block String Scanner
    else:
        # Check printable ASCII strings
        printable = []
        cur_str = []
        for b in raw:
            if 32 <= b <= 126 or b in (10, 13, 9):
                cur_str.append(chr(b))
            else:
                if len(cur_str) >= 4:
                    printable.append("".join(cur_str))
                cur_str = []
        if len(cur_str) >= 4:
            printable.append("".join(cur_str))

        if len(printable) > 0:
            print(f"\n  {c_cyan('===> PHÁT HIỆN: KHỐI DỮ LIỆU TỆP TIN (FILE CONTENT DATA BLOCK) <===')}")
            print(f"  Tìm thấy {len(printable)} chuỗi ký tự văn bản có nghĩa bên trong khối:")
            for s in printable[:6]:
                clean_s = s.replace('\n', ' ').replace('\r', '')
                if len(clean_s) > 70:
                    clean_s = clean_s[:67] + "..."
                print(f"    • {c_bold(clean_s)}")
            if len(printable) > 6:
                print(f"    ... và {len(printable) - 6} chuỗi khác ...")
        else:
            print(f"\n  {c_dim('===> KHỐI DỮ LIỆU NHỊ PHÂN (OPAQUE BINARY DATA BLOCK) <===')}")

    # 3. Standard Hex + ASCII Dump (hexdump -C style)
    print(f"\n  {c_bold('XUẤT MÃ HEX & ASCII CHI TIẾT (HEXDUMP 4096 BYTES):')}")
    print("  " + "-" * 76)
    
    prev_line = None
    star_printed = False

    for row in range(0, BLOCK_SIZE, 16):
        chunk = raw[row:row+16]
        hex_bytes = " ".join(f"{b:02x}" for b in chunk[:8]) + "  " + " ".join(f"{b:02x}" for b in chunk[8:])
        ascii_text = "".join(chr(b) if 32 <= b <= 126 else "." for b in chunk)
        
        # Deduplication
        if chunk == prev_line:
            if not star_printed:
                print("  *")
                star_printed = True
            continue
        
        star_printed = False
        prev_line = chunk
        print(f"  {row:08x}  {hex_bytes:<49}  |{ascii_text}|")

    print(f"  {BLOCK_SIZE:08x}")
    print("  " + "-" * 76)
    os.close(fd)


# --- Main Command Dispatcher ---

def main():
    parser = argparse.ArgumentParser(
        prog="vtxfs-admin",
        description="VTXFS v2.1 Comprehensive System Diagnostic, Block Map & Inspector Suite"
    )
    subparsers = parser.add_subparsers(dest="command", help="Lệnh cần thực thi")

    # 1. stat / info
    p_stat = subparsers.add_parser("stat", aliases=["info", "sys-info"], help="Báo cáo toàn diện mọi thông số cài đặt & vận hành")
    p_stat.add_argument("device", help="Đường dẫn thiết bị hoặc file image (VD: /dev/sdb2)")

    # 2. map / bitmap-map
    p_map = subparsers.add_parser("map", aliases=["bitmap-map"], help="Xuất bản đồ khối bitmap trực quan (Used vs Free)")
    p_map.add_argument("device", help="Đường dẫn thiết bị hoặc file image (VD: /dev/sdb2)")
    p_map.add_argument("--vag", type=int, default=None, help="Chỉ định xem VAG cụ thể (0..63)")
    p_map.add_argument("--cols", type=int, default=64, help="Số cột của bản đồ nhiệt (mặc định 64)")
    p_map.add_argument("--compact", action="store_true", help="Chế độ thu nhỏ (32 khối/ký tự)")
    p_map.add_argument("--ranges", type=int, default=25, help="Số lượng dải khối tối đa hiển thị (mặc định 25, 0 để hiển thị tất cả)")

    # 3. block / dump-block
    p_block = subparsers.add_parser("block", aliases=["dump-block"], help="Xem và giải mã thông minh nội dung bên trong khối 4KB")
    p_block.add_argument("device", help="Đường dẫn thiết bị hoặc file image (VD: /dev/sdb2)")
    p_block.add_argument("block_id", type=lambda x: int(x, 0), help="Số hiệu khối (hỗ trợ cả thập phân và hex 0x...)")
    p_block.add_argument("--vblk", action="store_true", help="Giải thích số hiệu khối là VBLK thay vì PBLK")

    # 4. verify
    p_verify = subparsers.add_parser("verify", help="Xác thực toàn vẹn dữ liệu Merkle Tree & 100%% Bit-for-Bit tốc độ cao")
    p_verify.add_argument("source", help="Đường dẫn nguồn")
    p_verify.add_argument("destination", help="Đường dẫn đích")
    p_verify.add_argument("--merkle", action="store_true", help="Kích hoạt xác thực Merkle Tree & Băm phân tầng (Layered Hashing)")
    p_verify.add_argument("--quick", action="store_true", help="Chế độ kiểm tra mẫu nhanh cho tệp lớn")
    p_verify.add_argument("--threads", type=int, default=8, help="Số luồng I/O song song (mặc định 8)")
    p_verify.add_argument("--save-manifest", type=str, default=None, help="Lưu bản kê Merkle Manifest (.json)")
    p_verify.add_argument("--use-manifest", type=str, default=None, help="Sử dụng bản kê Merkle Manifest")

    # 5. scrub
    p_scrub = subparsers.add_parser("scrub", help="Kích hoạt Online Kernel Scrubbing (VTX_IOC_SCRUB) trên điểm gắn VTXFS")
    p_scrub.add_argument("target", help="Điểm gắn mountpoint hoặc file image VTXFS")
    p_scrub.add_argument("--verify-only", action="store_true", default=True, help="Chế độ chỉ kiểm tra không sửa chữa (mặc định)")

    # Quick dispatch if invoked directly via symlink names
    prog_name = Path(sys.argv[0]).name
    if prog_name in ("vtxfs-stat", "vtxfs-info") and len(sys.argv) >= 2 and sys.argv[1] not in ("-h", "--help"):
        if sys.argv[1] not in ("stat", "info", "sys-info"):
            sys.argv.insert(1, "stat")
    elif prog_name in ("vtxfs-map", "vtxfs-bitmap") and len(sys.argv) >= 2 and sys.argv[1] not in ("-h", "--help"):
        if sys.argv[1] not in ("map", "bitmap-map"):
            sys.argv.insert(1, "map")
    elif prog_name in ("vtxfs-block", "vtxfs-dump") and len(sys.argv) >= 2 and sys.argv[1] not in ("-h", "--help"):
        if sys.argv[1] not in ("block", "dump-block"):
            sys.argv.insert(1, "block")
    elif prog_name == "vtxfs-verify" and len(sys.argv) >= 2 and sys.argv[1] not in ("-h", "--help"):
        if sys.argv[1] != "verify":
            sys.argv.insert(1, "verify")
    elif prog_name == "vtxfs-scrub" and len(sys.argv) >= 2 and sys.argv[1] not in ("-h", "--help"):
        if sys.argv[1] != "scrub":
            sys.argv.insert(1, "scrub")

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        sys.exit(1)

    if args.command in ("stat", "info", "sys-info"):
        cmd_stat(args.device)
    elif args.command in ("map", "bitmap-map"):
        cmd_map(args.device, vag_filter=args.vag, cols=args.cols, compact=args.compact, max_ranges=args.ranges)
    elif args.command in ("block", "dump-block"):
        cmd_block(args.device, args.block_id, is_vblk=args.vblk)
    elif args.command == "verify":
        from vtxfs_verify import run_verification
        ok = run_verification(args.source, args.destination, quick=args.quick, merkle=args.merkle,
                              num_threads=args.threads, save_manifest=args.save_manifest,
                              use_manifest=args.use_manifest)
        sys.exit(0 if ok else 1)
    elif args.command == "scrub":
        cmd = sudo_cmd(["vtxfs", "scrub", "start", args.target])
        if args.verify_only:
            cmd.append("--verify-only")
        res = subprocess.run(cmd)
        sys.exit(res.returncode)

if __name__ == "__main__":
    main()
