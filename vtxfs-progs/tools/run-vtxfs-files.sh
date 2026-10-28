#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ ! -x "$ROOT/tools/mkfs.vtxfs" ] ||
   [ ! -x "$ROOT/tools/vtxfs-inspect" ] ||
   [ ! -x "$ROOT/tools/fsck.vtxfs" ]; then
    make -C "$ROOT/tools" mkfs.vtxfs vtxfs-inspect fsck.vtxfs
fi
exec python3 "$ROOT/tools/vtxfs_files.py"
