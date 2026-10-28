# vtxfs-progs

VTXFS userspace utilities and regression tests.

- tools/: mkfs.vtxfs, fsck.vtxfs, vtxfs-inspect, vtxfs CLI and supporting tools
- tests/: portable engine, file-manager, QEMU, crash and fio scripts
- docs/: design, API coverage, implementation and validation reports
- archive/: historical header/source snapshots

The userspace utilities compile from the SAME portable core used by the kernel:
core -> ../fs/vtxfs/core
include -> ../fs/vtxfs/include
uapi -> ../fs/vtxfs/uapi
kernel -> ../fs/vtxfs (read-only path convenience for auditing/testing)

Do not edit the symlinks into separate copies. In this repository layout,
the source files live under fs/vtxfs.

Build from project root:
    make progs

Or from here:
    make
    make test

To run QEMU tests, build the kernel modules first:
    make -C /lib/modules/$(uname -r)/build M="$PWD/../fs/vtxfs" CONFIG_VTXFS_FREE_CACHE_TEST=m modules
    ./tests/run_vm_human.sh
    python3 tests/run_vm_crash.py

All destructive test I/O uses isolated /dev/shm test volumes.
The historical pre-migration README is README.previous-layout.md.
