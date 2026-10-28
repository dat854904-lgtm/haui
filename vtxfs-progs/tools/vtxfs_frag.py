#!/usr/bin/env python3
"""
VTXFS v1.0 — High-Precision Filesystem & Free-Space Fragmentation Analyzer
==========================================================================
Accurately measures:
  1. Per-file extent fragmentation & physical layout (Online via FIEMAP).
  2. Directory-wide & subvolume fragmentation metrics.
  3. Free Space Fragmentation Index (FSFI) across VAG allocation groups.
  4. Extent size distribution histogram (1-block, 2-7 blks, 8-31 blks, 32+ blks).
"""

import os
import sys
import struct
import fcntl
import argparse
import subprocess
from pathlib import Path

# FIEMAP ioctl constants
FS_IOC_FIEMAP = 0xC020660B
FIEMAP_FLAG_SYNC = 0x00000001
FIEMAP_EXTENT_LAST = 0x00000001

ROOT_DIR = Path(__file__).resolve().parent.parent.parent
TOOLS_DIR = ROOT_DIR / "vtxfs-progs/tools"
INSPECT_BIN = TOOLS_DIR / "vtxfs-inspect"
VTXFS_CLI = TOOLS_DIR / "vtxfs/vtxfs"


def get_file_extents_fiemap(filepath):
    """Query FIEMAP ioctl directly on a mounted file to get exact physical extents."""
    try:
        fd = os.open(filepath, os.O_RDONLY)
    except Exception as e:
        return None, str(e)

    try:
        st = os.fstat(fd)
        filesize = st.st_size
        if filesize == 0:
            return [], None

        # fiemap header struct: start(u64), len(u64), flags(u32), mapped(u32), count(u32), pad(u32)
        # struct fiemap_extent: logical(u64), phys(u64), len(u64), res1(u64), res2(u64), flags(u32), pad(u32)
        max_extents = 512
        fiemap_hdr_fmt = "=QQIIII"
        fiemap_hdr_size = struct.calcsize(fiemap_hdr_fmt)
        extent_fmt = "=QQQQQIIII"
        extent_size = struct.calcsize(extent_fmt)

        buf = bytearray(fiemap_hdr_size + max_extents * extent_size)
        struct.pack_into(fiemap_hdr_fmt, buf, 0, 0, filesize, FIEMAP_FLAG_SYNC, 0, max_extents, 0)

        fcntl.ioctl(fd, FS_IOC_FIEMAP, buf)

        _, _, _, mapped_extents, _, _ = struct.unpack_from(fiemap_hdr_fmt, buf, 0)

        extents = []
        for i in range(mapped_extents):
            offset = fiemap_hdr_size + i * extent_size
            logical, physical, length, _, _, flags, _, _, _ = struct.unpack_from(extent_fmt, buf, offset)
            extents.append({
                "index": i,
                "logical": logical,
                "physical": physical,
                "length": length,
                "blocks": length // 4096,
                "flags": flags
            })
        return extents, None
    finally:
        os.close(fd)


def analyze_mounted_path(target_path):
    p = Path(target_path)
    if p.is_file():
        files = [p]
    elif p.is_dir():
        files = [f for f in p.rglob("*") if f.is_file()]
    else:
        print(f"Lỗi: Đường dẫn '{target_path}' không tồn tại!", file=sys.stderr)
        return

    print("=" * 76)
    print("  VTXFS — PHÂN TÍCH ĐỘ PHÂN MẢNH TRỰC TUYẾN (ONLINE FIEMAP)")
    print(f"  Mục tiêu: {target_path} (Tổng số tệp: {len(files)})")
    print("=" * 76)
    print()

    total_files = 0
    total_extents = 0
    total_blocks = 0
    fragmented_files = 0

    results = []

    for f in files:
        extents, err = get_file_extents_fiemap(f)
        if err or extents is None:
            continue
        total_files += 1
        n_ext = len(extents)
        if n_ext == 0:
            continue
        
        file_bytes = f.stat().st_size
        file_blks = (file_bytes + 4095) // 4096
        total_blocks += file_blks
        total_extents += n_ext

        # Theoretical ideal extents for this file (contiguous)
        ideal_extents = 1
        # Discontinuities
        discontinuities = 0
        for i in range(1, len(extents)):
            expected_pblk = extents[i-1]["physical"] + extents[i-1]["length"]
            if extents[i]["physical"] != expected_pblk:
                discontinuities += 1

        is_frag = discontinuities > 0
        if is_frag:
            fragmented_files += 1

        frag_pct = (discontinuities / file_blks) * 100 if file_blks > 1 else 0.0

        results.append({
            "path": f,
            "size": file_bytes,
            "blocks": file_blks,
            "extents": n_ext,
            "discontinuities": discontinuities,
            "frag_pct": frag_pct,
            "details": extents
        })

    if not results:
        print("Không tìm thấy tệp dữ liệu có extent hợp lệ.")
        return

    # Sort by fragmentation
    results.sort(key=lambda x: x["discontinuities"], reverse=True)

    # Detailed report for single file
    if len(results) == 1:
        res = results[0]
        print(f"  Tệp tin           : {res['path']}")
        print(f"  Dung lượng        : {res['size']:,} bytes ({res['blocks']} khối 4K)")
        print(f"  Số đoạn extents   : {res['extents']}")
        print(f"  Điểm đứt gãy      : {res['discontinuities']}")
        print(f"  Tỷ lệ phân mảnh   : {res['frag_pct']:.2f}%")
        print()
        print("  BẢNG ÁNH XẠ EXTENT VẬT LÝ CHI TIẾT:")
        print("  " + "-" * 72)
        print("  Ext  Logical Offset        Physical Offset       Số khối   Trạng thái")
        print("  " + "-" * 72)
        for e in res["details"]:
            log_end = e["logical"] + e["length"] - 1
            phys_end = e["physical"] + e["length"] - 1
            pblk_start = e["physical"] // 4096
            pblk_end = phys_end // 4096
            flags_str = "EOF" if (e["flags"] & FIEMAP_EXTENT_LAST) else ""
            print(f"  {e['index']:<4} {e['logical']//4096:>5}..{log_end//4096:<6} "
                  f"-> pblk {pblk_start:>7}..{pblk_end:<7} ({e['blocks']:>3} blks)  {flags_str}")
        print("  " + "-" * 72)
    else:
        # Summary for directory
        overall_frag_rate = (fragmented_files / total_files) * 100 if total_files > 0 else 0
        avg_extents_per_file = total_extents / total_files if total_files > 0 else 0

        print(f"  Tổng số tệp quét được     : {total_files}")
        print(f"  Tổng số khối dữ liệu      : {total_blocks} khối ({total_blocks*4/1024:.2f} MiB)")
        print(f"  Tổng số extents           : {total_extents}")
        print(f"  Số tệp bị phân mảnh       : {fragmented_files} / {total_files} ({overall_frag_rate:.1f}%)")
        print(f"  Trung bình extents/tệp    : {avg_extents_per_file:.2f}")
        print()
        print("  TOP CÁC TỆP PHÂN MẢNH NHIỀU NHẤT:")
        print("  " + "-" * 72)
        print(f"  {'Tên tệp':<36} {'Dung lượng':>10} {'Extents':>8} {'Đứt gãy':>8} {'Phân mảnh':>10}")
        print("  " + "-" * 72)
        for r in results[:10]:
            name = r["path"].name
            if len(name) > 34:
                name = name[:31] + "..."
            print(f"  {name:<36} {r['size']/(1024*1024):>8.2f}MB {r['extents']:>8} {r['discontinuities']:>8} {r['frag_pct']:>9.1f}%")
        print("  " + "-" * 72)


def analyze_offline_device(device_path):
    print("=" * 76)
    print("  VTXFS — PHÂN TÍCH ĐỘ PHÂN MẢNH NGOẠI TUYẾN (B-TREE & VAG BITMAP)")
    print(f"  Thiết bị: {device_path}")
    print("=" * 76)
    print()

    # 1. Extent Fragmentation via vtxfs defrag scan
    print("--- 1. ĐỘ PHÂN MẢNH EXTENT DỮ LIỆU (B-TREE EXTENTS) ---")
    try:
        cp = subprocess.run(["sudo", str(VTXFS_CLI), "defrag", "scan", device_path],
                            capture_output=True, text=True, check=True)
        print(cp.stdout.strip())
    except Exception as e:
        print(f"Không thể đọc B-tree: {e}")

    # 2. Free Space Fragmentation (FSFI) via vtxfs-inspect bitmap
    print("\n--- 2. ĐỘ PHÂN MẢNH KHÔNG GIAN TRỐNG (FREE SPACE FRAGMENTATION - FSFI) ---")
    try:
        # Check first 8 VAGs
        hist = {"1": 0, "2-7": 0, "8-31": 0, "32-127": 0, "128+": 0}
        total_free_blocks = 0
        total_free_runs = 0
        max_free_run = 0

        for vag_id in range(8):
            cp = subprocess.run(["sudo", str(INSPECT_BIN), device_path, "bitmap", str(vag_id), "0", "32768"],
                                capture_output=True, text=True)
            if cp.returncode != 0:
                continue

            lines = cp.stdout.splitlines()
            bitmap_hex = ""
            for l in lines:
                if l.startswith("bitmap_hex="):
                    bitmap_hex = l.split("=")[-1]
                    break

            if not bitmap_hex:
                continue

            raw_bytes = bytes.fromhex(bitmap_hex)
            cur_free_run = 0

            for byte_val in raw_bytes:
                for bit in range(8):
                    is_allocated = (byte_val >> bit) & 1
                    if is_allocated == 0:
                        cur_free_run += 1
                        total_free_blocks += 1
                    else:
                        if cur_free_run > 0:
                            total_free_runs += 1
                            if cur_free_run > max_free_run:
                                max_free_run = cur_free_run
                            if cur_free_run == 1:
                                hist["1"] += 1
                            elif cur_free_run <= 7:
                                hist["2-7"] += 1
                            elif cur_free_run <= 31:
                                hist["8-31"] += 1
                            elif cur_free_run <= 127:
                                hist["32-127"] += 1
                            else:
                                hist["128+"] += 1
                            cur_free_run = 0

            if cur_free_run > 0:
                total_free_runs += 1
                if cur_free_run > max_free_run:
                    max_free_run = cur_free_run
                if cur_free_run == 1:
                    hist["1"] += 1
                elif cur_free_run <= 7:
                    hist["2-7"] += 1
                elif cur_free_run <= 31:
                    hist["8-31"] += 1
                elif cur_free_run <= 127:
                    hist["32-127"] += 1
                else:
                    hist["128+"] += 1

        fsfi = 1.0 - (max_free_run / total_free_blocks) if total_free_blocks > 0 else 0.0
        avg_free_run = total_free_blocks / total_free_runs if total_free_runs > 0 else 0

        print(f"  Tổng khối còn trống       : {total_free_blocks:,} khối ({total_free_blocks*4/1024:.2f} MiB)")
        print(f"  Dải trống lớn nhất        : {max_free_run:,} khối liên tục ({max_free_run*4/1024:.2f} MiB)")
        print(f"  Số lượng dải trống (runs) : {total_free_runs:,}")
        print(f"  Độ dài dải trống trung bình: {avg_free_run:.1f} khối")
        print(f"  Chỉ số phân mảnh FSFI     : {fsfi*100:.2f}% (0%=Hoàn toàn liền mạch, 100%=Vụn nát)")
        print()
        print("  BIỂU ĐỒ PHÂN BỐ CÁC DẢI TRỐNG (FREE RUN HISTOGRAM):")
        print(f"    1 block   (4 KiB)      : {hist['1']:>6} dải {'█' * min(40, hist['1'] // max(1, total_free_runs // 40))}")
        print(f"    2-7 blks  (8 - 28 KiB) : {hist['2-7']:>6} dải {'█' * min(40, hist['2-7'] // max(1, total_free_runs // 40))}")
        print(f"    8-31 blks (32 - 124 KiB): {hist['8-31']:>6} dải {'█' * min(40, hist['8-31'] // max(1, total_free_runs // 40))}")
        print(f"    32-127 blks(128 - 508 K): {hist['32-127']:>6} dải {'█' * min(40, hist['32-127'] // max(1, total_free_runs // 40))}")
        print(f"    128+ blks (>= 512 KiB) : {hist['128+']:>6} dải {'█' * min(40, hist['128+'] // max(1, total_free_runs // 40))}")

    except Exception as e:
        print(f"Lỗi đọc bitmap: {e}")


def main():
    parser = argparse.ArgumentParser(description="VTXFS Precise Fragmentation Analyzer")
    parser.add_argument("target", help="File, mounted directory, or block device (e.g. /mnt/vtxfs or /dev/sdb2)")
    args = parser.parse_args()

    target = args.target
    if os.path.exists(target) and (os.path.isfile(target) or os.path.isdir(target)) and not target.startswith("/dev/"):
        analyze_mounted_path(target)
    else:
        analyze_offline_device(target)


if __name__ == "__main__":
    main()
