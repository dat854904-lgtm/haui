# VTXFS kernel source (Linux fs/vtxfs)

This directory contains the VTXFS Linux filesystem implementation and the
authoritative portable core, on-disk ABI and headers:

- super.c, inode.c, file.c, etc: Linux VFS, writeback and kernel integration
- core/: filesystem engine shared with userspace
- include/: portable internal headers and disk layout definitions
- uapi/: exported userspace ABI header
- kunit/: kernel-specific tests
- Kconfig, Makefile: kernel build configuration

Build as an out-of-tree kernel module, from the VTXFS project root:

    make -C /lib/modules/$(uname -r)/build M="$PWD/fs/vtxfs" modules

Build optional free-space cache test module for disposable QEMU tests:

    make -C /lib/modules/$(uname -r)/build M="$PWD/fs/vtxfs" CONFIG_VTXFS_FREE_CACHE_TEST=m modules

The resulting module is fs/vtxfs/vtxfs.ko. Do not load into a production
kernel without VM/runtime validation. If integrating into a Linux tree,
add the matching fs/vtxfs Kconfig/Makefile entries to fs/Kconfig and
fs/Makefile and assess supported kernel API version and dependencies.

No duplicate core files: the sibling vtxfs-progs directory references
core/include/uapi using relative symlinks.
