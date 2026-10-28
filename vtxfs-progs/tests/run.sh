#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
ulimit -c 0 || true
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT

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
    "$root/core/vtxfs_stream.c"
    "$root/core/vtxfs_compress.c"
)

VTXFS_CLI_ALL=(
    "$root/tools/vtxfs/main.c"
    "$root/tools/vtxfs/common.c"
    "$root/tools/vtxfs"/cmds-*.c
)

for cc in gcc clang; do
    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "SKIP $cc (not installed)"
        continue
    fi
    "$cc" -std=c11 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_abi.c" -o "$tmp/test-$cc"
    "$tmp/test-$cc"
    "$cc" -std=c11 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_validation.c" -o "$tmp/validate-$cc"
    "$tmp/validate-$cc"
    "$cc" -std=c11 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_validation_extra.c" -o "$tmp/extra-$cc"
    "$tmp/extra-$cc"
    # Btrfs/APFS-inspired portable FIEMAP range and SFQ boundary guards.
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_space_and_fiemap_ranges.c" -o "$tmp/ranges-$cc"
    "$tmp/ranges-$cc"
    # VAG word-wise allocator + free-run summary index differential verification.
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tests/test_vag_index.c" "$root/core/vtxfs_csum.c" \
        "$root/core/vtxfs_vag.c" -lzstd -llz4 -lz \
        -o "$tmp/vag-index-$cc"
    "$tmp/vag-index-$cc"
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_csum.c" "$root/core/vtxfs_csum.c" \
        "$root/core/vtxfs_leaf.c" -o "$tmp/csum-$cc"
    "$tmp/csum-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tests/test_p2_data.c" "$root/core/vtxfs_compress.c" \
        "$root/core/vtxfs_crypto.c" "$root/core/vtxfs_csum.c" \
        "$root/core/vtxfs_leaf.c" -lzstd -llz4 -lz -lcrypto \
        -o "$tmp/p2-data-$cc"
    "$tmp/p2-data-$cc"
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_leaf.c" "$root/core/vtxfs_leaf.c" -o "$tmp/leaf-$cc"
    "$tmp/leaf-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_wire_endian.c" "$root/core/vtxfs_leaf.c" -o "$tmp/wire-$cc"
    "$tmp/wire-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_internal.c" "$root/core/vtxfs_internal.c" -o "$tmp/internal-$cc"
    "$tmp/internal-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_leaf_balance.c" "$root/core/vtxfs_leaf.c" \
        "$root/core/vtxfs_leaf_balance.c" -o "$tmp/balance-$cc"
    "$tmp/balance-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_internal_balance.c" "$root/core/vtxfs_internal.c" \
        "$root/core/vtxfs_internal_balance.c" -o "$tmp/ibalance-$cc"
    "$tmp/ibalance-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_btree_ro.c" "$root/core/vtxfs_leaf.c" \
        "$root/core/vtxfs_internal.c" "$root/core/vtxfs_btree_ro.c" \
        -o "$tmp/btree-ro-$cc"
    "$tmp/btree-ro-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic -I "$root/include" \
        "$root/tests/test_btree_rw.c" "$root/core/vtxfs_leaf.c" \
        "$root/core/vtxfs_leaf_balance.c" "$root/core/vtxfs_internal.c" \
        "$root/core/vtxfs_internal_balance.c" "$root/core/vtxfs_btree_ro.c" \
        "$root/core/vtxfs_btree_rw.c" -o "$tmp/btree-rw-$cc"
    "$tmp/btree-rw-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tests/test_engine_posix.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz \
        -o "$tmp/engine-posix-$cc"
    "$tmp/engine-posix-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tests/test_refcount_snapshot_safety.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz \
        -o "$tmp/refcount-safety-$cc"
    "$tmp/refcount-safety-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tests/test_fsck_dynamic_subvols.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz \
        -o "$tmp/fsck-many-sv-$cc"
    "$tmp/fsck-many-sv-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tests/test_crash_fault.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz \
        -o "$tmp/crash-fault-$cc"
    "$tmp/crash-fault-$cc"

    # Build and test userspace CLI utilities
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/mkfs_vtxfs.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/mkfs.vtxfs-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/fsck_vtxfs.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/fsck.vtxfs-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tests/test_fsck_repair.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz \
        -o "$tmp/test-fsck-repair-$cc"
    "$tmp/test-fsck-repair-$cc" "$tmp/fsck.vtxfs-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/vtxfs_inspect.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/vtxfs-inspect-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/vtxfs_ioctl_test.c" -o "$tmp/vtxfs-ioctl-test-$cc"

    # Build vtxfs (unified CLI)
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "${VTXFS_CLI_ALL[@]}" \
        "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/vtxfs-$cc"

    # Build vtxfs-image (metadump / image create+restore)
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/vtxfs-image/main.c" \
        "$root/tools/vtxfs-image/common.c" \
        "$root/tools/vtxfs-image/sanitize.c" \
        "$root/tools/vtxfs-image/image-create.c" \
        "$root/tools/vtxfs-image/image-restore.c" \
        "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/vtxfs-image-$cc"

    # Build standalone tools and scaffolds
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/mount.vtxfs.c" -o "$tmp/mount.vtxfs-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/vtxfs-balance.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/vtxfs-balance-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/vtxfs-defrag.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/vtxfs-defrag-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/vtxfs-send.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/vtxfs-send-$cc"
    "$cc" -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$root/include" -I "$root/uapi" \
        "$root/tools/vtxfs-receive.c" "${CORE_ALL[@]}" -lzstd -llz4 -lz -o "$tmp/vtxfs-receive-$cc"

    img="$tmp/vol-$cc.img"
    "$tmp/mkfs.vtxfs-$cc" -L "vol-$cc" -b 65536 "$img" >/dev/null
    "$tmp/fsck.vtxfs-$cc" -q "$img"
    "$tmp/vtxfs-inspect-$cc" "$img" mkdir / etc >/dev/null
    "$tmp/vtxfs-inspect-$cc" "$img" write /etc motd "VTXFS v2.1 OK" >/dev/null
    "$tmp/vtxfs-inspect-$cc" "$img" snap snap1 >/dev/null
    got="$("$tmp/vtxfs-inspect-$cc" "$img" cat /etc/motd)"
    test "$got" = "VTXFS v2.1 OK"

    # Test vtxfs CLI (info, subvolume create/list with name, snapshot create/list)
    "$tmp/vtxfs-$cc" filesystem info "$img" >/dev/null
    "$tmp/vtxfs-$cc" filesystem sync "$img" >/dev/null
    "$tmp/vtxfs-$cc" subvolume create "$img" subvol_test >/dev/null
    sub_list="$("$tmp/vtxfs-$cc" subvolume list "$img")"
    echo "$sub_list" | grep -q "subvol_test"
    "$tmp/vtxfs-$cc" snapshot create "$img" 256 >/dev/null
    snap_list="$("$tmp/vtxfs-$cc" snapshot list "$img")"
    echo "$snap_list" | grep -q "256"
    "$tmp/vtxfs-balance-$cc" "$img" >/dev/null
    "$tmp/vtxfs-defrag-$cc" "$img" >/dev/null

    "$tmp/fsck.vtxfs-$cc" -q "$img"
    rm -f "$img"

    # Round-trip: create -> write inline + multi-block external extent data -> dump(--full) -> restore -> fsck + scrub + content check
    rt="$tmp/rt-$cc.img"
    "$tmp/mkfs.vtxfs-$cc" -L "rt-$cc" -b 65536 "$rt" >/dev/null
    "$tmp/vtxfs-inspect-$cc" "$rt" mkdir / data >/dev/null
    "$tmp/vtxfs-inspect-$cc" "$rt" write /data file.bin \
          "VTXFS round-trip payload 0123456789" >/dev/null
    python3 -c 'import sys; sys.stdout.buffer.write(b"VTXFS-EXTENT-BLOCK-" * 600)' > "$tmp/rt-large-$cc.bin"
    "$tmp/vtxfs-inspect-$cc" "$rt" put-stdin /data/large.bin < "$tmp/rt-large-$cc.bin" >/dev/null
    "$tmp/vtxfs-image-$cc" create "$rt" -o "$tmp/rt-$cc.dump" --full >/dev/null
    "$tmp/vtxfs-image-$cc" restore "$tmp/rt-$cc.dump" "$tmp/rt-$cc-restored.img" >/dev/null
    "$tmp/fsck.vtxfs-$cc" -q "$tmp/rt-$cc-restored.img"
    "$tmp/vtxfs-$cc" scrub start "$tmp/rt-$cc-restored.img" --verify-only >/dev/null
    "$tmp/vtxfs-inspect-$cc" "$tmp/rt-$cc-restored.img" cat /data/large.bin > "$tmp/rt-got1-$cc.bin"
    cmp -s "$tmp/rt-large-$cc.bin" "$tmp/rt-got1-$cc.bin"
    # Round-trip với nén LZSS
    "$tmp/vtxfs-image-$cc" create "$rt" -o "$tmp/rt-$cc.dump.lz" --full --compress >/dev/null
    "$tmp/vtxfs-image-$cc" restore "$tmp/rt-$cc.dump.lz" "$tmp/rt-$cc-restored-lz.img" >/dev/null
    "$tmp/fsck.vtxfs-$cc" -q "$tmp/rt-$cc-restored-lz.img"
    "$tmp/vtxfs-$cc" scrub start "$tmp/rt-$cc-restored-lz.img" --verify-only >/dev/null
    "$tmp/vtxfs-inspect-$cc" "$tmp/rt-$cc-restored-lz.img" cat /data/large.bin > "$tmp/rt-got2-$cc.bin"
    cmp -s "$tmp/rt-large-$cc.bin" "$tmp/rt-got2-$cc.bin"
    rm -f "$rt" "$tmp/rt-$cc.dump" "$tmp/rt-$cc.dump.lz" \
          "$tmp/rt-$cc-restored.img" "$tmp/rt-$cc-restored-lz.img" \
          "$tmp/rt-large-$cc.bin" "$tmp/rt-got1-$cc.bin" "$tmp/rt-got2-$cc.bin"

    echo "PASS $cc C11/C17 (ABI + Crypto + B+Tree RW + Engine + POSIX + Snapshots + Reaper + SFQ + Crash/Fault + CLI + Image round-trip)"
done

for cc in g++ clang++; do
    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "SKIP $cc (not installed)"
        continue
    fi
    "$cc" -std=c++17 -Wall -Wextra -Werror -pedantic -I "$root/uapi" \
        "$root/tests/test_uapi_cpp.cpp" -o "$tmp/test-$cc"
    "$tmp/test-$cc"
    echo "PASS $cc C++17 UAPI"
done

echo "ALL VTXFS v2.1 P0 TESTS PASSED"
