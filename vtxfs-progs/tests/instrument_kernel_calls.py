#!/usr/bin/env python3
"""Inject opt-in rate-limited entry logging into VTXFS kernel C functions.

A deterministic lexer walks file-scope function definitions, not declarations
or initializer expressions. It does not modify call/return semantics.
Use --apply to alter kernel/*.c after creating per-source backups.
Re-run without --apply to audit the annotated function count.
For runtime call graphs use ftrace/function_graph (see docs).
"""
import argparse
import csv
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "kernel"
BACKUP = ROOT / ".vtxfs-backup-20261026" / "kernel-trace-20261026"
MARK = "\n\tVTXFS_TRACE_ENTER();\n"
HEADER = '#include "vtxfs_trace.h"\n'

def masked(src):
    """Same-length C text replacing comments, literals, macros with spaces."""
    x = list(src)
    pos = 0
    n = len(src)
    while pos < n:
        if src[pos] == '"' or src[pos] == "'":
            quote = src[pos]
            start = pos
            pos += 1
            while pos < n:
                if src[pos] == "\\":
                    pos += 2
                elif src[pos] == quote:
                    pos += 1
                    break
                else:
                    pos += 1
            for i in range(start, min(pos, n)):
                if x[i] != "\n":
                    x[i] = " "
            continue
        if src.startswith("/*", pos):
            start = pos
            end = src.find("*/", pos + 2)
            pos = n if end < 0 else end + 2
            for i in range(start, pos):
                if x[i] != "\n":
                    x[i] = " "
            continue
        if src.startswith("//", pos):
            start = pos
            end = src.find("\n", pos)
            pos = n if end < 0 else end
            for i in range(start, pos):
                x[i] = " "
            continue
        pos += 1
    # Preprocessor lines and multiline #define macros are not C bodies.
    at = 0
    pp_continue = False
    for line in src.splitlines(keepends=True):
        is_directive = pp_continue or line.lstrip().startswith("#")
        if is_directive:
            for i in range(at, at + len(line)):
                if x[i] != "\n":
                    x[i] = " "
        pp_continue = is_directive and line.rstrip().endswith("\\")
        at += len(line)
    return "".join(x)

def functions(src):
    clean = masked(src)
    depth = 0
    last_top = 0
    results = []
    for at, ch in enumerate(clean):
        if ch == "{":
            if depth == 0:
                j = at - 1
                while j >= last_top and clean[j].isspace():
                    j -= 1
                if j >= last_top and clean[j] == ")":
                    nesting = 0
                    start_paren = -1
                    for k in range(j, last_top - 1, -1):
                        if clean[k] == ")":
                            nesting += 1
                        elif clean[k] == "(":
                            nesting -= 1
                            if nesting == 0:
                                start_paren = k
                                break
                    if start_paren >= 0:
                        prefix = clean[last_top:start_paren]
                        m = re.search(r"([A-Za-z_]\w*)\s*$", prefix)
                        if m:
                            name = m.group(1)
                            beginning = prefix[:m.start()]
                            if (name not in ("if", "switch", "for", "while",
                                            "sizeof", "typeof", "return")
                                    and "=" not in beginning
                                    and re.search(r"[A-Za-z_]", beginning)):
                                results.append(dict(
                                    name=name,
                                    offset=at,
                                    line=src.count("\n", 0, at) + 1,
                                    linkage=("static" if re.search(
                                        r"\bstatic\b", beginning) else "external"),
                                    annotation=src[at+1:at+1+len(MARK)] == MARK,
                                    signature=" ".join(src[last_top:at].split())[-180:]
                                ))
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                raise ValueError(f"Unbalanced source at {at}")
            if depth == 0:
                last_top = at + 1
        elif ch == ";" and depth == 0:
            last_top = at + 1
    if depth != 0:
        raise ValueError(f"Unbalanced braces ending at depth {depth}")
    return results

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    out = []
    changed = 0
    # Kernel VFS is flat at fs/vtxfs/, but portable core is nested there.
    # Inventory ONLY kernel integration and optional kernel/kunit tests;
    # never treat fs/vtxfs/core (userspace-compatible C) as VFS functions.
    paths = sorted([x for x in KERNEL.glob("*.c")
                    if not x.name.endswith(".mod.c")] +
                   list((KERNEL/"kunit").glob("*.c")) +
                   [KERNEL/"vtxfs_drv.h", KERNEL/"ctree.h"])
    for src_path in paths:
        original = src_path.read_text()
        already = HEADER.strip() in original
        orig_fns = functions(original)
        # Never double-instrument, including if a previous attempt stopped early.
        pending = [f for f in orig_fns if not f["annotation"]]
        if args.apply and pending:
            backup_path = BACKUP / src_path.relative_to(KERNEL)
            backup_path.parent.mkdir(parents=True, exist_ok=True)
            if not backup_path.exists():
                shutil.copy2(src_path, backup_path)
            updated = original
            for f in sorted(pending, key=lambda f: f["offset"], reverse=True):
                updated = updated[:f["offset"]+1] + MARK + updated[f["offset"]+1:]
            if not already:
                include_lines = list(re.finditer(
                    r'^#include[ \t]+[^\n]+\n', updated, re.M))
                if not include_lines:
                    raise ValueError("No #include directives in "+str(src_path))
                updated = (updated[:include_lines[-1].end()] + HEADER +
                           updated[include_lines[-1].end():])
            src_path.write_text(updated)
            changed += 1
        fns = functions(src_path.read_text()) if args.apply else orig_fns
        for f in fns:
            out.append(dict(file=str(src_path.relative_to(ROOT)),
                            name=f["name"], line=f["line"],
                            linkage=f["linkage"], annotated=f["annotation"],
                            signature=f["signature"]))
        print(f"{src_path.name:26s}: functions={len(fns):3d} annotated="
              f"{sum(bool(f['annotation']) for f in fns):3d}", flush=True)
    total = len(out)
    print(f"SUMMARY files={len(paths)} functions={total} "
          f"annotated={sum(x['annotated'] for x in out)} changed_files={changed}")
    doc = ROOT/"docs"
    doc.mkdir(exist_ok=True)
    (doc/"KERNEL_FUNCTION_ENTRY_INVENTORY.json").write_text(
        json.dumps(out, ensure_ascii=False, indent=2) + "\n")
    with (doc/"KERNEL_FUNCTION_ENTRY_INVENTORY.csv").open("w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(out[0]))
        writer.writeheader()
        writer.writerows(out)
    if args.apply and not all(x["annotated"] for x in out):
        raise SystemExit("FAIL: Some discovered functions not instrumented")

if __name__ == "__main__":
    main()
