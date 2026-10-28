#!/usr/bin/env python3
import os, hashlib, subprocess, sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from vtxfs_files import require_ram_volume, engine, TOOLS, IMAGE, STREAM_CHUNK
require_ram_volume()
size_mib = int(sys.argv[1]) if len(sys.argv)>1 else 4
path = "/large-test-%d-%dMiB.bin" % (os.getpid(),size_mib)
data = STREAM_CHUNK * (size_mib * 16)
assert len(data)==size_mib*1048576
print(f"START writing {size_mib} MiB to RAM-backed VTXFS",flush=True)
try:
    result = subprocess.run([str(TOOLS/"vtxfs-inspect"), str(IMAGE),"put-stdin",path],
                             input=data, capture_output=True,timeout=260)
    print("put exit",result.returncode,result.stdout[:200],result.stderr[:200],flush=True)
    assert result.returncode==0
    value=engine("cat",path)
    assert hashlib.sha256(value).digest()==hashlib.sha256(data).digest()
    print(f"PASS {size_mib} MiB binary content hash/checkpoint roundtrip",flush=True)
finally:
    try: engine("remove",path)
    except Exception as exc: print("cleanup error",exc,flush=True)
check=subprocess.run([str(TOOLS/"fsck.vtxfs"), "-q", str(IMAGE)],capture_output=True,timeout=120)
assert check.returncode==0, check.stderr[:500]
print("PASS fsck after large write",flush=True)
