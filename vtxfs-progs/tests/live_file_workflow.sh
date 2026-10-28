#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Real CLI actions on ONE disposable image. Nothing in pre-existing volumes.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[[ "${1:-}" == /dev/shm/vtxfs-live-* ]] || { echo "REFUSE: unsafe temp root"; exit 3; }
scratch="$1"
[[ -d "$scratch" && ! -L "$scratch" ]] || exit 3
img="/dev/shm/$(basename "$scratch").img"
[[ ! -e "$img" && ! -L "$img" ]] || { echo "REFUSE: existing image"; exit 3; }
trap 'if [[ "$img" == /dev/shm/vtxfs-live-*.img && -f "$img" && ! -L "$img" ]]; then rm -f -- "$img"; fi' EXIT
ctl=("$ROOT/tools/vtxfsctl")
inspect="$ROOT/tools/vtxfs-inspect"
mkfs="$ROOT/tools/mkfs.vtxfs"
fsck="$ROOT/tools/fsck.vtxfs"
run() { printf '\n$'; printf ' %q' "$@"; printf '\n'; "$@"; }
echo '=== Người dùng thật: mkfs → Unicode → file nhị phân → hardlink → readonly → snapshot → fsck ==='
run "$mkfs" -L LIVE-SESSION -b 65536 "$img"
run "${ctl[@]}" "$img" create /Cong-viec --dir
run "${ctl[@]}" "$img" create /Cong-viec/Ghi-chu.txt
printf 'VTXFS - kiem thu Unicode: tiếng Việt, emoji 🧪\nLần ghi thứ nhất\n' > "$scratch/note.txt"
echo '$ vtxfsctl IMAGE edit /Cong-viec/Ghi-chu.txt <note.txt'
"${ctl[@]}" "$img" edit /Cong-viec/Ghi-chu.txt < "$scratch/note.txt"
run "${ctl[@]}" "$img" properties /Cong-viec/Ghi-chu.txt
echo '$ vtxfsctl IMAGE cat /Cong-viec/Ghi-chu.txt'
"${ctl[@]}" "$img" cat /Cong-viec/Ghi-chu.txt
echo
run "${ctl[@]}" "$img" hardlink /Cong-viec/Ghi-chu.txt /Cong-viec/Hardlink.txt
run "$inspect" "$img" stat /Cong-viec/Hardlink.txt
run "${ctl[@]}" "$img" readonly /Cong-viec/Ghi-chu.txt
echo '$ printf BAD | vtxfsctl IMAGE edit /Cong-viec/Hardlink.txt (EXPECTED PERMISSION ERROR)'
if printf BAD | "${ctl[@]}" "$img" edit /Cong-viec/Hardlink.txt; then
    echo 'FAIL: readonly bypass via hardlink'; exit 1
else
    echo 'PASS: readonly edit rejected'
fi
run "${ctl[@]}" "$img" readonly /Cong-viec/Ghi-chu.txt --off
run "${ctl[@]}" "$img" permanently-delete /Cong-viec/Hardlink.txt --yes
echo '$ cmp source note vs reopened original after unlinking hardlink'
"${ctl[@]}" "$img" cat /Cong-viec/Ghi-chu.txt > "$scratch/note-restored.txt"
cmp "$scratch/note.txt" "$scratch/note-restored.txt"
echo 'PASS: original data and hardlink lifecycle'
echo '$ head -c 4194304 /dev/urandom >random.bin'
head -c 4194304 /dev/urandom > "$scratch/random.bin"
echo '$ vtxfs-inspect IMAGE put-stdin /Cong-viec/random.bin <random.bin'
"$inspect" "$img" put-stdin /Cong-viec/random.bin < "$scratch/random.bin"
echo '$ vtxfs-inspect IMAGE cat /Cong-viec/random.bin >readback.bin'
"$inspect" "$img" cat /Cong-viec/random.bin > "$scratch/readback.bin"
echo '$ cmp random.bin readback.bin'
cmp "$scratch/random.bin" "$scratch/readback.bin"
echo '$ sha256sum random.bin readback.bin'
sha256sum "$scratch/random.bin" "$scratch/readback.bin"
echo 'PASS: 4194304 random bytes exactly round-tripped'
run "$inspect" "$img" snap live-snapshot
run "$fsck" -q "$img"
echo 'PASS: real CLI filesystem journey + snapshot + fsck'
