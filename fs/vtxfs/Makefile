# SPDX-License-Identifier: GPL-2.0-only
#
# VTXFS v2.1 — out-of-tree Linux kernel module (P0.6/P1 VFS + ctree + core engine)
#
#   make -C /lib/modules/$(uname -r)/build M=$PWD/fs/vtxfs modules
#   sudo insmod ./vtxfs.ko
#
# The dual-mode core sources under core/ are compiled with __KERNEL__
# defined by kbuild and automatically use their vzalloc/vfree paths.

obj-m += vtxfs.o

vtxfs-y := \
	super.o \
	inode.o \
	file.o \
	xattr.o \
	locking.o \
	lru_cache.o \
	root-tree.o \
	root.o \
	dir-item.o \
	inode-item.o \
	disk-io.o \
	direct-io.o \
	fiemap.o \
	ioctl.o \
	export.o \
	writeback.o \
	extent_io.o \
	ctree.o \
	extent-tree.o \
	inode-ext.o \
	trans.o \
	ordered-extent.o \
	delayed-inode.o \
	async-thread.o \
	compression.o \
	uuid-tree.o \
	orphan.o \
	print-tree.o \
	reaper.o \
	backref.o \
	sysfs.o \
	free-space-cache.o \
	vag-alloc.o \
	omap.o \
	mount-native.o \
	truncate.o \
	scrub.o \
	selftest.o \
	ctree_balance.o \
	ctree_locking.o \
	ctree_export.o \
	ctree_compat.o \
	ramdisk.o \
	delalloc.o \
	cpd.o \
	extent-item.o \
	sfq.o \
	core/vtxfs_csum.o \
	core/vtxfs_leaf.o \
	core/vtxfs_leaf_balance.o \
	core/vtxfs_internal.o \
	core/vtxfs_internal_balance.o \
	core/vtxfs_btree_ro.o \
	core/vtxfs_btree_rw.o \
	core/vtxfs_vag.o \
	core/vtxfs_engine.o \
	core/vtxfs_posix.o \
	core/vtxfs_compress.o \
	core/vtxfs_crypto.o \
	core/vtxfs_fsck.o

# End-to-end self-test (build with CONFIG_VTXFS_E2E_TEST=m to get vtxfs_e2e.ko)
obj-$(CONFIG_VTXFS_E2E_TEST) += vtxfs_e2e.o
vtxfs_e2e-y := selftest_e2e.o


# M3 real in-kernel selftest, load ONLY in disposable QEMU guest.
obj-$(CONFIG_VTXFS_FREE_CACHE_TEST) += vtxfs_free_cache_test.o
vtxfs_free_cache_test-y := selftest_free_cache.o free-space-cache.o

# KUnit (build with CONFIG_VTXFS_KUNIT_TEST=m to get vtxfs_ctree_test.ko)
obj-$(CONFIG_VTXFS_KUNIT_TEST) += vtxfs_ctree_test.o
vtxfs_ctree_test-y := kunit/vtxfs_ctree_test.o \
	extent_io.o ctree.o extent-tree.o \
	omap.o core/vtxfs_internal.o \
	core/vtxfs_csum.o core/vtxfs_leaf.o



ccflags-y += -I$(src)/include -I$(src)/uapi -I$(src)
