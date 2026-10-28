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
mode="${VTXFS_VM_MODE:-smoke}"
[[ "$mode" == smoke || "$mode" == stress ]] || { echo "Invalid guest test mode" >&2; exit 2; }
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
set +e
timeout --signal=TERM --kill-after=5s 85s qemu-system-x86_64 \
  -accel kvm -machine q35 -cpu host -m 1024 -smp 2 \
  -kernel "$kernel" -initrd "$dir/initramfs.cpio" \
  -append "console=ttyS0 rdinit=/init panic=5 loglevel=5 vtx_mode=$mode" \
  -drive "file=$image,if=virtio,format=raw,cache=none" \
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
./tools/fsck.vtxfs -q "$image"
echo "VTXFS VM PASS mode=$mode: mount + fsync + FIEMAP/threads + umount + fsck."
