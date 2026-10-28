#!/usr/bin/env python3
"""Negative security/collision tests for terminal trash and ZIP extraction."""
import io
import os
import subprocess
from pathlib import Path
import zipfile
ROOT=Path(__file__).resolve().parents[1]
IMAGE=Path("/dev/shm")/f"vtxfsctl-negative-{os.getpid()}.img"
BIN=ROOT/"tools/vtxfsctl.py"
def run(*args,data=None,fail=False):
    result=subprocess.run(["python3",str(BIN),str(IMAGE),*args],
                          capture_output=True,input=data,timeout=120)
    assert (result.returncode!=0 if fail else result.returncode==0), \
        (args,result.stderr[-300:],result.stdout[-200:])
    return result.stdout
def make_zip(entries):
    stream=io.BytesIO()
    with zipfile.ZipFile(stream,"w") as z:
        for name,data in entries:z.writestr(name,data)
    return stream.getvalue()
try:
    mk=subprocess.run([str(ROOT/"tools/mkfs.vtxfs"),str(IMAGE)],capture_output=True)
    assert mk.returncode==0,mk.stderr
    run("mkdir","/folder")
    run("create","/folder/file")
    run("edit","/folder/file",data=b"original")
    run("delete","/folder/file")
    run("create","/folder/file")
    run("edit","/folder/file",data=b"collision")
    name=run("ls","/.vtxfs-trash").decode().split(" name=",1)[1].strip()
    run("restore",name,fail=True)
    assert run("cat","/folder/file")==b"collision"
    assert b"name=" in run("ls","/.vtxfs-trash")
    print("PASS: restore refuses overwriting existing original path",flush=True)
    for label,entries in [
        ("dotdot",[("../evil.txt",b"escape")]),
        ("absolute",[("/root.txt",b"escape")]),
        ("duplicate",[("dup.txt",b"one"),("dup.txt",b"two")]),
        ("conflict",[("same",b"file"),("same/sub",b"sub")]),
    ]:
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore",UserWarning)
            blob=make_zip(entries)
        run("edit","/"+label+".zip",data=blob)
        run("unzip","/"+label+".zip","/folder",fail=True)
        assert b"evil" not in run("ls","/")
        assert b"dup.txt" not in run("ls","/folder")
        print("PASS: malicious ZIP case rejected",label,flush=True)
    run("empty-trash","--yes")
    assert not run("ls","/.vtxfs-trash")
    assert b"FSCK OK" in run("fsck")
    print("PASS: fsck after negative operations",flush=True)
finally:
    if IMAGE.is_file() and not IMAGE.is_symlink():IMAGE.unlink()
