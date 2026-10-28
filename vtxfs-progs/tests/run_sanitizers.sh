#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# VTXFS v2.1 — AddressSanitizer (ASan) + UndefinedBehaviorSanitizer (UBSan) Verification
set -euo pipefail
ulimit -c 0 || true
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d -p /dev/shm vtxfs-san-XXXXXXXX)"
trap 'rm -rf -- "$tmp"' EXIT

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

CORE_ALL=(
    "$root/core/vtxfs_csum.c"
    "$root/core/vtxfs_leaf.c"
    "$root/core/vtxfs_leaf_balance.c"
    "$root/core/vtxfs_internal.c"
    "$root/core/vtxfs_internal_balance.c"
    "$root/core/vtxfs_btree_ro.c"
    "$root/core/vtxfs_btree_rw.c"
    "$root/core/vtxfs_vag.c"
    "$root/core/vtxfs_engine.c"
    "$root/core/vtxfs_posix.c"
    "$root/core/vtxfs_fsck.c"
)

SAN_FLAGS=(-std=c17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Wextra -Werror -pedantic -I "$root/include" -I "$root/uapi")

cc="${CC:-gcc}"
echo "=== VTXFS ASan + UBSan Sanitizer Verification ($cc) ==="

"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_validation.c" -o "$tmp/validate-san"
"$tmp/validate-san"

"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_validation_extra.c" -o "$tmp/extra-san"
"$tmp/extra-san"

"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_csum.c" "$root/core/vtxfs_csum.c" "$root/core/vtxfs_leaf.c" -o "$tmp/csum-san"
"$tmp/csum-san"

"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_btree_rw.c" \
    "$root/core/vtxfs_leaf.c" "$root/core/vtxfs_leaf_balance.c" \
    "$root/core/vtxfs_internal.c" "$root/core/vtxfs_internal_balance.c" \
    "$root/core/vtxfs_btree_ro.c" "$root/core/vtxfs_btree_rw.c" -o "$tmp/btree-rw-san"
"$tmp/btree-rw-san"

"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_engine_posix.c" "${CORE_ALL[@]}" -o "$tmp/engine-posix-san"
"$tmp/engine-posix-san"

"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_refcount_snapshot_safety.c" "${CORE_ALL[@]}" -o "$tmp/refcount-safety-san"
"$tmp/refcount-safety-san"

"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_crash_fault.c" "${CORE_ALL[@]}" -o "$tmp/crash-fault-san"
"$tmp/crash-fault-san"

"$cc" "${SAN_FLAGS[@]}" "$root/tools/fsck_vtxfs.c" "${CORE_ALL[@]}" -o "$tmp/fsck.vtxfs-san"
"$cc" "${SAN_FLAGS[@]}" "$root/tests/test_fsck_repair.c" "${CORE_ALL[@]}" -o "$tmp/test-fsck-repair-san"
TMPDIR="$tmp" "$tmp/test-fsck-repair-san" "$tmp/fsck.vtxfs-san"

echo "PASS: ASan + UBSan clean across validation, crypto, B+Tree CoW, POSIX engine, snapshots, crash fault-injection, and fsck repair"
