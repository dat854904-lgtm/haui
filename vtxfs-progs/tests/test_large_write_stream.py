#!/usr/bin/env python3
import os, subprocess, hashlib, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from vtxfs_files import TOOLS, IMAGE, engine, require_ram_volume
require_ram_volume()
name = "/large-stream-test-%d.bin" % os.getpid()
sample = bytes(range(256))*2048  # 512 KiB; contains embedded null bytes
print("testing",len(sample),name,flush=True)
try:
    proc = subprocess.run([str(TOOLS/"vtxfs-inspect"), str(IMAGE),"put-stdin",name],
                          input=sample,capture_output=True,timeout=90)
    print(proc.returncode,proc.stdout[:200],proc.stderr[:500],flush=True)
    assert proc.returncode == 0
    retrieved=engine("cat",name)
    print("hash",hashlib.sha256(sample).hexdigest(),hashlib.sha256(retrieved).hexdigest(),flush=True)
    assert sample==retrieved
    status=engine("stat",name).decode()
    print(status,flush=True)
    print("PASS: 512 KiB binary write/read/checksum via stdin and checkpoint")
finally:
    try: engine("remove",name)
    except Exception as exc: print("cleanup failed",exc)
