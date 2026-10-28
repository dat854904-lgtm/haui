#!/usr/bin/env python3
"""CLI pathological path / directory / overwrite test on a NEW RAM image."""
from pathlib import Path
import subprocess
import tempfile
import sys
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/"tools"
groups=[]
def record(name,result):
    groups.append((name,result))
    print(("PASS" if result else "FAIL")+": "+name,flush=True)
with tempfile.TemporaryDirectory(prefix="vtxfs-edge-",dir="/dev/shm") as d:
    img=str(Path(d)/"edge.img")
    def cli(*args,data=None):
        return subprocess.run([str(T/"vtxfs-inspect"),img,*args],
                              input=data,capture_output=True,timeout=30)
    init=subprocess.run([str(T/"mkfs.vtxfs"),img],capture_output=True)
    assert init.returncode==0
    assert cli("put-stdin","/regular",data=b"DATA").returncode==0
    assert cli("mkdir","/","dir").returncode==0
    record("ls on regular file must fail ENOTDIR",cli("ls","/regular").returncode!=0)
    record("cat on directory must fail",cli("cat","/dir").returncode!=0)
    record("mkdir under regular file must fail",cli("mkdir","/regular","child").returncode!=0)
    record("duplicate directory name fails",cli("mkdir","/","dir").returncode!=0)
    record("rmdir directory with child fails",bool(cli("mkdir","/dir","child").returncode==0 and
                                                  cli("remove","/dir").returncode!=0))
    record("copy regular file into itself fails",cli("copy-file","/regular","/regular").returncode!=0)
    record("safe move collision fails",cli("move-safe","/regular","/dir").returncode!=0)
    record("remove directory-as-file name no accidental truncate",
           cli("put-stdin","/dir",data=b"x").returncode!=0)
    record("legacy rename to existing file replaces intentionally",
           bool(cli("put-stdin","/another",data=b"NEW").returncode==0 and
                cli("rename","/another","/regular").returncode==0 and
                cli("cat","/regular").stdout==b"NEW"))
    # These are on a disposable volume, but still probe reserved directory names
    record("mkdir '.' forbidden",cli("mkdir","/",".").returncode!=0)
    record("mkdir '..' forbidden",cli("mkdir","/","..").returncode!=0)
    record("mkdir filename >255 bytes forbidden",
           cli("mkdir","/","N"*256).returncode!=0)
    record("empty basename deletion forbidden",cli("remove","/dir/").returncode!=0)
    record("safe move directory into own child forbidden",
           cli("move-safe","/dir","/dir/child/in").returncode!=0)
    stat=cli("stat","/dir")
    record("directory inode still reachable after invalid operations",stat.returncode==0)
    check=subprocess.run([str(T/"fsck.vtxfs"),"-q",img],capture_output=True)
    record("fsck after invalid operations",check.returncode==0)
    if check.returncode: print(check.stderr[-700:],flush=True)
print(f"EDGE SUMMARY {sum(v for _,v in groups)}/{len(groups)}",flush=True)
sys.exit(0 if all(v for _,v in groups) else 1)
