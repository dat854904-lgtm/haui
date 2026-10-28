#!/usr/bin/env python3
"""Terminal VTXFS operations regression using a disposable tmpfs image."""
import os, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
IMAGE=Path("/dev/shm")/f"vtxfsctl-audit-{os.getpid()}.img"
COUNT=0
def cmd(*args,data=None,fail=False):
    global COUNT
    COUNT+=1
    r=subprocess.run(["python3",str(ROOT/"tools/vtxfsctl.py"),str(IMAGE),*args],
                     input=data,capture_output=True,timeout=240)
    if fail: assert r.returncode!=0,(args,r.stdout,r.stderr)
    else: assert r.returncode==0,(args,r.stdout[-250:],r.stderr[-700:])
    return r.stdout
def main():
    n=0
    def okay(name):
        nonlocal n
        n+=1
        print("PASS",n,name,flush=True)
    mk=subprocess.run([str(ROOT/"tools/mkfs.vtxfs"),"-b","65536",str(IMAGE)],
                      capture_output=True)
    assert mk.returncode==0,mk.stderr
    assert b"total_blocks" in cmd("info")
    assert b"Free alloc" in cmd("disk-space")
    assert b"VAG count" in cmd("check-space")
    assert b"FSCK OK" in cmd("fsck")
    okay("info/space/fsck")
    cmd("mkdir","/docs")
    cmd("create","/docs/sub","--dir")
    cmd("create","/docs/sub/file.txt")
    cmd("edit","/docs/sub/file.txt",data="Xin chào VTXFS".encode())
    assert cmd("open","/docs/sub/file.txt")=="Xin chào VTXFS".encode()
    assert b"size_bytes" in cmd("get-info","/docs/sub/file.txt")
    assert b"sub" in cmd("open","/docs")
    okay("create/open/edit/stat/dir")
    cmd("rename","/docs/sub/file.txt","new.txt")
    cmd("cut","/docs/sub/new.txt")
    cmd("paste","/docs")
    assert cmd("cat","/docs/new.txt")=="Xin chào VTXFS".encode()
    cmd("copy-select","/docs/new.txt")
    cmd("paste","/docs/sub")
    assert cmd("cat","/docs/sub/new.txt")==cmd("cat","/docs/new.txt")
    okay("rename/cut/copy-select/paste")
    cmd("copy","/docs","/docs-copy")
    assert cmd("cat","/docs-copy/sub/new.txt")==cmd("cat","/docs/new.txt")
    cmd("copy","/docs","/docs-copy",fail=True)
    cmd("copy","/docs","/docs/sub/loop",fail=True)
    cmd("move","/docs-copy","/docs-moved")
    assert b"name=sub" in cmd("ls","/docs-moved")
    okay("recursive copy / no overwrite / safe move")
    cmd("symlink","/docs/new.txt","/pointer")
    assert cmd("readlink","/pointer").strip()==b"/docs/new.txt"
    cmd("shortcut","/docs/new.txt","/shortcut")
    assert cmd("open","/shortcut").strip()==b"/docs/new.txt"
    cmd("copy","/pointer","/pointer-copy")
    assert cmd("readlink","/pointer-copy").strip()==b"/docs/new.txt"
    okay("symlink/shortcut/copy symlink")
    assert b"/docs/sub/new.txt" in cmd("search","new")
    assert b"/docs/sub/new.txt" in cmd("sort","/docs/sub","--by","mtime")
    assert b"/docs/sub/new.txt" in cmd("filter","/docs/sub","--ext",".txt")
    assert b"Link target" in cmd("properties","/pointer")
    okay("search/sort/filter/properties")
    cmd("readonly","/docs/new.txt")
    assert b"0100444" in cmd("stat","/docs/new.txt")
    cmd("readonly","/docs/new.txt","--off")
    assert b"0100644" in cmd("stat","/docs/new.txt")
    cmd("hide","/docs/new.txt")
    assert b"name=.new.txt" in cmd("ls","/docs")
    cmd("unhide","/docs/.new.txt")
    okay("readonly/hide/unhide")
    cmd("delete","/docs/sub/new.txt")
    assert b"name=new.txt" not in cmd("ls","/docs/sub")
    names=cmd("ls","/.vtxfs-trash").decode().splitlines()
    assert len(names)==1,names
    deleted=names[0].split(" name=",1)[1]
    cmd("restore",deleted)
    assert cmd("cat","/docs/sub/new.txt")=="Xin chào VTXFS".encode()
    okay("delete / restore EXACT original folder")
    cmd("zip","/docs","/archive.zip")
    cmd("mkdir","/extracted")
    cmd("unzip","/archive.zip","/extracted")
    assert cmd("cat","/extracted/docs/sub/new.txt")==cmd("cat","/docs/sub/new.txt")
    cmd("unzip","/archive.zip","/extracted",fail=True)
    okay("zip/unzip nested content, collision reject")
    cmd("permanently-delete","/docs-moved",fail=True)
    cmd("permanently-delete","/docs-moved","--yes")
    cmd("delete","/docs/new.txt")
    cmd("empty-trash",fail=True)
    cmd("empty-trash","--yes")
    assert not cmd("ls","/.vtxfs-trash").strip()
    assert b"FSCK OK" in cmd("fsck")
    okay("permanently-delete / empty-trash / fsck")
    cmd("permanently-delete","/","--yes",fail=True)
    cmd("create","/docs/../invalid",fail=True)
    cmd("copy","/docs","/docs/sub/loop",fail=True)
    cmd("restore","nonexistent",fail=True)
    okay("safety / invalid paths")
    return n
if __name__=="__main__":
    try:
        count=main()
        print(f"PASS EXTENDED TERMINAL MATRIX: {count} groups, {COUNT} subprocess calls",flush=True)
    finally:
        if IMAGE.is_file() and not IMAGE.is_symlink():IMAGE.unlink()
