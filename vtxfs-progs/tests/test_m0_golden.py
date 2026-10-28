#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""M0 golden v2.1 contract: freshly formatted RAM image, byte-exact lifecycle.

This is not a fixed binary golden image: magic/layout/feature details are
validated with fsck and the artifact is a reproducible semantic manifest.
No existing volume or disk is opened for write.
"""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
TOOLS=ROOT/"tools"
def execute(*args, data=None):
    out=subprocess.run([str(arg) for arg in args],input=data,stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE,timeout=120)
    if out.returncode:raise AssertionError(f"{args}: {out.stderr[-450:]!r}")
    return out.stdout

def main():
    mountinfo=Path("/proc/self/mountinfo").read_text()
    assert any(x.split(" - ")[0].split()[4] == "/dev/shm" and
               x.split(" - ")[1].split()[0]=="tmpfs"
               for x in mountinfo.splitlines() if " - " in x)
    assert shutil.disk_usage("/dev/shm").free > 350*1024*1024
    with tempfile.TemporaryDirectory(prefix="vtxfs-golden-m0-",dir="/dev/shm") as tmp:
        image=Path(tmp)/"created-only.img"
        assert not image.exists()
        execute(TOOLS/"mkfs.vtxfs","-L","M0-GOLDEN","-b","65536",image)
        execute(TOOLS/"fsck.vtxfs","-q",image)
        info=execute(TOOLS/"vtxfs-inspect",image,"info").decode()
        assert re.search(r"total_blocks\s*:\s*65536",info)
        def cmd(*argv, data=None):
            return execute(TOOLS/"vtxfs-inspect",image,*argv,data=data)
        cmd("mkdir","/","golden")
        binary=(bytes(range(256))*16)+b"\0\xff\xff\x00END"
        cmd("put-stdin","/golden/blob.bin",data=binary)
        got=cmd("cat","/golden/blob.bin")
        assert got==binary
        assert b"blob.bin" in cmd("ls","/golden")
        assert b"size_bytes" in cmd("stat","/golden/blob.bin")
        cmd("snap","golden-snapshot")
        execute(TOOLS/"fsck.vtxfs","-q",image)
        manifest={
            "contract":"vtxfs-v2.1-m0-semantic-golden-v1",
            "format_blocks":65536,
            "payload_size":len(binary),
            "payload_sha256":hashlib.sha256(binary).hexdigest(),
            "operations":["mkfs","fsck","mkdir","put-stdin","cat","ls","stat","snapshot","fsck"],
            "image_safety":"new temporary regular file in /dev/shm only",
            "on_disk_feature_bits_changed":False,
            "result":"PASS"
        }
        print(json.dumps(manifest,ensure_ascii=False,sort_keys=True))
        return manifest
if __name__=="__main__":
    main()
