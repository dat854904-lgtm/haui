#!/usr/bin/env python3
"""Audit VTXFS kernel definitions vs module symbols and textual references.

Static references include address-taken callbacks in file_operations. No
source-only inventory can prove a function executed; runtime ftrace can.
"""
import csv
import json
from collections import Counter
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
records = json.loads((ROOT/"docs/KERNEL_FUNCTION_ENTRY_INVENTORY.json").read_text())
makefile=(ROOT/"kernel/Makefile").read_text()
built=set(re.findall(r"\b([A-Za-z_][\w-]*).o\b",
                     makefile.split("obj-$(CONFIG_VTXFS_E2E_TEST)")[0]))
assert built, "failed to parse Makefile"
nm=subprocess.run(["nm","-a",str(ROOT/"kernel/vtxfs.ko")],
                  capture_output=True,text=True,check=True)
syms=set()
for line in nm.stdout.splitlines():
    m=re.match(r"^[0-9a-fA-F]+\s+[tTwW]\s+(\S+)$",line)
    if m:
        syms.add(m.group(1))
# Ignore comments, but keep live C tokens used in static function callbacks.
from instrument_kernel_calls import masked
refs=Counter()
for f in (ROOT/"kernel").glob("*.c"):
    c=masked(f.read_text())
    refs.update(re.findall(r"\b[A-Za-z_]\w*\b",c))
stat=Counter()
output=[]
for rec in records:
    stem=rec["file"].removesuffix(".c")
    is_main = stem.removeprefix("kernel/") in built
    is_optional = (stem=="kernel/selftest_e2e" or
                   stem.startswith("kernel/kunit/"))
    is_header = rec["file"].endswith(".h")
    present = rec["name"] in syms or any(
        v.startswith(rec["name"]+".") for v in syms)
    # Some funcs can be inlined/optimized, and declarations also add refs.
    usage_estimate=max(0,refs[rec["name"]]-1)
    item={**rec,"build":("main-module" if is_main else
                        "optional-tests" if is_optional else
                        "inline-header" if is_header else "other"),
          "symbol_in_vtxfs_ko":present,
          "source_mentions_after_definition":usage_estimate,
          "runtime_executed":"UNVERIFIED"}
    if is_main and usage_estimate == 0 and rec["linkage"]=="static":
        item["review_hint"]="static; no textual call or callback reference seen"
    elif is_main and not present:
        item["review_hint"]="possibly inlined/optimized; not proof of dead code"
    elif is_header:
        item["review_hint"]="inline helper: compiled in callers; standalone symbol optional"
    elif not is_main:
        item["review_hint"]="not linked into the default main module"
    else:
        item["review_hint"]="linked; runtime execution not established"
    stat[item["build"]]+=1
    stat["symbol-present" if present else "not-symbol-present"]+=1
    output.append(item)
out=ROOT/"docs/KERNEL_FUNCTION_BUILD_AUDIT.csv"
with out.open("w",newline="") as fp:
    writer=csv.DictWriter(fp,fieldnames=output[0].keys())
    writer.writeheader()
    writer.writerows(output)
print(f"DEFINITIONS {len(output)}, main-module={stat['main-module']}, "
      f"optional-tests={stat['optional-tests']}, inline-headers="
      f"{stat['inline-header']}, symbols-present="
      f"{stat['symbol-present']}, symbols-not-present={stat['not-symbol-present']}")
print(f"OUTPUT {out}")
print("Unverified: all runtime execution (requires loaded module + observed workload)")
