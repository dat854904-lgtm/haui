// SPDX-License-Identifier: GPL-2.0-only
/* M3 kernel-only free-run cache test, load ONLY in disposable QEMU guest.
 * Memory-only; never inspects/reclaims disk blocks. Return error on failure.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include "vtxfs_subsys.h"

#define EXPECT(expr) do { if (!(expr)) { \
	pr_err("VTXFS_M3_CACHE_FAIL line=%d: %s\n", __LINE__, #expr); \
	ret = -EUCLEAN; goto out; \
} } while (0)

static int __init vtxfs_free_cache_test_init(void)
{
	struct vtxfs_fs_info *fs;
	u64 found = 0;
	int ret = 0;

	fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;
	fs->free_space_tree = RB_ROOT;
	spin_lock_init(&fs->free_space_lock);
	fs->committed_xid = 7;
	EXPECT(vtxfs_free_space_add(fs, 1, 8) == 0);
	EXPECT(vtxfs_free_space_add(fs, 9, 3) == 0);
	EXPECT(vtxfs_free_space_find(fs, 11, &found) == 0 && found == 1);
	EXPECT(vtxfs_free_space_add(fs, 3, 1) == -EEXIST);
	EXPECT(vtxfs_free_space_remove(fs, 4, 3) == 0);
	EXPECT(vtxfs_free_space_find(fs, 5, &found) == 0 && found == 7);
	EXPECT(vtxfs_free_space_remove(fs, 7, 5) == 0);
	EXPECT(vtxfs_free_space_find(fs, 5, &found) == -ENOSPC);
	EXPECT(vtxfs_free_space_add(fs, VTXFS_MAKE_VBLK(0, 32767), 2) == -EINVAL);
	EXPECT(vtxfs_free_space_add(fs, 100, 4) == 0);
	fs->committed_xid = 8;
	EXPECT(vtxfs_free_space_find(fs, 4, &found) == -ENOSPC);
	EXPECT(vtxfs_free_space_add(fs, 100, 5) == 0);
	EXPECT(vtxfs_free_space_find(fs, 5, &found) == 0 && found == 100);
	EXPECT(vtxfs_free_space_load(fs) == -EOPNOTSUPP);
	EXPECT(vtxfs_free_space_find(fs, 1, &found) == -ENOSPC);
	pr_info("VTXFS_M3_CACHE_TEST_PASS\n");
out:
	vtxfs_free_space_invalidate(fs);
	kfree(fs);
	return ret;
}

static void __exit vtxfs_free_cache_test_exit(void) {}

module_init(vtxfs_free_cache_test_init);
module_exit(vtxfs_free_cache_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VTXFS disposable guest M3 cache split/merge test");
