#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail-closed VTXFS verified benchmark. Never runs against a device implicitly.

A score is VALID only after: exact byte count, end_fsync, device writes,
live VTX_IOC_SCRUB check, cold remount, CRC/byte verification, SHA256 equality,
clean offline fsck and zero VTXFS kernel errors.
Supports both dedicated host mounts (--mount / --device) and isolated QEMU guest
VM execution (--vm) on disposable /dev/shm storage.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KMOD = ROOT.parent / "fs/vtxfs"
KERNEL = Path(os.environ.get("VTXFS_VM_KERNEL", "/boot/vmlinuz-linux-zen"))
GIB = 1024 ** 3
VM_BENCH_BYTES = 64 * 1024 * 1024
VTX_ERROR = re.compile(r"(?:vtxfs.*(?:error|fail|corrupt|EIO|ENOSPC|EUCLEAN)|"
                       r"(?:BUG:|Kernel panic|I/O error).*vtxfs)", re.I)


def run(argv, *, capture=True):
    cp = subprocess.run([str(x) for x in argv], text=True,
                        stdout=subprocess.PIPE if capture else None,
                        stderr=subprocess.PIPE if capture else None, check=False)
    if cp.returncode != 0:
        raise RuntimeError(f"{argv[0]} exit={cp.returncode}: {(cp.stderr or cp.stdout or '')[-2500:]}")
    return cp.stdout or ""


def expected_rc_stream_sha256(total_bytes):
    h = hashlib.sha256()
    buf = bytearray(65536)
    mask = 0xFFFFFFFFFFFFFFFF
    for base in range(0, total_bytes, 65536):
        chunk_len = min(65536, total_bytes - base)
        for i in range(chunk_len):
            off = (base + i) & mask
            x = (off * 0x9E3779B97F4A7C15 + 0xBF58476D1CE4E5B9) & mask
            x ^= x >> 30
            x = (x * 0xBF58476D1CE4E5B9) & mask
            x ^= x >> 27
            buf[i] = x & 0xFF
        h.update(memoryview(buf)[:chunk_len])
    return h.hexdigest()


def build_bench_initramfs(tmp):
    fs = tmp / "root"
    for name in ("dev", "proc", "sys", "mnt", "deps"):
        (fs / name).mkdir(parents=True, exist_ok=True)
    run(["gcc", "-static", "-pthread", "-O2", "-Wall", "-Wextra", "-Werror",
         ROOT / "tests/vm_guest_init.c", "-o", fs / "init"])
    shutil.copy2(KMOD / "vtxfs.ko", fs / "vtxfs.ko")
    testmod = KMOD / "vtxfs_free_cache_test.ko"
    if testmod.is_file():
        shutil.copy2(testmod, fs / testmod.name)

    order = []
    deps = run(["modinfo", "-F", "depends", KMOD / "vtxfs.ko"]).strip()
    for dep in [d for d in deps.split(",") if d]:
        lines = run(["modprobe", "--show-depends", dep]).splitlines()
        for line in lines:
            words = line.split()
            if len(words) < 2 or words[0] != "insmod":
                continue
            module = Path(words[1])
            target = fs / "deps" / module.name.removesuffix(".zst")
            if target.exists():
                continue
            if module.suffix == ".zst":
                p = subprocess.run(["zstd", "-dc", str(module)],
                                   capture_output=True, check=True)
                target.write_bytes(p.stdout)
            else:
                shutil.copy2(module, target)
            order.append("/deps/" + target.name)
    (fs / "deps/order").write_text("".join(x + "\n" for x in order))

    archive = tmp / "initramfs.cpio"
    paths = sorted([p.relative_to(fs).as_posix() for p in fs.rglob("*")])
    files = b"\x00".join(os.fsencode("./" + p) for p in paths) + b"\x00"
    with archive.open("wb") as output:
        p = subprocess.run(["cpio", "-0", "-o", "-H", "newc", "--quiet"],
                           cwd=fs, input=files, stdout=output,
                           stderr=subprocess.PIPE, timeout=20)
    if p.returncode != 0:
        raise RuntimeError(f"cpio failed: {p.stderr.decode(errors='replace')}")
    return archive


def run_vm_benchmark(args):
    args.report_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.report_dir / "verified_benchmark.json"
    result = {
        "status": "INVALID",
        "power_cut_tested": False,
        "execution_mode": "qemu_guest_vm",
        "size_required_bytes": VM_BENCH_BYTES,
        "mode": "CORRECTNESS_AND_VM_THROUGHPUT",
        "warning": "Cold remount inside QEMU guest VM on /dev/shm; measures VTXFS kernel VFS + CoW B+Tree + CRC32C path.",
    }
    try:
        with tempfile.TemporaryDirectory(prefix="vtxfs-bench-", dir="/dev/shm") as temp:
            tmp = Path(temp)
            image = tmp / "bench.img"
            run([ROOT / "tools/mkfs.vtxfs", "-b", "65536", image])
            initrd = build_bench_initramfs(tmp)
            qemu_cmd = [
                "qemu-system-x86_64", "-accel", "kvm", "-machine", "q35",
                "-cpu", "host", "-m", "1024", "-smp", "2",
                "-kernel", str(KERNEL), "-initrd", str(initrd),
                "-append", "console=ttyS0 rdinit=/init panic=5 loglevel=5 vtx_mode=bench",
                "-drive", f"file={image},if=virtio,format=raw,cache=none",
                "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
            ]
            with open(os.devnull, "rb") as devnull:
                cp = subprocess.run(qemu_cmd, cwd=ROOT, stdin=devnull,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=300)
            out = cp.stdout
            bad = [line for line in out.splitlines()
                   if (VTX_ERROR.search(line) and "failures=0" not in line) or "VTXFS_VM_FAILURE" in line]
            if bad or "VTXFS_VM_BENCH_OK" not in out or "VTXFS_VM_PASS failures=0" not in out:
                raise RuntimeError(f"VM benchmark failed: {bad or out[-1000:]}")

            m = re.search(
                r"VTXFS_VM_BENCH_OK bytes=(\d+) write_ns=(\d+) read_ns=(\d+) "
                r"disk_written_bytes=(\d+) scrub_data=(\d+) scrub_meta=(\d+)",
                out,
            )
            if not m:
                raise RuntimeError("Missing VTXFS_VM_BENCH_OK metrics line")

            b_done = int(m.group(1))
            write_s = max(int(m.group(2)) / 1e9, 1e-6)
            read_s = max(int(m.group(3)) / 1e9, 1e-6)
            disk_written = int(m.group(4))
            scrub_data = int(m.group(5))
            scrub_meta = int(m.group(6))

            # Verify offline fsck and SHA-256 on host
            run([ROOT / "tools/fsck.vtxfs", "-q", image])
            p_cat = subprocess.run([str(ROOT / "tools/vtxfs-inspect"), str(image), "cat", "/bench.bin"],
                                   capture_output=True, timeout=60)
            if p_cat.returncode != 0 or len(p_cat.stdout) != VM_BENCH_BYTES:
                raise RuntimeError("Host vtxfs-inspect cat /bench.bin verification failed")
            expected_sha = expected_rc_stream_sha256(VM_BENCH_BYTES)
            actual_sha = hashlib.sha256(p_cat.stdout).hexdigest()
            if actual_sha != expected_sha:
                raise RuntimeError(f"SHA256 mismatch: {actual_sha} != {expected_sha}")

            mib = b_done / (1024 * 1024)
            result["write"] = {
                "bytes": b_done,
                "MiB_s": round(mib / write_s, 3),
                "wall_seconds": round(write_s, 3),
            }
            result["read"] = {
                "bytes": b_done,
                "MiB_s": round(mib / read_s, 3),
                "wall_seconds": round(read_s, 3),
            }
            result["live_scrub"] = {
                "data_blocks_scrubbed": scrub_data,
                "meta_blocks_scrubbed": scrub_meta,
                "checksum_errors": 0,
            }
            result["sha256_before"] = expected_sha
            result["sha256_after_remount"] = actual_sha
            result["device_written_bytes"] = disk_written
            result["device_write_ratio_includes_other_io"] = round(disk_written / b_done, 4)
            result["status"] = "CORRECTNESS_PASS_VM_VERIFIED"
            result["reason"] = "64 MiB write+fsync, live VTX_IOC_SCRUB, cold RO remount, read verify, SHA256, offline fsck, and guest dmesg OK"
    except Exception as exc:
        result["reason"] = str(exc)
    finally:
        log_path.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"].startswith("CORRECTNESS_PASS") else 1


def fio_job(name, filename, size, kind, result_path):
    opts = [
        "fio", f"--name={name}", f"--filename={filename}",
        "--ioengine=sync", "--thread=1", "--numjobs=1",
        f"--rw={kind}", "--bs=1m", f"--size={size}",
        "--verify=crc32c", "--verify_fatal=1", "--direct=0",
        "--group_reporting=1", "--output-format=json",
        f"--output={result_path}",
    ]
    if kind == "write":
        opts += ["--do_verify=0", "--end_fsync=1",
                 "--refill_buffers=1", "--randrepeat=0",
                 "--verify_state_save=1"]
    else:
        opts += ["--verify_only=1", "--do_verify=1",
                 "--verify_state_load=1", "--readonly"]
    start = time.monotonic()
    cp = subprocess.run(opts, text=True, capture_output=True)
    duration = time.monotonic() - start
    raw = result_path.read_text() if result_path.exists() else ""
    try:
        js = json.loads(raw)
    except json.JSONDecodeError:
        js = {}
    jobs = js.get("jobs", [])
    errors = [j.get("error", -1) for j in jobs]
    if cp.returncode or not jobs or any(errors):
        raise RuntimeError(f"fio {kind}: exit={cp.returncode}, job_errors={errors}, "
                           f"stderr={cp.stderr[-1000:]}")
    return {"raw": js, "duration_seconds": round(duration, 3)}


def get_stat(dev):
    st = dev.stat()
    if not stat.S_ISBLK(st.st_mode):
        raise RuntimeError(f"not a block device: {dev}")
    p = Path(f"/sys/dev/block/{os.major(st.st_rdev)}:{os.minor(st.st_rdev)}/stat")
    if not p.is_file():
        raise RuntimeError(f"missing block I/O counters for {dev}: {p}")
    nums = [int(v) for v in p.read_text().split()]
    if len(nums) < 7:
        raise RuntimeError("incomplete block I/O counters")
    return {"read_sectors": nums[2], "write_sectors": nums[6]}


def file_hash(f):
    h = hashlib.sha256()
    n = 0
    with f.open("rb", buffering=0) as fd:
        while True:
            piece = fd.read(4 * 1024 * 1024)
            if not piece:
                break
            h.update(piece)
            n += len(piece)
    return h.hexdigest(), n


def kernel_log():
    return run(["dmesg", "--color=never"]).splitlines()


def guard(args, size):
    m = args.mount.resolve()
    d = args.device.resolve(strict=True)
    if not m.is_dir() or not os.path.ismount(m) or m == Path("/"):
        raise RuntimeError("mount must be an existing, dedicated mounted directory")
    if args.report_dir.resolve() == m or m in args.report_dir.resolve().parents:
        raise RuntimeError("report directory must be OUTSIDE the benchmark filesystem")
    if args.confirm_device != str(args.device):
        raise RuntimeError("pass --confirm-device EXACTLY matching --device to authorize remount")
    source = run(["findmnt", "-n", "-o", "SOURCE", "--target", str(m)]).strip()
    fstype = run(["findmnt", "-n", "-o", "FSTYPE", "--target", str(m)]).strip()
    if fstype != "vtxfs" or Path(source).resolve() != d:
        raise RuntimeError(f"unexpected mount: {source}, type={fstype}; require dedicated VTXFS mount")
    opts = run(["findmnt", "-n", "-o", "OPTIONS", "--target", str(m)]).strip()
    if "ro" in opts.split(","):
        raise RuntimeError("mount is read-only")
    if not args.allow_small_validation and size <= 2 * os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"):
        raise RuntimeError("dataset must exceed 2x physical RAM. Override only with --allow-small-validation")
    if shutil.disk_usage(m).free < int(size * 1.2):
        raise RuntimeError("insufficient free filesystem space (+20% minimum); refuse partial write")
    return m, d


def summarize_fio(j, direction):
    x = j["raw"]["jobs"][0]
    op = x.get(direction, {})
    percentiles = op.get("clat_ns", {}).get("percentile", {})
    bw = op.get("bw_bytes", 0)
    bytes_done = op.get("io_bytes", 0)
    cpu_pct = (x.get("usr_cpu", 0) or 0) + (x.get("sys_cpu", 0) or 0)
    cpu_s_per_gib = ((j["duration_seconds"] * cpu_pct / 100) /
                     (bytes_done / GIB)) if bytes_done else None
    return {"bytes": bytes_done,
            "CPU_seconds_per_GiB_fio_process": cpu_s_per_gib,
            "MiB_s": round(bw / (1024 * 1024), 3),
            "IOPS": op.get("iops"),
            "p99_ms": (float(percentiles.get("99.000000", 0)) / 1e6),
            "p99_9_ms": (float(percentiles.get("99.900000", 0)) / 1e6),
            "fio_usr_cpu_pct": x.get("usr_cpu"),
            "fio_sys_cpu_pct": x.get("sys_cpu"),
            "wall_seconds": j["duration_seconds"]}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mount", type=Path, default=Path("/mnt/not-mounted"))
    ap.add_argument("--device", type=Path, default=Path("/dev/loop-not-real"))
    ap.add_argument("--confirm-device", default="/dev/loop-not-real",
                    help="Must match --device exactly, explicitly authorizing unmount/remount")
    ap.add_argument("--report-dir", type=Path, required=True)
    ap.add_argument("--size-gib", type=float, default=16)
    ap.add_argument("--fsck", type=Path, default=Path("tools/fsck.vtxfs"))
    ap.add_argument("--remount-options", default="",
                    help="User-provided VTXFS mount options, e.g. subvol=...")
    ap.add_argument("--allow-small-validation", action="store_true",
                    help="Correctness-only: bypass >2x RAM; NEVER report bandwidth as disk-performance valid")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--vm", action="store_true",
                    help="Execute verified 64 MiB write/fsync/scrub/cold-remount/read/fsck benchmark in isolated QEMU VM")
    args = ap.parse_args()

    if args.vm:
        return run_vm_benchmark(args)

    size = int(args.size_gib * GIB)
    if size < 1024 * 1024 or size % (1024 * 1024):
        ap.error("size must be an integer multiple of 1 MiB (at least 1 MiB)")
    if args.dry_run:
        print(json.dumps({"mode": "DRY_RUN", "size": size,
                          "mount": str(args.mount), "device": str(args.device),
                          "phases": ["fio write+fsync", "SHA256 baseline",
                                     "unmount", "offline fsck", "remount",
                                     "fio CRC read verify", "SHA256 cold read",
                                     "compare I/O counters and dmesg"]}, indent=2))
        return 0

    mount_path = args.mount.resolve()
    report_path = args.report_dir.resolve()
    if report_path == mount_path or mount_path in report_path.parents:
        ap.error("report directory must be outside the filesystem under test")
    args.report_dir.mkdir(parents=True, exist_ok=True)
    result = {"status": "INVALID", "power_cut_tested": False,
              "reason": "", "size_required_bytes": size,
              "mode": "CORRECTNESS_ONLY" if args.allow_small_validation else "DURABILITY_BENCHMARK_PRECHECK",
              "warning": "Cold remount does not simulate loss of volatile device write cache."}
    log_path = args.report_dir / "verified_benchmark.json"
    mounted = True
    try:
        for tool in ("fio", "findmnt", "dmesg", "mount", "umount"):
            if shutil.which(tool) is None:
                raise RuntimeError(f"missing {tool}")
        m, dev = guard(args, size)
        fsck = args.fsck.resolve(strict=True)
        if not os.access(fsck, os.X_OK):
            raise RuntimeError(f"fsck tool is not executable: {fsck}")
        before_log = kernel_log()
        before_dev = get_stat(dev)
        name = f".vtx-verified-benchmark-{os.getpid()}-{int(time.time())}"
        path = m / name
        if path.exists():
            raise RuntimeError("benchmark filename collision")
        data = fio_job("vtx_verified", path, size, "write",
                       args.report_dir / "fio_write.json")
        if summarize_fio(data, "write")["bytes"] != size:
            raise RuntimeError("SHORT WRITE despite successful fio exit")
        pre_hash, pre_bytes = file_hash(path)
        if pre_bytes != size:
            raise RuntimeError(f"SHORT PRE-REMOUNT READ: {pre_bytes} != {size}")
        result["write"] = summarize_fio(data, "write")
        run(["umount", str(m)])
        mounted = False
        run([str(fsck), "-q", str(dev)])
        mount_cmd = ["mount", "-t", "vtxfs"]
        if args.remount_options:
            mount_cmd += ["-o", args.remount_options]
        mount_cmd += [str(dev), str(m)]
        run(mount_cmd)
        mounted = True
        check = fio_job("vtx_verified", path, size, "read",
                        args.report_dir / "fio_verify.json")
        if summarize_fio(check, "read")["bytes"] != size:
            raise RuntimeError("SHORT COLD READ despite successful fio exit")
        post_hash, post_bytes = file_hash(path)
        if post_bytes != size or post_hash != pre_hash:
            raise RuntimeError("DATA MISMATCH after cold remount")
        after_dev = get_stat(dev)
        disk_written = (after_dev["write_sectors"] - before_dev["write_sectors"]) * 512
        if disk_written < size:
            raise RuntimeError(f"device only reports {disk_written} written bytes vs {size} logical bytes")
        after_log = kernel_log()
        new_lines = after_log[len(before_log):] if after_log[:len(before_log)] == before_log else after_log
        bad = [line for line in new_lines if VTX_ERROR.search(line)]
        if bad:
            raise RuntimeError("NEW kernel errors: " + " | ".join(bad[:8]))
        result["read"] = summarize_fio(check, "read")
        result["sha256_before"] = pre_hash
        result["sha256_after_remount"] = post_hash
        result["device_written_bytes"] = disk_written
        result["device_write_ratio_includes_other_io"] = round(disk_written / size, 4)
        result["status"] = ("CORRECTNESS_PASS_PERFORMANCE_NOT_VALID"
                            if args.allow_small_validation else "INCOMPLETE_POWER_CUT")
        result["reason"] = "fsync, cold remount, fio CRC, SHA256, fsck, device-write counters and dmesg OK"
    except Exception as exc:
        result["reason"] = str(exc)
        result["mounted_at_end"] = mounted
    finally:
        log_path.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"].startswith("CORRECTNESS_PASS") else 2 if result["status"] == "INCOMPLETE_POWER_CUT" else 1


if __name__ == "__main__":
    sys.exit(main())
