#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""M2 crash durability: kill only disposable QEMU after guest fsync; reboot same image.

Not a hardware power-loss simulation. Host /dev/shm persists while QEMU dies.
Never opens a real block device, host VTXFS mount, or existing volume.
"""
import os
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
KMOD=ROOT.parent/"fs/vtxfs"
TOOLS=ROOT/"tools"
KERNEL=Path(os.environ.get("VTXFS_VM_KERNEL","/boot/vmlinuz-linux-zen"))
BAD=("BUG:","Kernel panic","general protection fault","KASAN:","VTXFS_VM_FAILURE")

def call(args, **kw):
    ret=subprocess.run(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=45,**kw)
    if ret.returncode:
        raise RuntimeError(f"{args[0]} failed={ret.returncode} {ret.stderr[-500:]!r}")
    return ret.stdout

def prerequisite():
    info=Path("/proc/self/mountinfo").read_text().splitlines()
    assert any(x.split(" - ")[0].split()[4]=="/dev/shm" and
               x.split(" - ")[1].split()[0]=="tmpfs"
               for x in info if " - " in x), "unsafe: /dev/shm not tmpfs"
    assert shutil.disk_usage("/dev/shm").free > 1250*1024*1024
    assert KERNEL.is_file()
    for name in ("qemu-system-x86_64","gcc","zstd","modinfo","modprobe","cpio"):
        assert shutil.which(name), f"missing: {name}"
    assert (KMOD/"vtxfs.ko").is_file()
    assert (TOOLS/"mkfs.vtxfs").is_file()
    assert (TOOLS/"fsck.vtxfs").is_file()

def initramfs(tmp):
    fs=tmp/"root"
    for name in ("dev","proc","sys","mnt","deps"):
        (fs/name).mkdir(parents=True)
    call(["gcc","-static","-pthread","-O2","-Wall","-Wextra","-Werror",str(ROOT/"tests/vm_guest_init.c"),
          "-o",str(fs/"init")])
    shutil.copy2(KMOD/"vtxfs.ko",fs/"vtxfs.ko")
    testmod=KMOD/"vtxfs_free_cache_test.ko"
    if testmod.is_file():shutil.copy2(testmod,fs/testmod.name)
    order=[]
    deps=call(["modinfo","-F","depends",str(KMOD/"vtxfs.ko")]).decode().strip()
    for dep in filter(None,deps.split(",")):
        lines=call(["modprobe","--show-depends",dep]).decode().splitlines()
        for line in lines:
            words=line.split()
            if len(words)<2 or words[0]!="insmod":continue
            module=Path(words[1])
            target=fs/"deps"/(module.name.removesuffix(".zst"))
            if target.exists():continue
            if module.suffix==".zst":
                target.write_bytes(call(["zstd","-dc",str(module)]))
            else:
                shutil.copy2(module,target)
            order.append("/deps/"+target.name)
    (fs/"deps/order").write_text("".join(x+"\n" for x in order))
    archive=tmp/"initramfs.cpio"
    paths=sorted([p.relative_to(fs).as_posix() for p in fs.rglob("*")])
    files=b"\x00".join(os.fsencode("./"+p) for p in paths)+b"\x00"
    with archive.open("wb") as output:
        p=subprocess.run(["cpio","-0","-o","-H","newc","--quiet"],cwd=fs,
                         input=files,stdout=output,stderr=subprocess.PIPE,timeout=15)
    assert p.returncode==0,p.stderr[-400:]
    return archive

def qemu_cmd(image, initrd, mode):
    return ["qemu-system-x86_64","-accel","kvm","-machine","q35",
        "-cpu","host","-m","1024","-smp","2","-kernel",str(KERNEL),
        "-initrd",str(initrd),
        "-append",f"console=ttyS0 rdinit=/init panic=5 loglevel=5 vtx_mode={mode}",
        "-drive",f"file={image},if=virtio,format=raw,cache=none",
        "-display","none","-monitor","none","-serial","stdio","-no-reboot"]

def force_guest_cut(image, initrd):
    p=subprocess.Popen(qemu_cmd(image,initrd,"powercut"),cwd=ROOT,
                       stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    selector=selectors.DefaultSelector()
    selector.register(p.stdout, selectors.EVENT_READ)
    output=bytearray()
    marker=b"VTXFS_VM_CRASH_READY durable_bytes=98304"
    ready=False
    try:
        deadline=time.monotonic()+28
        while time.monotonic()<deadline:
            if p.poll() is not None:break
            events=selector.select(timeout=0.25)
            for key,_ in events:
                chunk=os.read(key.fd,8192)
                if chunk:output.extend(chunk)
            if marker in output:
                ready=True
                break
        if not ready:
            raise AssertionError("guest never finished durable fsync checkpoint")
    finally:
        # Kill strictly this QEMU instance, not host processes or kernel.
        if p.poll() is None:p.kill()
        try:p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill();p.wait(timeout=5)
        selector.close()
    text=output.decode(errors="replace")
    print("M2 POWER-CUT (QEMU process only):",
          "marker seen" if ready else "not ready",flush=True)
    print("\n".join(x for x in text.splitlines() if
           "VTXFS_VM_" in x or "Kernel panic" in x or "BUG:" in x),flush=True)
    assert ready
    assert not any(x in text for x in BAD), "guest integrity/kernel failure"

def recover_guest(image, initrd):
    ret=subprocess.run(qemu_cmd(image,initrd,"recover"),cwd=ROOT,
                       stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=35)
    text=ret.stdout.decode(errors="replace")
    print("\n".join(x for x in text.splitlines() if
          "VTXFS_VM_" in x or "Kernel panic" in x or "BUG:" in x),flush=True)
    assert ret.returncode==0, f"QEMU exit={ret.returncode}"
    for marker in ("VTXFS_VM_MOUNT_OK","VTXFS_VM_RECOVER_DATA_OK bytes=98304",
                   "VTXFS_VM_UMOUNT_OK","VTXFS_VM_PASS failures=0"):
        assert marker in text, f"recovery missing marker {marker}"
    assert not any(x in text for x in BAD), "recovery kernel failure"

def verify_corruption(image, initrd, temp, *, name, ring_start, ring_count):
    """Corrupt ONLY primary-ring blocks on a newly cloned disposable image."""
    target=temp/f"{name}-corrupt-clone.img"
    assert not target.exists()
    shutil.copyfile(image,target)
    with target.open("r+b",buffering=0) as out:
        for pblk in range(ring_start,ring_start+ring_count):
            out.seek(pblk*4096)
            original=out.read(1)
            assert len(original)==1
            out.seek(pblk*4096)
            out.write(bytes([original[0]^0xff]))
    call([str(TOOLS/"fsck.vtxfs"),"-q",str(target)])
    recover_guest(target,initrd)
    call([str(TOOLS/"fsck.vtxfs"),"-q",str(target)])
    print(f"PASS: {name}: mirror recovered from all corrupted primary-ring slots",flush=True)

def verify_both_cpd_rings_fail_closed(image, initrd, temp):
    """Both CPD replicas broken: fsck and kernel must NOT accept the image."""
    target=temp/"both-cpd-rings-corrupt-clone.img"
    assert not target.exists()
    shutil.copyfile(image,target)
    with target.open("r+b",buffering=0) as out:
        for base in (8, 16384+8):
            for pblk in range(base,base+8):
                out.seek(pblk*4096)
                one=out.read(1)
                assert len(one)==1
                out.seek(pblk*4096)
                out.write(bytes([one[0]^0xff]))
    check=subprocess.run([str(TOOLS/"fsck.vtxfs"),"-q",str(target)],
                         cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                         timeout=15)
    assert check.returncode!=0, "fsck accepted image with no valid CPD"
    ret=subprocess.run(qemu_cmd(target,initrd,"reject"),cwd=ROOT,
                       stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=35)
    log=ret.stdout.decode(errors="replace")
    print("\n".join(x for x in log.splitlines()
                    if "VTXFS_VM_" in x or "mount-native:" in x),flush=True)
    assert ret.returncode==0
    assert "VTXFS_VM_REJECT_OK" in log
    assert "VTXFS_VM_PASS failures=0" in log
    assert not any(x in log for x in BAD)
    print("PASS: dual CPD corruption fails closed (fsck + guest kernel)",flush=True)

def main():
    prerequisite()
    with tempfile.TemporaryDirectory(prefix="vtxfs-m2-crash-",dir="/dev/shm") as dirname:
        temp=Path(dirname)
        initrd=initramfs(temp)
        image=temp/"only-disposable.img"
        assert not image.exists()
        call([str(TOOLS/"mkfs.vtxfs"),"-L","M2-CRASH","-b","65536",str(image)])
        call([str(TOOLS/"fsck.vtxfs"),"-q",str(image)])
        force_guest_cut(image,initrd)
        # Validate committed journal/CPD before any clean guest unmount.
        call([str(TOOLS/"fsck.vtxfs"),"-q",str(image)])
        print("PASS: fsck directly after abrupt QEMU termination",flush=True)
        # Disrupt ONLY test copies, never a user's image or mounted block device.
        verify_corruption(image,initrd,temp,name="cpd-primary",
                          ring_start=8,ring_count=8)
        verify_corruption(image,initrd,temp,name="super-primary",
                          ring_start=16,ring_count=16)
        verify_both_cpd_rings_fail_closed(image,initrd,temp)
        recover_guest(image,initrd)
        call([str(TOOLS/"fsck.vtxfs"),"-q",str(image)])
        print("PASS: re-mount RO, byte-exact recovery after fsync, host fsck",flush=True)
        print("M2 VM CRASH RECOVERY PASS (software VM cut; not real power-loss test)",flush=True)
if __name__=="__main__":
    main()
