#!/usr/bin/env python3
"""Turn a captured VTXFS dmesg/ftrace transcript into observed-function coverage.

Important: 'not observed' NEVER means unused or independent/dead code. It
only means the supplied workload never produced a matching trace entry.
"""
import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument("log", type=Path,
               help="Text saved from trace-vtxfs.sh follow or ftrace-read")
p.add_argument("--csv", type=Path, help="optional output CSV path")
args=p.parse_args()
raw=args.log.read_text(errors="replace")
records=json.loads((ROOT/"docs/KERNEL_FUNCTION_ENTRY_INVENTORY.json").read_text())
names={row["name"] for row in records}
# Dynamic debug entries are unequivocal. Ftrace flat or graph lines from
# the explicitly module-filtered vtxfs_audit instance are accepted as well.
observed=set(re.findall(r"vtxfs-call enter\s+([A-Za-z_]\w*)\(\)",raw))
observed |= set(re.findall(r"vtxfs-event\s+([A-Za-z_]\w*)\(\)",raw))
for line in raw.splitlines():
    if "|" not in line or "vtxfs-call" in line:
        continue
    m = re.search(r"\|\s*(?:\d+\.\d+ us\s*\|\s*)?([A-Za-z_]\w+)\(\)",line)
    if m and m.group(1) in names:
        observed.add(m.group(1))
    m=re.search(r"\|\s*([A-Za-z_]\w+)\s+<-",line)
    if m and m.group(1) in names:
        observed.add(m.group(1))
counts=Counter()
rows=[]
for record in records:
    hit=record["name"] in observed
    result={**record, "observed_in_supplied_trace":hit,
            "runtime_state":("SEEN" if hit else "NOT_SEEN_IN_THIS_TRACE")}
    rows.append(result)
    counts["seen" if hit else "not-seen"]+=1
    counts["file:"+record["file"]]+=int(hit)
if args.csv:
    with args.csv.open("w",newline="") as fp:
        w=csv.DictWriter(fp,fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)
print(f"Observed {counts['seen']}/{len(rows)} function definitions "
      "(different files can share a name)")
for file in sorted({r["file"] for r in rows}):
    selected=[r for r in rows if r["file"]==file]
    print(f"{file:43s}  seen {sum(r['observed_in_supplied_trace'] for r in selected):3d} "
          f"/ total {len(selected):3d}")
print("CAUTION: NOT_SEEN_IN_THIS_TRACE does not establish unused/dead code.")
