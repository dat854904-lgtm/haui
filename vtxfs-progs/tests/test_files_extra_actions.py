#!/usr/bin/env python3
"""End-to-end copy and nonoverwriting move in RAM VTXFS; never physical disk."""
import os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from vtxfs_files import engine,require_ram_volume,TOOLS,IMAGE
import subprocess,hashlib
require_ram_volume()
base=f"/actions-{os.getpid()}"
created=[]
def fails(*args):
    r=subprocess.run([str(TOOLS/"vtxfs-inspect"),str(IMAGE),*args],capture_output=True)
    assert r.returncode !=0,(args,r.stdout,r.stderr)
engine("mkdir","/",base[1:])
try:
    engine("mkdir",base,"dir")
    sample=(bytes(range(256))*4096)[:1048576]
    engine("put-stdin",base+"/source.bin",input_data=sample)
    print("created original 1MiB",flush=True)
    engine("copy-file",base+"/source.bin",base+"/copied.bin")
    assert engine("cat",base+"/copied.bin")==sample
    assert engine("cat",base+"/source.bin")==sample
    fails("copy-file",base+"/source.bin",base+"/copied.bin")
    fails("copy-file",base+"/dir",base+"/dircopy")
    engine("move-safe",base+"/copied.bin",base+"/dir/moved.bin")
    assert engine("cat",base+"/dir/moved.bin")==sample
    fails("move-safe",base+"/source.bin",base+"/dir/moved.bin")
    fails("move-safe",base+"/dir",base+"/dir/inner")
    assert engine("cat",base+"/source.bin")==sample
    print("PASS: copy 1MiB bytes, no overwrite, move file to child folder, reject directory cycle",flush=True)
finally:
    for path in [base+"/dir/moved.bin",base+"/copied.bin",base+"/source.bin"]:
        try:engine("remove",path)
        except Exception:pass
    try:engine("remove",base+"/dir")
    except Exception:pass
    engine("remove",base)
proc=subprocess.run([str(TOOLS/"fsck.vtxfs"),"-q",str(IMAGE)],capture_output=True)
assert proc.returncode==0,proc.stderr[:600]
print("PASS: fsck after copy and move",flush=True)
