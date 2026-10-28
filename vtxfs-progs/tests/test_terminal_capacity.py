#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Large file, crossing 128MiB VAG and ENOSPC tests on disposable tmpfs images.

Each image is unique, freshly formatted, and is deleted with TemporaryDirectory.
Never touches existing user images. Shell redirection and block devices forbidden.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
TOOL=ROOT/"tools"
SIZE=1024*1024
PIECE=(bytes(range(256))*256) # 64KiB
assert len(PIECE)==65536
def run(*args,good=True,timeout=240):
    r=subprocess.run(args,capture_output=True,timeout=timeout)
    if good and r.returncode:
        raise AssertionError(f"{args}: code {r.returncode}: {r.stderr[-500:]}")
    return r

def transfer(img,mib,expect_success):
    started=time.monotonic()
    cmd=[str(TOOL/"vtxfs-inspect"),str(img),"put-stdin","/large.bin"]
    source_digest=hashlib.sha256()
    sent=0
    with subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as proc:
        error_pipe=False
        for n in range(mib*16):
            try:
                proc.stdin.write(PIECE)
                sent+=len(PIECE)
                if expect_success:
                    source_digest.update(PIECE)
            except BrokenPipeError:
                error_pipe=True
                break
            if (n+1)%512==0:
                print(f"  sent {(n+1)//16} MiB",flush=True)
        try:proc.stdin.close()
        except BrokenPipeError:error_pipe=True
        out=proc.stdout.read()
        err=proc.stderr.read()
        exitcode=proc.wait(timeout=180)
    print(f"  writer returned code={exitcode}, stream={sent//SIZE} MiB, "
          f"duration={time.monotonic()-started:.1f}s, "
          f"stderr={err[-250:]!r}",flush=True)
    if expect_success:
        assert exitcode==0,(exitcode,err[-350:])
        assert int(out.split(b"put_bytes=")[-1].strip())==mib*SIZE
        got=hashlib.sha256()
        r=subprocess.Popen([str(TOOL/"vtxfs-inspect"),str(img),"cat","/large.bin"],
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        actual=0
        while True:
            buf=r.stdout.read(262144)
            if not buf:break
            actual+=len(buf)
            got.update(buf)
        msg=r.stderr.read()
        status=r.wait(timeout=180)
        assert status==0,(status,msg[-350:])
        assert actual==mib*SIZE,(actual,mib*SIZE)
        assert source_digest.digest()==got.digest(),"corrupt large file checksum"
        print(f"PASS: {mib} MiB cross-VAG binary sha256 roundtrip",flush=True)
    else:
        assert exitcode!=0,"ENOSPC not detected; capacity test unexpectedly succeeded"
        assert b"No space" in err or b"space" in err.lower() or b"ENOSPC" in err, \
            "failed for a reason other than ENOSPC: "+repr(err[-350:])
        print("PASS: 256MiB image rejects 144MiB write with ENOSPC",flush=True)
    fs=run(str(TOOL/"fsck.vtxfs"),"-q",str(img),good=False)
    if fs.returncode!=0:
        print("FAIL: fsck after large write:",fs.stderr[-550:],flush=True)
        return False
    print("PASS: fsck after", "cross-VAG" if expect_success else "ENOSPC",flush=True)
    return True

def main():
    mounts=Path("/proc/self/mountinfo").read_text().splitlines()
    assert any(x.split(" - ")[0].split()[4]=="/dev/shm" and
               x.split(" - ")[1].split()[0]=="tmpfs"
               for x in mounts if " - " in x)
    assert shutil.disk_usage("/dev/shm").free>750*SIZE
    results=[]
    with tempfile.TemporaryDirectory(prefix="vtxfs-capacity-",dir="/dev/shm") as temp:
        for size_mib,write_mib,should_succeed in [(512,144,True),(256,144,False)]:
            img=Path(temp)/f"test-{size_mib}.img"
            run(str(TOOL/"mkfs.vtxfs"),"-b",str(size_mib*256),str(img))
            print(f"CASE image {size_mib}MiB; write {write_mib}MiB "
                  f"expected {'success' if should_succeed else 'ENOSPC'}",
                  flush=True)
            try:
                results.append(transfer(img,write_mib,should_succeed))
            except Exception as exc:
                print("FAIL: capacity",size_mib,type(exc).__name__,str(exc),flush=True)
                results.append(False)
    print(f"CAPACITY SUMMARY {sum(results)}/{len(results)} cases pass",flush=True)
    return 0 if all(results) else 1

if __name__=="__main__":
    sys.exit(main())
