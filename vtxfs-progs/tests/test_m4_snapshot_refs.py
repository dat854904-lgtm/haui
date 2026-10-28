#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""M4 pre-migration REF/SFQ snapshot consistency regression.

Uses v2.1 fsck authoritative cross-tree verification after many CoW rewrites.
DOES NOT claim complete persisted reverse owner graph.
"""
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
TOOLS=ROOT/"tools"
def invoke(*parts, stdin=None):
    p=subprocess.run([str(x) for x in parts],input=stdin,
                     stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180)
    if p.returncode:
        raise AssertionError(f"{parts}: {p.stderr[-600:]!r}")
    return p.stdout

def main():
    assert any(
        x.split(" - ")[0].split()[4]=="/dev/shm" and
        x.split(" - ")[1].split()[0]=="tmpfs"
        for x in Path("/proc/self/mountinfo").read_text().splitlines()
        if " - " in x
    ), "No tmpfs"
    assert shutil.disk_usage("/dev/shm").free >= 350*1024*1024
    with tempfile.TemporaryDirectory(dir="/dev/shm",prefix="vtxfs-m4-roaudit-") as d:
        image=Path(d)/"only-test.img"
        invoke(TOOLS/"mkfs.vtxfs","-b","65536",image)
        def cli(*args,data=None):
            return invoke(TOOLS/"vtxfs-inspect",image,*args,stdin=data)
        cli("mkdir","/","snapshots")
        data=bytes(range(256))*256
        cli("put-stdin","/snapshots/data.bin",data=data)
        base_hash=hashlib.sha256(data).hexdigest()
        for i in range(1,7):
            cli("snap",f"before-write-{i}")
            data=(bytes([(i*33) & 255])*8192)+bytes(range(256))*256
            cli("put-stdin","/snapshots/data.bin",data=data)
            assert cli("cat","/snapshots/data.bin")==data
            invoke(TOOLS/"fsck.vtxfs","-q",image)
        report={
            "test":"vtxfs-m4-pre-migration-snapshot-ref-sfq",
            "snapshots_created":6,
            "file_rewrites":6,
            "fsck_passes":6,
            "initial_sha256":base_hash,
            "final_sha256":hashlib.sha256(data).hexdigest(),
            "verified":"v2.1 REF/SFQ via fsck and active data SHA",
            "not_verified":"complete persisted data/tree reverse-owner index",
            "result":"PASS"
        }
        print(json.dumps(report,sort_keys=True))
if __name__=="__main__":
    main()
