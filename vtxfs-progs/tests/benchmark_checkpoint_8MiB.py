#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare old 256KiB vs new 1MiB CLI checkpoint periods on 8MiB identical input.

This is a microbenchmark only, never represent results as generic disk I/O.
All images and temporary executables are disposable files in /dev/shm.
"""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/".vtxfs-backup-20261026/user-posix-p0-20261026/tools_vtxfs_inspect.c"
CORE=[
    "vtxfs_csum.c","vtxfs_leaf.c","vtxfs_leaf_balance.c",
    "vtxfs_internal.c","vtxfs_internal_balance.c","vtxfs_btree_ro.c",
    "vtxfs_btree_rw.c","vtxfs_vag.c","vtxfs_engine.c","vtxfs_posix.c",
    "vtxfs_fsck.c"
]
def ok(argv,**kw):
    p=subprocess.run([str(x) for x in argv],stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE,timeout=180,**kw)
    if p.returncode: raise RuntimeError((argv,p.returncode,p.stderr[-400:]))
    return p.stdout

def main():
    assert SRC.is_file()
    assert shutil.disk_usage("/dev/shm").free > 900*1024*1024
    with tempfile.TemporaryDirectory(prefix="vtxfs-checkpoint-bench-",dir="/dev/shm") as directory:
        d=Path(directory)
        oldbin=d/"vtxfs-old-checkpoint"
        cmd=["cc","-std=c17","-O2","-Wall","-Wextra","-Werror","-pedantic",
             "-I"+str(ROOT/"include"),"-I"+str(ROOT/"uapi"),str(SRC)]
        cmd += [str(ROOT/"core"/p) for p in CORE]
        ok([*cmd,"-o",oldbin])
        b=bytes(range(256))*32768 # 8MiB, deterministic, diverse bytes
        h=hashlib.sha256(b).hexdigest()
        reports=[]
        for label,binary in [("256KiB_before",oldbin),("1MiB_after",ROOT/"tools/vtxfs-inspect")]:
            image=d/f"{label}.img"
            ok([ROOT/"tools/mkfs.vtxfs","-b","65536",image])
            start=time.perf_counter()
            ok([binary,image,"put-stdin","/checkpoint-test.bin"],input=b)
            duration=time.perf_counter()-start
            got=ok([binary,image,"cat","/checkpoint-test.bin"])
            assert hashlib.sha256(got).hexdigest()==h
            ok([ROOT/"tools/fsck.vtxfs","-q",image])
            reports.append((label,duration))
            print(f"{label}: write 8MiB in {duration:.3f}s, throughput={8/duration:.3f}MiB/s SHA256/fsck PASS",flush=True)
        before,after=reports[0][1],reports[1][1]
        print(f"RATIO old/new={before/after:.2f}x; This is one synthetic 8MiB tmpfs test, NOT a disk benchmark.",flush=True)

if __name__=="__main__":
    main()
