#!/usr/bin/env python3
"""Compile/link and API contract audit for VTXFS-native backref helpers.

Only asserts source, prototype and ELF symbols. No claim of runtime I/O
coverage: KUnit source must be run in an isolated kernel test setup.
"""
import re
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
HEADER=(ROOT/"kernel/vtxfs_backref.h").read_text()
SOURCE=(ROOT/"kernel/backref.c").read_text()
MOD=ROOT/"kernel/vtxfs.ko"
assert MOD.is_file(), "First build vtxfs.ko with kbuild"
nm=subprocess.run(["nm","-a",str(MOD)],capture_output=True,text=True,check=True)
names=set(re.findall(r"(?m)^[0-9A-Fa-f]+\s+[Tt]\s+(vtxfs_\w+)(?:\.\S+)?$",nm.stdout))
exports=set(re.findall(r"\b(vtxfs_\w+)\s*\(",HEADER))
ignore={"vtxfs_backref_visit_t","vtxfs_extent_visit_t"}
missing=sorted(exports-names-ignore)
assert not missing, "Declared public helpers missing in vtxfs.ko: "+repr(missing)
assert len(exports)>35, len(exports)
assert SOURCE.count("VTXFS_TRACE_ENTER();") >= 55

legacy_only=(
    "vtxfs_get_extent_inline_ref", "vtxfs_add_inline_refs",
    "vtxfs_add_keyed_refs", "vtxfs_resolve_indirect_refs",
    "vtxfs_find_parent_nodes", "vtxfs_find_one_extref",
    "vtxfs_find_all_roots_safe", "vtxfs_handle_indirect_tree_backref",
    "vtxfs_backref_alloc_node", "vtxfs_backref_drop_node",
)
for name in legacy_only:
    assert name in names, f"Unsupported API contract missing: {name}"
    body=re.search(r"\b"+name+r"\([^;]*\)\s*\{([^{}]*)\}",SOURCE,re.S)
    assert body and "return -EOPNOTSUPP;" in body.group(1), name

assert "#include \"vtxfs_backref.h\"" in (ROOT/"kernel/vtxfs_subsys.h").read_text()
print(f"PASS: {len(exports)} declared VTXFS backref API names resolved "
      f"in linked module, {len(legacy_only)} unsupported Btrfs-specific "
      f"operations fail explicitly; trace callsites present")
print("NOTE: Compilation/static symbols do not prove kernel execution")
