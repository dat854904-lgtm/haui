#!/usr/bin/env python3
"""Read-only bitmap export + actual allocation integration, on /dev/shm only."""
import os
import sys
import subprocess
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from vtxfs_files import IMAGE, TOOLS, require_ram_volume, engine
from vtxfs_bitmap import parse_bitmap
require_ram_volume()
initial = parse_bitmap(engine("bitmap", "0"))
assert initial.vag_blocks == 32768
assert initial.allocated_count + initial.free_blocks == 32768
assert initial.start_pblk == 32768
page = engine("bitmap", "0", "32750", "18").decode("ascii")
assert "start_offset=32750" in page
assert "count=18" in page
assert "bitmap_hex=" in page
for command in [
    ("bitmap", "1"), ("bitmap", "-1"), ("bitmap", "0", "32768"),
    ("bitmap", "0", "32760", "20"), ("bitmap", "0", "0", "0"),
]:
    result = subprocess.run([str(TOOLS / "vtxfs-inspect"), str(IMAGE), *command],
                            capture_output=True)
    assert result.returncode != 0, f"invalid parameters unexpectedly accepted: {command}"
print("PASS: bitmap page/export/count/bounds validation")

test_dir = f"/bitmap-sanity-{os.getpid()}"
engine("mkdir", "/", test_dir[1:])
try:
    before = parse_bitmap(engine("bitmap", "0"))
    payload = "E"*16384
    engine("replace", test_dir + "/allocated.txt", payload)
    after = parse_bitmap(engine("bitmap", "0"))
    changed = sum((a ^ b).bit_count() for a, b in zip(before.bits, after.bits))
    assert changed > 0, "bitmap did not react to filesystem allocation"
    assert after.allocated_count == after.allocated
    assert engine("cat", test_dir + "/allocated.txt") == payload.encode()
    print(f"PASS: writing real file changed {changed} VAG allocation bits; "
          f"allocated {before.allocated} -> {after.allocated}")
finally:
    engine("remove", test_dir + "/allocated.txt")
    engine("remove", test_dir)
check = subprocess.run([str(TOOLS / "fsck.vtxfs"), "-q", str(IMAGE)],
                       capture_output=True, text=True)
assert check.returncode == 0, check.stderr or check.stdout
print("PASS: image fsck after bitmap tests")
