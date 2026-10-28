# Compile-only module: verifies dual-mode kernel build of all VTXFS v2.1 core modules.
obj-m += vtxfs_probe.o
vtxfs_probe-y := vtxfs_header_probe.o \
                 vtxfs_csum.o \
                 vtxfs_leaf.o \
                 vtxfs_leaf_balance.o \
                 vtxfs_internal.o \
                 vtxfs_internal_balance.o \
                 vtxfs_btree_ro.o \
                 vtxfs_btree_rw.o \
                 vtxfs_vag.o \
                 vtxfs_engine.o \
                 vtxfs_posix.o \
                 vtxfs_fsck.o
ccflags-y += -I$(src)/../../include -I$(src)/../../uapi
