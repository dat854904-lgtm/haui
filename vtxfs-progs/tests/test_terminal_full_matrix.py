#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Black-box VTXFS terminal CRUD regression with strict RAM-only safety.

Builds a NEW, isolated VTXFS image under /dev/shm and never touches existing
RAM volumes or Linux block devices. A report is printed for every area.
Userspace extended POSIX methods are exercised by tests/test_engine_posix.c.
"""
import hashlib
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "tools"
CASES = []
START = time.monotonic()
ACTIONS = 0

def check_tmpfs():
    lines = Path("/proc/self/mountinfo").read_text().splitlines()
    assert any(
        len(x.split(" - ")) == 2 and
        x.split(" - ")[0].split()[4] == "/dev/shm" and
        x.split(" - ")[1].split()[0] == "tmpfs"
        for x in lines), "Refusing to create test image: /dev/shm not tmpfs"
    assert shutil.disk_usage("/dev/shm").free > 350 * 1024 * 1024

def invoke(*argv, data=None, good=True, timeout=180):
    global ACTIONS
    ACTIONS += 1
    cp = subprocess.run(argv, input=data, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=timeout)
    if good and cp.returncode:
        raise AssertionError(f"{' '.join(map(str,argv[1:]))}: code={cp.returncode} "
                             f"stderr={cp.stderr[:600]!r}, out={cp.stdout[:250]!r}")
    if good is False and cp.returncode == 0:
        raise AssertionError(f"unexpectedly accepted {' '.join(map(str,argv[1:]))}: "
                             f"out={cp.stdout[:200]!r}")
    return cp.stdout

def area(title, fn):
    try:
        fn()
        CASES.append((title, "PASS", ""))
        print("PASS:", title, flush=True)
    except Exception as exc:
        CASES.append((title, "FAIL", f"{type(exc).__name__}: {exc}"))
        print("FAIL:", title, str(exc)[:230], flush=True)

def must(condition, description):
    if not condition:
        raise AssertionError(description)

def main():
    check_tmpfs()
    with tempfile.TemporaryDirectory(prefix="vtxfs-terminal-audit-", dir="/dev/shm") as tmp:
        img = Path(tmp) / "isolated.img"
        image = str(img)
        def cli(*argv, data=None, good=True, timeout=180):
            return invoke(str(BIN/"vtxfs-inspect"), image, *argv,
                          data=data, good=good, timeout=timeout)
        def fsck():
            invoke(str(BIN/"fsck.vtxfs"), "-q", image)
        def file(path, data):
            cli("put-stdin", path, data=data)
            got = cli("cat", path)
            must(got == data, f"corrupt {path}: expected {len(data)}, got {len(got)}")
        def has(path):
            return cli("stat", path, good=None) is not None  # unused
        def names(folder):
            entries = cli("ls", folder).decode("utf-8").splitlines()
            return {row.split(" name=",1)[1] for row in entries}
        def count_used():
            out = cli("bitmap", "0").decode()
            match = re.search(r"^allocated=(\d+)$", out, re.M)
            must(bool(match), "allocated field absent")
            return int(match.group(1))
        print("Created private tmpfs-only audit location:", tmp,flush=True)
        invoke(str(BIN/"mkfs.vtxfs"), "-L", "CLI-FILE-DIR-AUDIT",
               "-b", "65536", image)
        area("mkfs/info + baseline fsck", lambda: (
            must(b"65536" in cli("info"), "wrong image size"), fsck()))
        def hierarchy():
            cli("mkdir", "/", "A")
            cli("mkdir", "/A", "B")
            cli("mkdir", "/A/B", "C")
            cli("mkdir", "/", "Empty")
            must("A" in names("/") and "Empty" in names("/"), "root listing")
            must("B" in names("/A") and "C" in names("/A/B"), "nested listing")
            must(b"mode" in cli("stat", "/A/B/C"), "directory stat")
        area("mkdir nested depth 3 / ls / directory stat", hierarchy)
        area("rmdir empty / persist deletion", lambda: (
            cli("remove", "/Empty"), cli("stat", "/Empty", good=False),
            must("Empty" not in names("/"), "directory remained")))
        area("rmdir nonempty rejected, original survives", lambda: (
            cli("remove", "/A", good=False),
            must("B" in names("/A"), "nonempty directory altered")))
        area("empty regular file / size 0 / read 0", lambda: (
            file("/A/empty.txt", b""),
            must(b"size_bytes : 0" in cli("stat", "/A/empty.txt"), "wrong empty size")))
        area("inline data 1, 2048 / byte-exact roundtrip", lambda: (
            file("/A/inl", b"\x00"),
            file("/A/inl", bytes(range(256)) * 8)))
        area("inline promotion 2049 & block boundaries", lambda: (
            file("/A/boundary", b"Q"*2049),
            file("/A/boundary", bytes(range(256))*16),
            file("/A/boundary", b"Z"*4097),
            file("/A/boundary", b"M"*65535),
            file("/A/boundary", b"N"*65536),
            file("/A/boundary", b"O"*65537)))
        def overwrite():
            file("/A/B/C/test.txt", "alpha \nß\n".encode("utf-8"))
            file("/A/B/C/test.txt", b"new")
            must(b"size_bytes : 3" in cli("stat", "/A/B/C/test.txt"),
                 "not truncated on replacement")
            cli("write", "/A/B/C", "test.txt", "12345")
            must(cli("cat","/A/B/C/test.txt") == b"12345", "write from start failed")
        area("overwrite/truncate/legacy write / Unicode content", overwrite)
        data_big = bytes(range(256)) * (1048576 // 256)
        def binary():
            file("/A/B/C/data.bin", data_big)
            must(hashlib.sha256(cli("cat", "/A/B/C/data.bin")).digest() ==
                 hashlib.sha256(data_big).digest(), "sha256 mismatch")
        area("1 MiB binary/NUL/high-byte data / SHA-256", binary)
        area("8 MiB multi-checkpoint streaming / fsck", lambda: (
            file("/A/large.bin", data_big * 8), fsck()))
        area("16 MiB multi-checkpoint streaming / fsck", lambda: (
            file("/A/large.bin", data_big * 16), fsck()))
        area("zero-length replacement truncation", lambda: (
            file("/A/large.bin", b""),
            must(b"size_bytes : 0" in cli("stat", "/A/large.bin"),"failed truncate")))
        def unicode():
            cli("mkdir","/A","Việt Nam")
            file("/A/Việt Nam/đường dẫn có khoảng trắng 😃.txt","Tiếng Việt\n".encode())
            must("đường dẫn có khoảng trắng 😃.txt" in names("/A/Việt Nam"),
                 "unicode ls wrong")
        area("Unicode + spaces + emoji in paths", unicode)
        def copy_move():
            file("/A/source", data_big)
            cli("copy-file", "/A/source", "/A/B/copied")
            must(cli("cat","/A/B/copied") == data_big, "copy checksum mismatch")
            cli("copy-file", "/A/source", "/A/B/copied", good=False)
            cli("copy-file", "/A/B", "/A/dircpy", good=False)
            cli("copy-file", "/A/absent", "/A/xxx", good=False)
            cli("copy-file", "/A/source", "/A/source", good=False)
            cli("move-safe", "/A/B/copied", "/A/B/C/relocated")
            must(cli("cat","/A/B/C/relocated")==data_big,"move data loss")
            cli("stat","/A/B/copied",good=False)
            cli("move-safe", "/A/source", "/A/B/C/relocated",good=False)
            must(cli("cat","/A/source")==data_big, "collision destroyed source")
            cli("move-safe", "/A/B", "/A/B/C/B",good=False)
            must("B" in names("/A"), "directory cycle damaged tree")
        area("copy, move, collision protection & cycle prevention", copy_move)
        def rename():
            file("/A/to-rename", b"old")
            cli("move-safe","/A/to-rename","/A/renamed")
            cli("move-safe","/A/renamed","/A/renamed")
            must(cli("cat","/A/renamed")==b"old","same-name rename")
            cli("rename","/A/renamed","/A/B/C/renamed-again")
            must(cli("cat","/A/B/C/renamed-again")==b"old","rename cross-parent")
            cli("stat","/A/renamed",good=False)
        area("same-directory/cross-directory rename + no-op",rename)
        def posix_replacement():
            file("/A/overwrite_src", b"YES")
            file("/A/overwrite_dst", b"NO")
            cli("rename","/A/overwrite_src","/A/overwrite_dst")
            must(cli("cat","/A/overwrite_dst")==b"YES","POSIX replace failed")
            cli("stat","/A/overwrite_src",good=False)
        area("intentional POSIX rename destination replacement",posix_replacement)
        def negative():
            for args in (
                ("stat","/nothing"), ("cat","/nothing"),
                ("ls","/nothing"), ("remove","/nothing"),
                ("remove","/"), ("copy-file","/nothing","/A/nowhere"),
                ("move-safe","/nothing","/A/x"),
                ("move-safe","/A/source","/nowhere/file"),
                ("put-stdin","/A/source/child"),
                ("put-stdin","/A/B"),
            ):
                cli(*args, data=b"ignored" if args[0]=="put-stdin" else None,
                    good=False)
        area("missing paths, root deletion, directory-as-file / invalid targets",negative)
        def bitmap():
            info=cli("info").decode()
            must("total_vags        : 1" in info, "vag geometry")
            full=cli("bitmap","0").decode()
            fields=dict(line.split("=",1) for line in full.splitlines())
            buf=bytes.fromhex(fields["bitmap_hex"])
            must(len(buf)==4096,"bitmap size")
            bits=sum(n.bit_count() for n in buf)
            must(bits==int(fields["allocated"]),"bitmap allocated mismatch")
            must(bits+int(fields["free_blocks"])==32768,"allocation total mismatch")
            part=cli("bitmap","0","32760","8").decode()
            must("count=8" in part and "start_offset=32760" in part,"bitmap page")
            for args in (("bitmap","1"),("bitmap","-1"),("bitmap","0","32768"),
                         ("bitmap","0","32760","9"),("bitmap","0","0","0")):
                cli(*args,good=False)
        area("VAG bitmap integrity, slices & invalid bounds",bitmap)
        def dir_stress():
            cli("mkdir","/","Many")
            for i in range(90):
                file(f"/Many/{i:03}.txt",f"record {i}".encode())
            found=names("/Many")
            must(len(found)==90 and "089.txt" in found,"paged ls truncated")
            for i in range(90):
                must(cli("cat",f"/Many/{i:03}.txt")==f"record {i}".encode(),
                     f"file #{i} not readable")
            for i in reversed(range(90)):
                cli("remove",f"/Many/{i:03}.txt")
            must(names("/Many")==set(),"directory remains non-empty")
            cli("remove","/Many")
        area("90 files: paginated ls, sequential reads, reverse deletes",dir_stress)
        def repeated():
            rng=random.Random(20261026)
            known={}
            cli("mkdir","/","Fuzz")
            for i in range(55):
                name=f"/Fuzz/key-{rng.randrange(25):02d}.dat"
                op=rng.choice(["write","replace","remove","copy"])
                if op in ("write","replace"):
                    contents=rng.randbytes(rng.randint(0,16384))
                    file(name,contents)
                    known[name]=contents
                elif op=="remove" and name in known:
                    cli("remove",name)
                    known.pop(name)
                elif op=="copy" and name in known:
                    new=f"/Fuzz/copy-{i}.dat"
                    cli("copy-file",name,new)
                    known[new]=known[name]
            for name, expected in known.items():
                must(cli("cat",name)==expected,"random model mismatch at "+name)
            for name in list(known):
                cli("remove",name)
            cli("remove","/Fuzz")
        area("55 deterministic randomized create/write/replace/delete/copy ops",repeated)
        def snapshot():
            cli("snap","terminal-audit")
            must(b"terminal-audit" in invoke(str(BIN/"vtxfs"/"vtxfs"),
                                            "snapshot","list",image),
                 "snapshot not listed")
        area("RO snapshot create + listing",snapshot)
        def final():
            fsck()
            must("A" in names("/"),"lost source after remount")
            must(cli("cat","/A/source")==data_big,"content lost after many checkpoints")
            must(cli("cat","/A/B/C/relocated")==data_big,"copied content lost")
            fsck()
        area("final remount persistence + fsck",final)
        elapsed=round(time.monotonic()-START,2)
        passed=sum(t=="PASS" for _,t,_ in CASES)
        print(f"SUMMARY {passed}/{len(CASES)} groups, {ACTIONS} CLI calls, "
              f"{elapsed}s; all files under isolated /dev/shm directory",flush=True)
        if passed<len(CASES):
            for label,status,detail in CASES:
                if status!="PASS":
                    print("FAILED CASE:",label,"=>",detail[:800],flush=True)
            return 1
        return 0

if __name__=="__main__":
    sys.exit(main())
