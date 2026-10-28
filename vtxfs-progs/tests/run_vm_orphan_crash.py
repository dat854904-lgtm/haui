#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unmount-free orphan crash cut in disposable guest /dev/shm image."""
import os
import selectors
import subprocess
import tempfile
import time
from pathlib import Path
from run_vm_crash import ROOT, TOOLS, initramfs, prerequisite, qemu_cmd, call, BAD

def main():
    prerequisite()
    with tempfile.TemporaryDirectory(prefix="vtxfs-orphan-crash-",dir="/dev/shm") as d:
        t=Path(d)
        initrd=initramfs(t)
        image=t/"orphan-isolated.img"
        call([str(TOOLS/"mkfs.vtxfs"),"-b","65536",str(image)])
        p=subprocess.Popen(qemu_cmd(image,initrd,"orphancut"),cwd=ROOT,
                           stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        selector=selectors.DefaultSelector()
        selector.register(p.stdout,selectors.EVENT_READ)
        out=bytearray()
        marker=b"VTXFS_VM_ORPHAN_CUT_READY opened_fd=1 unlinked=1"
        try:
            cutoff=time.monotonic()+45
            while time.monotonic()<cutoff and p.poll() is None:
                for key,_ in selector.select(timeout=0.25):
                    out.extend(os.read(key.fd,8192))
                if marker in out:break
            assert marker in out,repr(out[-1300:])
        finally:
            if p.poll() is None:p.kill()
            p.communicate(timeout=6)
            selector.close()
        log=out.decode(errors="replace")
        print("\n".join(x for x in log.splitlines()
                        if "VTXFS_VM_" in x or "BUG:" in x),flush=True)
        assert not any(b in log for b in BAD), "guest failed before orphan cut"
        print("ORPHAN_VM_POWER_CUT completed with unlinked FD still open",flush=True)
        cp=subprocess.run(qemu_cmd(image,initrd,"orphanrecover"),cwd=ROOT,
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                          timeout=60)
        log=cp.stdout.decode(errors="replace")
        print("\n".join(x for x in log.splitlines()
                        if "VTXFS_VM_" in x or "BUG:" in x),flush=True)
        assert cp.returncode==0
        for m in ("VTXFS_VM_MOUNT_OK",
                  "VTXFS_VM_ORPHAN_RECOVER_OK name_absent=1",
                  "VTXFS_VM_UMOUNT_OK",
                  "VTXFS_VM_PASS failures=0"):
            assert m in log,m
        assert not any(b in log for b in BAD)
        call([str(TOOLS/"fsck.vtxfs"),"-q",str(image)])
        print("PASS: orphan FD survived unlink; after VM kill, next RW mount reaped orphan and fsck is clean.")
if __name__=="__main__":
    main()
