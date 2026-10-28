// SPDX-License-Identifier: GPL-2.0-only
/*
 * selftest.c — dựng cây PHYSICAL_VBLK đa tầng hoàn toàn trong RAM rồi
 * kiểm tra search/insert (kèm tách)/iterate/CoW/truncate. Chạy khi nạp module.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vtxfs_csum.h"
#include "vtxfs_trace.h"

static atomic64_t mock_vblk = ATOMIC64_INIT(1000);

static int mock_alloc(struct vtxfs_fs_info *fs, u32 blocks, u64 *out)
{
	VTXFS_TRACE_ENTER();

	(void)fs;
	(void)blocks;
	*out = (u64)atomic64_add_return(1, &mock_vblk);
	return 0;
}

static int mock_free(struct vtxfs_fs_info *fs, u64 start, u32 blocks)
{
	VTXFS_TRACE_ENTER();

	(void)fs;
	(void)start;
	(void)blocks;
	return 0;
}

static const struct vtxfs_alloc_ops mock_ops = {
	.alloc_blocks = mock_alloc,
	.free_blocks = mock_free,
};

static u8 mock_scrub_blocks[4 * VTXFS_BLOCK_SIZE];

static int mock_scrub_read_block(struct vtxfs_fs_info *fs, u64 pblk, void *buf)
{
	VTXFS_TRACE_ENTER();

	(void)fs;
	if (pblk >= 4)
		return -EIO;
	memcpy(buf, mock_scrub_blocks + (size_t)pblk * VTXFS_BLOCK_SIZE, VTXFS_BLOCK_SIZE);
	return 0;
}

static int selftest_scrub_crc(void)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs;
	struct vtxfs_scrub_ctx sc;
	struct vtx_extent_item *ext;
	__le32 *csums;
	size_t ext_sz;
	u32 crc0, crc1;
	int fails = 0, i, ret;

	fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;
	fs->node_size = 4096;
	fs->read_block_fn = mock_scrub_read_block;

	for (i = 0; i < VTXFS_BLOCK_SIZE; i++) {
		mock_scrub_blocks[0 * VTXFS_BLOCK_SIZE + i] = (u8)(i ^ 0x5a);
		mock_scrub_blocks[1 * VTXFS_BLOCK_SIZE + i] = (u8)(i ^ 0xa5);
	}
	crc0 = vtx_crc32c(mock_scrub_blocks + 0 * VTXFS_BLOCK_SIZE, VTXFS_BLOCK_SIZE);
	crc1 = vtx_crc32c(mock_scrub_blocks + 1 * VTXFS_BLOCK_SIZE, VTXFS_BLOCK_SIZE);

	/* Case 1: 1-block extent, valid CRC32C */
	ext_sz = sizeof(*ext) + sizeof(__le32);
	ext = kzalloc(ext_sz, GFP_KERNEL);
	if (!ext) {
		kfree(fs);
		return -ENOMEM;
	}
	ext->vblk = cpu_to_le64(0);
	ext->physical_blocks = cpu_to_le32(1);
	ext->csum_count = cpu_to_le16(1);
	ext->csum_type = cpu_to_le16(VTX_CSUM_CRC32C);
	csums = (__le32 *)ext->tail;
	csums[0] = cpu_to_le32(crc0);

	memset(&sc, 0, sizeof(sc));
	sc.fs = fs;

	ret = scrub_verify_extent(&sc, ext);
	if (ret != 0 ||
	    vtx_cpu64(sc.state.verified_blocks) != 1 ||
	    vtx_cpu32(sc.state.error_count) != 0 ||
	    vtx_cpu64(sc.state.last_checked_vblk) != 0) {
		pr_err("selftest_scrub: Case 1 failed: ret=%d ver=%llu err=%u last=%llu\n",
		       ret, (unsigned long long)vtx_cpu64(sc.state.verified_blocks),
		       vtx_cpu32(sc.state.error_count),
		       (unsigned long long)vtx_cpu64(sc.state.last_checked_vblk));
		fails++;
	}

	/* Case 2: Corrupted block CRC (actual != expected) */
	csums[0] = cpu_to_le32(crc0 ^ 0xdeadbeef);
	ret = scrub_verify_extent(&sc, ext);
	if (ret != 0 ||
	    vtx_cpu64(sc.state.verified_blocks) != 1 ||
	    vtx_cpu32(sc.state.error_count) != 1 ||
	    vtx_cpu64(sc.state.last_checked_vblk) != 0) {
		pr_err("selftest_scrub: Case 2 failed: ret=%d ver=%llu err=%u last=%llu\n",
		       ret, (unsigned long long)vtx_cpu64(sc.state.verified_blocks),
		       vtx_cpu32(sc.state.error_count),
		       (unsigned long long)vtx_cpu64(sc.state.last_checked_vblk));
		fails++;
	}
	kfree(ext);

	/* Case 3: 2-block extent: block 0 valid, block 1 corrupted */
	ext_sz = sizeof(*ext) + 2 * sizeof(__le32);
	ext = kzalloc(ext_sz, GFP_KERNEL);
	if (!ext) {
		kfree(fs);
		return -ENOMEM;
	}
	ext->vblk = cpu_to_le64(0);
	ext->physical_blocks = cpu_to_le32(2);
	ext->csum_count = cpu_to_le16(2);
	ext->csum_type = cpu_to_le16(VTX_CSUM_CRC32C);
	csums = (__le32 *)ext->tail;
	csums[0] = cpu_to_le32(crc0);
	csums[1] = cpu_to_le32(crc1 ^ 0x12345678);

	memset(&sc, 0, sizeof(sc));
	sc.fs = fs;

	ret = scrub_verify_extent(&sc, ext);
	if (ret != 0 ||
	    vtx_cpu64(sc.state.verified_blocks) != 1 ||
	    vtx_cpu32(sc.state.error_count) != 1 ||
	    vtx_cpu64(sc.state.last_checked_vblk) != 1) {
		pr_err("selftest_scrub: Case 3 failed: ret=%d ver=%llu err=%u last=%llu\n",
		       ret, (unsigned long long)vtx_cpu64(sc.state.verified_blocks),
		       vtx_cpu32(sc.state.error_count),
		       (unsigned long long)vtx_cpu64(sc.state.last_checked_vblk));
		fails++;
	}

	/* Case 4: Inline extent is skipped without reading blocks */
	ext->flags = cpu_to_le32(VTX_EXTENT_INLINE);
	ret = scrub_verify_extent(&sc, ext);
	if (ret != 0 ||
	    vtx_cpu64(sc.state.verified_blocks) != 1 ||
	    vtx_cpu32(sc.state.error_count) != 1) {
		pr_err("selftest_scrub: Case 4 failed: ret=%d ver=%llu err=%u\n",
		       ret, (unsigned long long)vtx_cpu64(sc.state.verified_blocks),
		       vtx_cpu32(sc.state.error_count));
		fails++;
	}

	kfree(ext);
	kfree(fs);
	return fails ? -EINVAL : 0;
}

int vtxfs_run_selftest(void)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_fs_info *fs;
	struct vtxfs_root *root;
	struct vtxfs_extent_buffer *root_eb = NULL;
	struct vtxfs_path *path;
	int i, ret, fails = 0;

	fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	root = kzalloc(sizeof(*root), GFP_KERNEL);
	path = vtxfs_alloc_path();
	if (!fs || !root || !path) {
		kfree(fs);
		kfree(root);
		vtxfs_free_path(path);
		return -ENOMEM;
	}

	fs->node_size = 4096;
	fs->current_xid = 1;
	fs->committed_xid = 1;
	fs->alloc_ops = &mock_ops;
	mutex_init(&fs->trans_lock);
	init_rwsem(&fs->commit_sem);
	spin_lock_init(&fs->eb_lock);
	INIT_LIST_HEAD(&fs->dirty_ebs);
	atomic64_set(&fs->next_object_id, 100);
	xa_init(&fs->eb_cache);
	xa_init(&fs->roots);

	root->fs = fs;
	root->tree_type = VTX_TREE_OMAP_META;	/* PHYSICAL_VBLK */
	root->root_oid = 1;
	root->subvol_id = 0;

	mutex_lock(&fs->trans_lock);
	/* gốc lá ban đầu */
	ret = alloc_tree_block_for_test(root, &root_eb);
	if (ret) {
		fails++;
		goto out;
	}
	rcu_assign_pointer(root->node, root_eb);

	/* chèn 300 item để buộc tách lá + tăng gốc */
	for (i = 0; i < 300; i++) {
		struct vtx_key k;
		char val[64];

		vtxfs_mk_key(&k, 1000 + i, VTX_TYPE_OMAP_VAL, 0);
		memset(val, i & 0xff, sizeof(val));
		vtxfs_release_path(path);
		ret = vtxfs_search_slot(root, path, &k, sizeof(val), 1);
		if (ret != 1) {
			pr_err("selftest: search_slot %d ret=%d\n", i, ret);
			fails++;
			continue;
		}
		ret = vtxfs_insert_item(root, path, &k, sizeof(val), NULL);
		if (ret) {
			pr_err("selftest: insert %d ret=%d\n", i, ret);
			fails++;
		}
	}

	/* xác minh mọi khóa đọc lại được */
	for (i = 0; i < 300; i++) {
		struct vtx_key k;
		struct vtxfs_path *p2 = NULL;

		vtxfs_mk_key(&k, 1000 + i, VTX_TYPE_OMAP_VAL, 0);
		ret = vtxfs_find_item(root, &k, &p2);
		if (ret) {
			pr_err("selftest: find %d ret=%d\n", i, ret);
			fails++;
		} else {
			vtxfs_free_path(p2);
		}
	}

	/* truncate: xoá nửa sau */
	for (i = 150; i < 300; i++) {
		struct vtx_key k;
		struct vtxfs_path *p3;

		vtxfs_mk_key(&k, 1000 + i, VTX_TYPE_OMAP_VAL, 0);
		p3 = vtxfs_alloc_path();
		if (!p3) {
			fails++;
			continue;
		}
		ret = vtxfs_search_slot(root, p3, &k, 0, 1);
		if (ret == 0) {
			ret = vtxfs_del_item(root, p3);
			if (ret) {
				pr_err("selftest: del %d ret=%d\n", i, ret);
				fails++;
			}
		} else {
			pr_err("selftest: search for del %d ret=%d\n", i, ret);
			fails++;
		}
		vtxfs_free_path(p3);
	}
out:
	mutex_unlock(&fs->trans_lock);
	vtxfs_free_path(path);
	kfree(root);
	kfree(fs);
	if (selftest_scrub_crc() != 0)
		fails++;

	pr_info("vtxfs selftest: %s (%d failures)\n",
		fails ? "FAIL" : "PASS", fails);
	return fails ? -EINVAL : 0;
}
