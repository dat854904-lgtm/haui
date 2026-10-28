#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""dm-log-writes FUA replay on disposable tmpfs-backed devices or QEMU guest VM.

Supports:
  1) Host root mode (when euid == 0) using disposable /dev/shm loop devices.
  2) Unprivileged QEMU guest VM mode (when euid != 0 and QEMU + dm-log-writes.ko
     are available), running the real Linux kernel dm-log-writes target inside an
     isolated guest VM and replaying every FUA barrier on the host via replay-log.
Never passes an existing volume, block device, or user's mount to dmsetup.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KMOD = ROOT.parent / "fs/vtxfs"
KERNEL = Path(os.environ.get("VTXFS_VM_KERNEL", "/boot/vmlinuz-linux-zen"))
IMAGE_SIZE = 256 * 1024 * 1024
DATA_SIZE = 4 * 1024 * 1024
BAD_MARKERS = ("BUG:", "Kernel panic", "general protection fault", "KASAN:", "VTXFS_VM_FAILURE")


def call(*args, timeout=180):
    p = subprocess.run([str(x) for x in args], capture_output=True, text=True,
                       timeout=timeout)
    if p.returncode:
        raise RuntimeError(f"{args[0]} rc={p.returncode}: {(p.stderr or p.stdout)[-1000:]}")
    return p.stdout.strip()


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


def hash_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1024 * 1024)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def hash_inspected_file(img_path, vtx_path):
    p = subprocess.run([str(ROOT / "tools/vtxfs-inspect"), str(img_path), "cat", vtx_path],
                       capture_output=True, timeout=60)
    if p.returncode != 0:
        raise RuntimeError(f"vtxfs-inspect cat {vtx_path} failed: {p.stderr.decode(errors='replace')}")
    return len(p.stdout), hashlib.sha256(p.stdout).hexdigest()


def get_replay_bin():
    return shutil.which("replay-log") or (
        str(ROOT / "tools/replay-log") if (ROOT / "tools/replay-log").is_file() else None
    )


def has_guest_dm_log_writes():
    if not KERNEL.is_file() or not shutil.which("qemu-system-x86_64") or not shutil.which("modprobe"):
        return False
    p = subprocess.run(["modprobe", "--show-depends", "dm-log-writes"],
                       capture_output=True, text=True)
    return p.returncode == 0 and "dm-log-writes" in p.stdout


def preflight():
    replay_bin = get_replay_bin()
    vm_dm_ok = has_guest_dm_log_writes()
    check = {
        "tmpfs": any(" /dev/shm " in line and " - tmpfs " in line
                     for line in Path("/proc/self/mountinfo").read_text().splitlines()),
        "root_or_qemu_vm": (os.geteuid() == 0) or vm_dm_ok,
        "replay_log": bool(replay_bin),
        "vtxfs_module": (KMOD / "vtxfs.ko").is_file(),
        "mkfs": (ROOT / "tools/mkfs.vtxfs").is_file(),
        "fsck": (ROOT / "tools/fsck.vtxfs").is_file(),
        "inspect": (ROOT / "tools/vtxfs-inspect").is_file(),
        "space": shutil.disk_usage("/dev/shm").free > 1300 * 1024 * 1024,
        "dm_log_writes": False,
    }
    if os.geteuid() == 0 and shutil.which("dmsetup"):
        p = subprocess.run(["dmsetup", "targets"], capture_output=True, text=True)
        check["dm_log_writes"] = p.returncode == 0 and "log-writes" in p.stdout
    elif vm_dm_ok:
        check["dm_log_writes"] = True
    return check


def build_dm_initramfs(tmp):
    fs = tmp / "root"
    for name in ("dev", "proc", "sys", "mnt", "deps"):
        (fs / name).mkdir(parents=True, exist_ok=True)
    call("gcc", "-static", "-pthread", "-O2", "-Wall", "-Wextra", "-Werror",
         ROOT / "tests/vm_guest_init.c", "-o", fs / "init")
    shutil.copy2(KMOD / "vtxfs.ko", fs / "vtxfs.ko")
    testmod = KMOD / "vtxfs_free_cache_test.ko"
    if testmod.is_file():
        shutil.copy2(testmod, fs / testmod.name)

    order = []
    deps = call("modinfo", "-F", "depends", KMOD / "vtxfs.ko")
    dep_list = [d for d in deps.split(",") if d] + ["dm-log-writes"]
    for dep in dep_list:
        lines = call("modprobe", "--show-depends", dep).splitlines()
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


def run_vm_dm_log_writes(tmp, result):
    base = tmp / "backend.img"
    log = tmp / "log.img"
    replay = tmp / "replay.img"
    replay_fua = tmp / "replay-fua.img"

    call(ROOT / "tools/mkfs.vtxfs", "-b", "65536", base)
    shutil.copyfile(base, replay)
    shutil.copyfile(base, replay_fua)
    with log.open("wb") as f:
        f.truncate(IMAGE_SIZE)

    initrd = build_dm_initramfs(tmp)
    start = time.monotonic()
    qemu_cmd = [
        "qemu-system-x86_64", "-accel", "kvm", "-machine", "q35",
        "-cpu", "host", "-m", "1024", "-smp", "2",
        "-kernel", str(KERNEL), "-initrd", str(initrd),
        "-append", "console=ttyS0 rdinit=/init panic=5 loglevel=5 vtx_mode=dmlogwrites",
        "-drive", f"file={base},if=virtio,format=raw,cache=none",
        "-drive", f"file={log},if=virtio,format=raw,cache=none",
        "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
    ]
    with open(os.devnull, "rb") as devnull:
        p = subprocess.run(qemu_cmd, cwd=ROOT, stdin=devnull,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=90)
    elapsed = time.monotonic() - start
    out = p.stdout
    if any(b in out for b in BAD_MARKERS) or "VTXFS_VM_DMLOGWRITES_OK" not in out or "VTXFS_VM_PASS failures=0" not in out:
        raise RuntimeError(f"QEMU dm-log-writes guest failed:\n{out[-1500:]}")

    expected = expected_rc_stream_sha256(DATA_SIZE)
    replay_bin = get_replay_bin()

    # 1. Replay up to the 'fsync' mark and verify filesystem + data SHA-256
    call(replay_bin, "--log", log, "--replay", replay, "--end-mark", "fsync")
    call(ROOT / "tools/fsck.vtxfs", "-q", replay)
    got_len, got_sha = hash_inspected_file(replay, "/durable.bin")
    if got_len != DATA_SIZE or got_sha != expected:
        raise RuntimeError(f"AFTER-REPLAY mismatch: len={got_len} sha={got_sha} expected={expected}")

    # 2. Replay every FUA barrier up to 'fsync' and run fsck.vtxfs -q at each FUA point
    fua_out = call(replay_bin, "-v", "--log", log, "--replay", replay_fua,
                   "--end-mark", "fsync", "--check", "fua",
                   "--fsck", f"{ROOT / 'tools/fsck.vtxfs'} -q {replay_fua}")
    call(ROOT / "tools/fsck.vtxfs", "-q", replay_fua)
    got_len_fua, got_sha_fua = hash_inspected_file(replay_fua, "/durable.bin")
    if got_len_fua != DATA_SIZE or got_sha_fua != expected:
        raise RuntimeError("FUA-REPLAY SHA256 mismatch")

    result.update(
        status="CORRECTNESS_PASS_NOT_DISK_BENCHMARK",
        execution_mode="qemu_guest_dm_log_writes",
        power_cut_replayed=True,
        fua_points_checked=True,
        guest_elapsed_seconds=round(elapsed, 3),
        logical_written_bytes=DATA_SIZE,
        fsync_mark_sha256=expected,
        replay_log_summary=fua_out,
        note="Real Linux dm-log-writes kernel target executed in isolated QEMU guest VM on /dev/shm; every FUA point verified with fsck.vtxfs -q",
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run", action="store_true",
                    help="Execute dm-log-writes FUA replay on disposable /dev/shm images")
    args = ap.parse_args()
    check = preflight()
    if not args.run or not all(check.values()):
        print(json.dumps({"status": "BLOCKED" if args.run else "PREFLIGHT",
                          "requirements": check,
                          "note": "Pass --run to execute on disposable /dev/shm images."}, indent=2))
        return 2 if args.run else 0

    result = {"status": "INVALID", "power_cut_replayed": False,
              "fua_points_checked": False}
    with tempfile.TemporaryDirectory(prefix="vtxfs-dm-", dir="/dev/shm") as temp:
        tmp = Path(temp)
        if os.geteuid() != 0:
            try:
                run_vm_dm_log_writes(tmp, result)
            except Exception as exc:
                result["error"] = str(exc)
            print(json.dumps(result, indent=2))
            return 0 if result["status"] == "CORRECTNESS_PASS_NOT_DISK_BENCHMARK" else 1

        name = f"vtxfs-logs-{os.getpid()}"
        loops = []
        mapped = False
        mounted = False
        mountpoint = None
        try:
            base, log, replay, replay_fua = (
                tmp / "backend.img", tmp / "log.img",
                tmp / "replay.img", tmp / "replay-fua.img")
            call(ROOT / "tools/mkfs.vtxfs", "-b", "65536", base)
            shutil.copyfile(base, replay)
            shutil.copyfile(base, replay_fua)
            with log.open("wb") as f:
                f.truncate(IMAGE_SIZE * 2)
            for image in (base, log, replay, replay_fua):
                dev = call("losetup", "--find", "--show", image)
                if not dev.startswith("/dev/loop"):
                    raise RuntimeError("losetup returned an unexpected device")
                loops.append(dev)
            backend, logdev, target, target_fua = loops
            sectors = int(call("blockdev", "--getsz", backend))
            table = f"0 {sectors} log-writes {backend} {logdev}"
            call("dmsetup", "create", name, "--table", table)
            mapped = True
            mapper = Path("/dev/mapper") / name
            mountpoint = tmp / "mnt"
            mountpoint.mkdir()
            call("mount", "-t", "vtxfs", mapper, mountpoint)
            mounted = True
            fd_path = mountpoint / "durable.bin"
            start = time.monotonic()
            h = hashlib.sha256()
            with fd_path.open("wb", buffering=0) as f:
                for _ in range(DATA_SIZE // 65536):
                    b = os.urandom(65536)
                    n = f.write(b)
                    if n != len(b):
                        raise RuntimeError(f"SHORT WRITE: {n} != {len(b)}")
                    h.update(b)
                os.fsync(f.fileno())
            dirfd = os.open(mountpoint, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(dirfd)
            finally:
                os.close(dirfd)
            result["write_and_fsync_seconds"] = time.monotonic() - start
            result["logical_written_bytes"] = DATA_SIZE
            expected = h.hexdigest()
            if hash_file(fd_path) != expected:
                raise RuntimeError("BEFORE-REPLAY SHA256 mismatch")
            call("dmsetup", "message", name, "0", "mark", "fsync")
            call("umount", mountpoint)
            mounted = False
            call("dmsetup", "remove", name)
            mapped = False
            replay_bin = get_replay_bin()
            call(replay_bin, "--log", logdev, "--replay", target,
                 "--end-mark", "fsync")
            call(ROOT / "tools/fsck.vtxfs", "-q", target)
            call("mount", "-t", "vtxfs", "-o", "ro", target, mountpoint)
            mounted = True
            restored = mountpoint / "durable.bin"
            if restored.stat().st_size != DATA_SIZE or hash_file(restored) != expected:
                raise RuntimeError("AFTER-REPLAY fsynced data missing/corrupt")
            call("umount", mountpoint)
            mounted = False
            result["fsync_mark_sha256"] = expected

            call(replay_bin, "--log", logdev, "--replay", target_fua,
                 "--end-mark", "fsync", "--check", "fua",
                 "--fsck", f"{ROOT / 'tools/fsck.vtxfs'} -q {target_fua}")
            call(ROOT / "tools/fsck.vtxfs", "-q", target_fua)
            call("mount", "-t", "vtxfs", "-o", "ro", target_fua, mountpoint)
            mounted = True
            if (mountpoint / "durable.bin").stat().st_size != DATA_SIZE or (
                hash_file(mountpoint / "durable.bin") != expected
            ):
                raise RuntimeError("FUA-REPLAY SHA256 mismatch")
            call("umount", mountpoint)
            mounted = False
            result.update(status="CORRECTNESS_PASS_NOT_DISK_BENCHMARK",
                          power_cut_replayed=True,
                          fua_points_checked=True,
                          note="Loop-backed /dev/shm devices: durability evidence, "
                               "not throughput/latency of a physical disk")
        except Exception as exc:
            result["error"] = str(exc)
        finally:
            if mounted and mountpoint:
                subprocess.run(["umount", str(mountpoint)], capture_output=True)
            if mapped:
                subprocess.run(["dmsetup", "remove", name], capture_output=True)
            for dev in reversed(loops):
                subprocess.run(["losetup", "-d", dev], capture_output=True)
        print(json.dumps(result, indent=2))
        return 0 if result["status"] == "CORRECTNESS_PASS_NOT_DISK_BENCHMARK" else 1


if __name__ == "__main__":
    sys.exit(main())
