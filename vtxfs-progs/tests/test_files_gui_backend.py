#!/usr/bin/env python3
"""Non-destructive smoke test of the real VTXFS GUI backend on tmpfs."""
import os
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import vtxfs_files as v

v.require_ram_volume()
root = f"/__vtxfs_smoke_{os.getpid()}"
v.engine("mkdir", "/", root.lstrip("/"))
try:
    v.engine("replace", root + "/hello.txt", "Xin chao VTXFS!\n")
    assert v.engine("cat", root + "/hello.txt") == b"Xin chao VTXFS!\n"
    v.engine("replace", root + "/hello.txt", "Noi dung da cap nhat")
    assert v.engine("cat", root + "/hello.txt") == b"Noi dung da cap nhat"
    v.engine("rename", root + "/hello.txt", root + "/renamed.txt")
    assert "name=renamed.txt" in v.engine("ls", root).decode()
    v.engine("remove", root + "/renamed.txt")
    assert v.engine("ls", root) == b""
    print("PASS: RAM-backed VTXFS create/write/read/overwrite/rename/delete")
finally:
    # Never remove an existing user's directory or image.
    v.engine("remove", root)
assert "VTXFS-RAM" in v.engine("info").decode()
print("PASS: all operations are scoped to", v.IMAGE)
