#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Guest-only VTXFS runtime smoke, disposable image in /dev/shm.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
KMOD_DIR="$ROOT/../fs/vtxfs"
for cmd in qemu-system-x86_64 cpio gcc mktemp timeout; do
  command -v "$cmd" >/dev/null || { echo "M1 BLOCKED: missing $cmd" >&2; exit 2; }
done
mode="human"
[[ "$mode" == human ]] || { echo "Invalid guest test mode" >&2; exit 2; }
kernel="${VTXFS_VM_KERNEL:-/boot/vmlinuz-linux-zen}"
[[ -f "$kernel" ]] || { echo "M1 BLOCKED: guest kernel image not found" >&2; exit 2; }
[[ -f "$KMOD_DIR/vtxfs.ko" && -f "$KMOD_DIR/vtxfs_free_cache_test.ko" && -x tools/mkfs.vtxfs ]] || {
  echo "M1 BLOCKED: build kernel module and mkfs before VM test" >&2; exit 2;
}
if ! findmnt -n -T /dev/shm -o FSTYPE | grep -qx tmpfs; then
  echo "REFUSING TEST: /dev/shm is not tmpfs" >&2; exit 2
fi
[[ $(df -B1 /dev/shm | awk 'NR==2 {print $4}') -gt 440000000 ]] || {
  echo "M1 BLOCKED: /dev/shm space insufficient" >&2; exit 2
}
dir="$(mktemp -d -p /dev/shm vtxfs-m1-vm.XXXXXXXX)"
trap 'rm -rf -- "$dir"' EXIT
mkdir -p "$dir/root"/{dev,proc,sys,mnt}
gcc -static -pthread -O2 -Wall -Wextra -Werror tests/vm_guest_init.c -o "$dir/root/init"
cp "$KMOD_DIR/vtxfs.ko" "$dir/root/vtxfs.ko"
if [[ -f "$KMOD_DIR/vtxfs_free_cache_test.ko" ]]; then
  cp "$KMOD_DIR/vtxfs_free_cache_test.ko" "$dir/root/vtxfs_free_cache_test.ko"
fi
mkdir -p "$dir/root/deps"
: > "$dir/root/deps/order"
for dependency in $(modinfo -F depends "$KMOD_DIR/vtxfs.ko" | tr ',' ' '); do
  while read -r opcode filename _; do
    [[ "$opcode" == insmod ]] || continue
    name="$(basename "$filename")"
    name="${name%.zst}"
    [[ ! -e "$dir/root/deps/$name" ]] || continue
    if [[ "$filename" == *.zst ]]; then
      zstd -dc "$filename" > "$dir/root/deps/$name"
    else
      cp "$filename" "$dir/root/deps/$name"
    fi
    printf '/deps/%s\n' "$name" >> "$dir/root/deps/order"
  done < <(modprobe --show-depends "$dependency")
done
(
  cd "$dir/root"
  find . -print0 | cpio -0 -o -H newc --quiet > "$dir/initramfs.cpio"
)
image="$dir/isolated-vtxfs.img"
./tools/mkfs.vtxfs -b 65536 "$image" >/dev/null
./tools/fsck.vtxfs -q "$image"
image2="$dir/isolated-vtxfs-second.img"
./tools/mkfs.vtxfs -b 65536 "$image2" >/dev/null
./tools/fsck.vtxfs -q "$image2"
# A genuinely immutable snapshot requested as RW: VFS must expose it RO.
ro_snap_output="$(./tools/vtxfs-inspect "$image2" snap guest-ro)"
ro_snap_id="$(printf '%s\n' "$ro_snap_output" | sed -n 's/.*subvol_id=\([0-9][0-9]*\).*/\1/p')"
[[ "$ro_snap_id" =~ ^[0-9]+$ ]] || { echo "Cannot create RO snapshot on isolated vdb" >&2; exit 5; }
./tools/fsck.vtxfs -q "$image2"
set +e
timeout --signal=TERM --kill-after=5s 85s qemu-system-x86_64 \
  -accel kvm -machine q35 -cpu host -m 1024 -smp 2 \
  -kernel "$kernel" -initrd "$dir/initramfs.cpio" \
  -append "console=ttyS0 rdinit=/init panic=5 loglevel=5 vtx_mode=$mode vtx_ro_id=$ro_snap_id" \
  -drive "file=$image,if=virtio,format=raw,cache=none" \
  -drive "file=$image2,if=virtio,format=raw,cache=none" \
  -display none -monitor none -serial stdio -no-reboot \
  < /dev/null > "$dir/serial.log" 2>&1
qemu_status=$?
set -e
echo "QEMU exit: $qemu_status"
grep -E 'VTXFS_VM_|BUG:|WARNING:|Call Trace:|Kernel panic' "$dir/serial.log" | tail -70 || true
if ! grep -q '^VTXFS_VM_PASS failures=0' "$dir/serial.log"; then
  tail -90 "$dir/serial.log"
  echo "M1 VM RUNTIME FAILED (guest-only; no host kernel touched)" >&2
  exit 1
fi
if grep -Eq 'BUG:|Kernel panic|general protection fault|KASAN:|lockdep.*WARNING' "$dir/serial.log"; then
  echo "M1 VM RUNTIME FAILED (kernel anomaly)" >&2
  exit 1
fi
if [[ "$mode" == stress ]]; then
  # Verify that all concurrent fsync/rename operations and dirty mmap pages
  # survived an actual second VM boot from the same (disposable) image.
  ./tools/fsck.vtxfs -q "$image"
  set +e
  timeout --signal=TERM --kill-after=5s 85s qemu-system-x86_64 \
    -accel kvm -machine q35 -cpu host -m 1024 -smp 2 \
    -kernel "$kernel" -initrd "$dir/initramfs.cpio" \
    -append "console=ttyS0 rdinit=/init panic=5 loglevel=5 vtx_mode=stressverify" \
    -drive "file=$image,if=virtio,format=raw,cache=none" \
    -display none -monitor none -serial stdio -no-reboot \
    < /dev/null > "$dir/reopen.log" 2>&1
  verify_status=$?
  set -e
  grep -E 'VTXFS_VM_|BUG:|Call Trace:|Kernel panic' "$dir/reopen.log" | tail -35 || true
  if [[ $verify_status -ne 0 ]] ||
     ! grep -q 'VTXFS_VM_STRESS_REOPEN_OK files=32 mmap_dirty_persisted=1' "$dir/reopen.log" ||
     ! grep -q 'VTXFS_VM_PASS failures=0' "$dir/reopen.log" ||
     grep -Eq 'BUG:|Kernel panic|KASAN:|general protection fault' "$dir/reopen.log"; then
    echo "M5/M6 persistent mmap/rename verification failed" >&2
    tail -80 "$dir/reopen.log"
    exit 1
  fi
  echo "M5/M6 PASS: all 32 concurrent files and mmap dirty page persist after remount."
fi
# Cold read-only reboot of the same test volume: validate hardlink, orphan,
# overwritten rename and physically allocated zeros survived real unmount.
set +e
timeout --signal=TERM --kill-after=5s 85s qemu-system-x86_64 \
  -accel kvm -machine q35 -cpu host -m 1024 -smp 2 \
  -kernel "$kernel" -initrd "$dir/initramfs.cpio" \
  -append "console=ttyS0 rdinit=/init panic=5 loglevel=5 vtx_mode=humanverify" \
  -drive "file=$image,if=virtio,format=raw,cache=none" \
  -display none -monitor none -serial stdio -no-reboot \
  < /dev/null > "$dir/human-reopen.log" 2>&1
reboot_status=$?
set -e
grep -E 'VTXFS_VM_|Call Trace:|BUG:' "$dir/human-reopen.log" | tail -20 || true
if [[ $reboot_status -ne 0 ]] ||
   ! grep -q 'VTXFS_VM_HUMAN_REOPEN_OK hardlink=1 rename=1 prealloc=1 orphan=1' "$dir/human-reopen.log" ||
   ! grep -q 'VTXFS_VM_PASS failures=0' "$dir/human-reopen.log"; then
  echo "Human remount integrity FAILED" >&2
  tail -70 "$dir/human-reopen.log"
  exit 4
fi
echo "HUMAN COLD REMOUNT PASS: hardlink, rename, preallocation, orphan persisted."
cp "$dir/serial.log" "$ROOT/docs/VTXFS_HUMAN_VM_SERIAL_2026-10-26.log"
./tools/fsck.vtxfs -q "$image"
./tools/fsck.vtxfs -q "$image2"
if ! grep -q "VTXFS_VM_HUMAN_FINISHED" "$dir/serial.log"; then
  echo "HUMAN VM INCOMPLETE: summary missing" >&2
  exit 1
fi
echo "VTXFS HUMAN AUDIT COMPLETE (check DEFECT/MISSING above; guest harness success is not feature success)."
if ! grep -q "VTXFS_VM_HUMAN_FINISHED defects=0 " "$dir/serial.log"; then
  echo "VTXFS HUMAN AUDIT FOUND DEFECTS; NOT A PASS" >&2
  exit 3
fi
