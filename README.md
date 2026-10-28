# VTXFS project layout

- fs/vtxfs: Linux kernel module, portable engine, include and uapi.
- vtxfs-progs: CLI, mkfs, fsck, test programs, QEMU harness, documentation.

The portable core and on-disk UAPI are authoritative in fs/vtxfs only.
vtxfs-progs has relative symlinks to these sources, not duplicate copies.

Build kernel: make kernel
Build userspace: make progs
Portable tests: make test

Direct kernel build:
make -C /lib/modules/$(uname -r)/build M=$PWD/fs/vtxfs modules

All previous test images, including vtxfs-test.img, remain untouched.
See vtxfs-progs/README.previous-layout.md for historical project overview.
# haui
# haui
# haui
# haui
# haui
# haui
# haui
