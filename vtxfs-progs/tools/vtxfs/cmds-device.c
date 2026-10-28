// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * cmds-device.c — Quản lý cấu hình đa thiết bị (Multi-device JBOD/Linear Pool) VTXFS.
 * Hỗ trợ info, stats, add (thêm thiết bị mở rộng dung lượng), remove và replace.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "commands.h"
#include "common.h"
#include "vtxfs_csum.h"

static const char *dev_state_str(__u32 f)
{
	static char buf[128];

	buf[0] = '\0';
	if (f & VTX_DEV_STATE_ONLINE)
		strcat(buf, "ONLINE ");
	if (f & VTX_DEV_STATE_EVACUATING)
		strcat(buf, "EVACUATING ");
	if (f & VTX_DEV_STATE_REPLACING)
		strcat(buf, "REPLACING ");
	if (f & VTX_DEV_STATE_READONLY)
		strcat(buf, "READONLY ");
	if (f & VTX_DEV_STATE_MISSING)
		strcat(buf, "MISSING ");
	if (buf[0])
		buf[strlen(buf) - 1] = '\0';
	else
		strcpy(buf, "UNKNOWN");
	return buf;
}

static const char *tier_name(__u16 tier)
{
	switch (tier) {
	case VTX_TIER_NVME_HOT: return "NVME_HOT";
	case VTX_TIER_SSD_WARM: return "SSD_WARM";
	case VTX_TIER_HDD_COLD: return "HDD_COLD";
	default: return "STANDARD";
	}
}

static int cmd_device_info(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u32 count, i;
	__u64 pool_total = 0, pool_used = 0;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs device info <image|device>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;

	count = vtx_cpu32(h.fs->dev_tbl.hdr.entry_count);
	if (count == 0)
		count = 1;

	printf("==================================================================================\n");
	printf("  VTXFS — THÔNG TIN NHÓM THIẾT BỊ LƯU TRỮ (STORAGE POOL MEMBERS)\n");
	printf("  Hệ thống tệp: %s | UUID: ", argv[0]);
	vtxfs_cli_print_uuid(h.fs->super_blk.hdr.fsid_uuid);
	printf("\n  Tổng số thiết bị thành viên: %u (chế độ: %s)\n",
	       count,
	       (vtx_cpu64(h.fs->super_blk.hdr.incompat_flags) & VTXFS_FEAT_INCOMPAT_MULTIDEV)
	       ? "ĐA THIẾT BỊ (MULTI-DEVICE JBOD/SPAN)" : "ĐƠN THIẾT BỊ (SINGLE-DEVICE)");
	printf("==================================================================================\n");

	for (i = 0; i < count; i++) {
		const struct vtx_dev_item *d = &h.fs->dev_tbl.devs[i];
		__u64 tb = vtx_cpu64(d->total_blocks);
		__u64 ub = vtx_cpu64(d->used_blocks);
		pool_total += tb;
		pool_used += ub;

		printf("  [Thiết bị #%u] %s\n",
		       vtx_cpu16(d->dev_id),
		       i == 0 ? "(Thiết bị khởi tạo chính / Bootstrap)" : "(Thiết bị mở rộng pool)");
		printf("    UUID              : ");
		vtxfs_cli_print_uuid(d->dev_uuid);
		printf("\n");
		printf("    Phân tầng lưu trữ : %s (Tier ID %u)\n",
		       tier_name(vtx_cpu16(d->tier_id)), vtx_cpu16(d->tier_id));
		printf("    Trạng thái        : %s\n",
		       dev_state_str(vtx_cpu32(d->state_flags)));
		printf("    Kích thước sector : %u bytes | Kích thước khối: 4096 bytes\n",
		       vtx_cpu32(d->sector_size));
		printf("    Thế hệ (Gen)      : %u\n", vtx_cpu32(d->generation));
		printf("    Dung lượng        : %llu blocks (%.2f MiB / %.2f GiB)\n",
		       (unsigned long long)tb,
		       (double)(tb * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0),
		       (double)(tb * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0 * 1024.0));
		printf("    Đã sử dụng        : %llu blocks (%.2f MiB, %.2f%%)\n",
		       (unsigned long long)ub,
		       (double)(ub * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0),
		       tb > 0 ? ((double)ub * 100.0 / (double)tb) : 0.0);
		printf("    Số khe VAG        : %u slots (đã dùng: %u slots)\n",
		       vtx_cpu32(d->total_vag_slots), vtx_cpu32(d->used_vag_slots));
		printf("    Số lỗi I/O        : %u\n", vtx_cpu32(d->error_count));
		printf("----------------------------------------------------------------------------------\n");
	}

	printf("  TỔNG QUAN TOÀN BỘ STORAGE POOL:\n");
	printf("    Tổng dung lượng pool : %.2f MiB (%.2f GiB)\n",
	       (double)(pool_total * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0),
	       (double)(pool_total * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0 * 1024.0));
	printf("    Tổng khối đã sử dụng : %llu blocks (%.2f MiB)\n",
	       (unsigned long long)pool_used,
	       (double)(pool_used * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0));
	printf("    Tổng khối còn trống  : %llu blocks (%.2f MiB)\n",
	       (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.free_blocks),
	       (double)(vtx_cpu64(h.fs->cp_blk.hdr.free_blocks) * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0));
	printf("==================================================================================\n");

	vtxfs_cli_close(&h, 0);
	return 0;
}

static int cmd_device_stats(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u32 count, i;
	int rc;

	(void)c;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs device stats <image|device>\n");
		return 1;
	}
	rc = vtxfs_cli_open(&h, argv[0], 0);
	if (rc)
		return 1;

	count = vtx_cpu32(h.fs->dev_tbl.hdr.entry_count);
	if (count == 0)
		count = 1;

	printf("Thống kê I/O thiết bị '%s':\n", argv[0]);
	for (i = 0; i < count; i++) {
		const struct vtx_dev_item *d = &h.fs->dev_tbl.devs[i];
		printf("  Thiết bị #%u (state: %s):\n",
		       vtx_cpu16(d->dev_id), dev_state_str(vtx_cpu32(d->state_flags)));
		printf("    Lỗi tích lũy   : %u\n", vtx_cpu32(d->error_count));
		if (i == 0) {
			printf("    Lượt đọc (read): %llu (phiên mount này)\n",
			       (unsigned long long)h.fs->bdev->reads_issued);
			printf("    Lượt ghi (write): %llu (phiên mount này)\n",
			       (unsigned long long)h.fs->bdev->writes_issued);
			printf("    Flush barriers : %llu (phiên mount này)\n",
			       (unsigned long long)h.fs->bdev->flushes_issued);
		}
	}
	vtxfs_cli_close(&h, 0);
	return 0;
}

static int cmd_device_add(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct vtx_bdev new_bdev;
	const char *target_img;
	const char *new_dev_path;
	__u32 new_id, new_vag_slots;
	__u64 new_blocks;
	int rc;

	(void)c;
	if (argc < 2) {
		fprintf(stderr, "Cách dùng: vtxfs device add <image|device_chính> <thiết_bị_mới>\n");
		fprintf(stderr, "Ví dụ:\n");
		fprintf(stderr, "  vtxfs device add /dev/sda2 /dev/sdc1\n");
		fprintf(stderr, "  vtxfs device add disk1.img disk2.img\n");
		return 1;
	}
	target_img = argv[0];
	new_dev_path = argv[1];

	/* Mở thiết bị mới để xác định kích thước */
	rc = vtx_file_bdev_open(&new_bdev, new_dev_path, 1, 0);
	if (rc) {
		fprintf(stderr, "vtxfs device add: không thể mở thiết bị mới '%s': %s\n",
			new_dev_path, strerror(-rc));
		return 1;
	}
	new_blocks = new_bdev.total_blocks;
	if (new_blocks < VTXFS_VAG_BLOCKS) {
		fprintf(stderr, "vtxfs device add: thiết bị '%s' quá nhỏ (< %u blocks)\n",
			new_dev_path, VTXFS_VAG_BLOCKS);
		vtx_file_bdev_close(&new_bdev);
		return 1;
	}

	/* Mở filesystem chính ở chế độ ghi */
	rc = vtxfs_cli_open(&h, target_img, 1);
	if (rc) {
		vtx_file_bdev_close(&new_bdev);
		return 1;
	}

	new_id = vtx_cpu32(h.fs->dev_tbl.hdr.entry_count);
	if (new_id >= VTXFS_MAX_DEVICES) {
		fprintf(stderr, "vtxfs device add: đã đạt giới hạn tối đa %u thiết bị!\n",
			VTXFS_MAX_DEVICES);
		vtxfs_cli_close(&h, 0);
		vtx_file_bdev_close(&new_bdev);
		return 1;
	}

	new_vag_slots = (__u32)(new_blocks / VTXFS_VAG_BLOCKS);

	/* Cấu hình thiết bị mới trong vtx_dev_table */
	struct vtx_dev_item *nd = &h.fs->dev_tbl.devs[new_id];
	memset(nd, 0, sizeof(*nd));
	memcpy(nd->dev_uuid, h.fs->super_blk.hdr.fsid_uuid, VTXFS_UUID_SIZE);
	nd->dev_uuid[15] ^= (__u8)(new_id & 0xFF); /* phân biệt UUID thành viên */
	nd->dev_id = vtx_le16((__u16)new_id);
	nd->tier_id = vtx_le16(VTX_TIER_SSD_WARM);
	nd->state_flags = vtx_le32(VTX_DEV_STATE_ONLINE);
	nd->sector_size = vtx_le32(VTXFS_BLOCK_SIZE);
	nd->generation = vtx_le32(1);
	nd->total_blocks = vtx_le64(new_blocks);
	nd->used_blocks = vtx_le64(0);
	nd->total_vag_slots = vtx_le32(new_vag_slots);
	nd->used_vag_slots = vtx_le32(0);
	nd->error_count = vtx_le32(0);

	/* Cập nhật header bảng thiết bị */
	h.fs->dev_tbl.hdr.entry_count = vtx_le32(new_id + 1);
	h.fs->dev_tbl.hdr.generation = vtx_le64(vtx_cpu64(h.fs->dev_tbl.hdr.generation) + 1);
	vtx_dev_table_checksum_compute(&h.fs->dev_tbl);

	/* Bật cờ VTXFS_FEAT_INCOMPAT_MULTIDEV và cập nhật superblock */
	__u64 incompat = vtx_cpu64(h.fs->super_blk.hdr.incompat_flags);
	incompat |= VTXFS_FEAT_INCOMPAT_MULTIDEV;
	h.fs->super_blk.hdr.incompat_flags = vtx_le64(incompat);
	h.fs->super_blk.hdr.device_count = vtx_le32(new_id + 1);

	__u64 cur_total = vtx_cpu64(h.fs->super_blk.hdr.total_blocks);
	h.fs->super_blk.hdr.total_blocks = vtx_le64(cur_total + new_blocks);
	h.fs->cp_blk.hdr.total_blocks = vtx_le64(cur_total + new_blocks);

	__u64 cur_free = vtx_cpu64(h.fs->cp_blk.hdr.free_blocks);
	h.fs->cp_blk.hdr.free_blocks = vtx_le64(cur_free + new_blocks);

	vtx_super_checksum_compute(&h.fs->super_blk);

	/* Ghi bảng cấu hình lên thiết bị mới */
	new_bdev.ops->write_blocks(&new_bdev, VTXFS_SUPER_RING_PBLK, 1, &h.fs->super_blk);
	new_bdev.ops->write_blocks(&new_bdev, VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK, 1, &h.fs->super_blk);
	new_bdev.ops->write_blocks(&new_bdev, VTXFS_DEV_TABLE_PBLK, VTXFS_DEV_TABLE_BLOCKS, &h.fs->dev_tbl);
	new_bdev.ops->flush(&new_bdev);
	vtx_file_bdev_close(&new_bdev);

	/* Ghi lại bảng thiết bị và superblock lên thiết bị chính */
	h.bdev.ops->write_blocks(&h.bdev, VTXFS_DEV_TABLE_PBLK, VTXFS_DEV_TABLE_BLOCKS, &h.fs->dev_tbl);
	h.bdev.ops->write_blocks(&h.bdev, VTXFS_SUPER_RING_PBLK, 1, &h.fs->super_blk);
	h.bdev.ops->write_blocks(&h.bdev, VTXFS_BOOT_MIRROR0_PBLK + VTXFS_SUPER_RING_PBLK, 1, &h.fs->super_blk);

	/* Commit transaction checkpoint */
	rc = vtx_sync_checkpoint(h.fs);
	vtxfs_cli_close(&h, rc ? 0 : 1);

	if (rc) {
		fprintf(stderr, "vtxfs device add: commit checkpoint thất bại: %s\n", strerror(-rc));
		return 1;
	}

	printf("==================================================================================\n");
	printf("  VTXFS — THÊM THIẾT BỊ MỞ RỘNG VÀO STORAGE POOL THÀNH CÔNG\n");
	printf("  Đường dẫn thiết bị mới : %s\n", new_dev_path);
	printf("  Mã thiết bị (dev_id)   : %u\n", new_id);
	printf("  Dung lượng mở rộng     : %llu blocks (%.2f MiB / %.2f GiB)\n",
	       (unsigned long long)new_blocks,
	       (double)(new_blocks * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0),
	       (double)(new_blocks * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0 * 1024.0));
	printf("  Tổng dung lượng pool   : %.2f GiB (%u thiết bị thành viên)\n",
	       (double)((cur_total + new_blocks) * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0 * 1024.0),
	       new_id + 1);
	printf("  Cờ tính năng           : VTXFS_FEAT_INCOMPAT_MULTIDEV (KÍCH HOẠT)\n");
	printf("==================================================================================\n");

	return 0;
}

static int cmd_device_remove(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	__u32 dev_id;
	__u32 count;
	int rc;

	(void)c;
	if (argc < 2) {
		fprintf(stderr, "Cách dùng: vtxfs device remove <image|device> <dev_id>\n");
		return 1;
	}
	dev_id = (__u32)strtoul(argv[1], NULL, 10);
	if (dev_id == 0) {
		fprintf(stderr, "vtxfs device remove: không thể gỡ bỏ thiết bị gốc #0 (Bootstrap device)!\n");
		return 1;
	}

	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc)
		return 1;

	count = vtx_cpu32(h.fs->dev_tbl.hdr.entry_count);
	if (dev_id >= count) {
		fprintf(stderr, "vtxfs device remove: thiết bị #%u không tồn tại (tổng: %u)!\n",
			dev_id, count);
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	struct vtx_dev_item *d = &h.fs->dev_tbl.devs[dev_id];
	if (vtx_cpu64(d->used_blocks) > 0) {
		d->state_flags = vtx_le32(VTX_DEV_STATE_EVACUATING);
		printf("Cảnh báo: Thiết bị #%u đang chứa %llu khối dữ liệu.\n",
		       dev_id, (unsigned long long)vtx_cpu64(d->used_blocks));
		printf("Đã chuyển trạng thái thiết bị sang EVACUATING để di tản dữ liệu.\n");
	} else {
		__u64 removed_blocks = vtx_cpu64(d->total_blocks);
		d->state_flags = vtx_le32(VTX_DEV_STATE_MISSING);
		d->total_blocks = 0;

		__u64 cur_total = vtx_cpu64(h.fs->super_blk.hdr.total_blocks);
		if (cur_total > removed_blocks)
			h.fs->super_blk.hdr.total_blocks = vtx_le64(cur_total - removed_blocks);
		__u64 cur_free = vtx_cpu64(h.fs->cp_blk.hdr.free_blocks);
		if (cur_free > removed_blocks)
			h.fs->cp_blk.hdr.free_blocks = vtx_le64(cur_free - removed_blocks);

		printf("Đã gỡ bỏ thiết bị #%u khỏi nhóm lưu trữ (thu hồi %.2f MiB).\n",
		       dev_id, (double)(removed_blocks * VTXFS_BLOCK_SIZE) / (1024.0 * 1024.0));
	}

	while (count > 1 && vtx_cpu32(h.fs->dev_tbl.devs[count - 1].state_flags) == VTX_DEV_STATE_MISSING) {
		memset(&h.fs->dev_tbl.devs[count - 1], 0, sizeof(struct vtx_dev_item));
		count--;
	}
	h.fs->dev_tbl.hdr.entry_count = vtx_le32(count);
	h.fs->super_blk.hdr.device_count = vtx_le32(count);
	if (count == 1) {
		h.fs->super_blk.hdr.incompat_flags = vtx_le64(
			vtx_cpu64(h.fs->super_blk.hdr.incompat_flags) & ~VTXFS_FEAT_INCOMPAT_MULTIDEV);
	}

	h.fs->dev_tbl.hdr.generation = vtx_le64(vtx_cpu64(h.fs->dev_tbl.hdr.generation) + 1);
	vtx_dev_table_checksum_compute(&h.fs->dev_tbl);
	vtx_super_checksum_compute(&h.fs->super_blk);

	h.bdev.ops->write_blocks(&h.bdev, VTXFS_DEV_TABLE_PBLK, VTXFS_DEV_TABLE_BLOCKS, &h.fs->dev_tbl);
	h.bdev.ops->write_blocks(&h.bdev, VTXFS_SUPER_RING_PBLK, 1, &h.fs->super_blk);

	rc = vtx_sync_checkpoint(h.fs);
	vtxfs_cli_close(&h, rc ? 0 : 1);
	return rc ? 1 : 0;
}

static int cmd_device_replace(const struct cmd_struct *c, int argc, char **argv)
{
	struct vtxfs_handle h;
	struct vtx_bdev new_bdev;
	__u32 dev_id;
	__u32 count;
	int rc;

	(void)c;
	if (argc < 3) {
		fprintf(stderr, "Cách dùng: vtxfs device replace <image|device> <dev_id> <thiết_bị_mới>\n");
		return 1;
	}
	dev_id = (__u32)strtoul(argv[1], NULL, 10);
	const char *new_dev_path = argv[2];

	rc = vtx_file_bdev_open(&new_bdev, new_dev_path, 1, 0);
	if (rc) {
		fprintf(stderr, "vtxfs device replace: không thể mở thiết bị thay thế '%s': %s\n",
			new_dev_path, strerror(-rc));
		return 1;
	}

	rc = vtxfs_cli_open(&h, argv[0], 1);
	if (rc) {
		vtx_file_bdev_close(&new_bdev);
		return 1;
	}

	count = vtx_cpu32(h.fs->dev_tbl.hdr.entry_count);
	if (dev_id >= count) {
		fprintf(stderr, "vtxfs device replace: dev_id #%u không hợp lệ!\n", dev_id);
		vtxfs_cli_close(&h, 0);
		vtx_file_bdev_close(&new_bdev);
		return 1;
	}

	struct vtx_dev_item *d = &h.fs->dev_tbl.devs[dev_id];
	if (new_bdev.total_blocks < vtx_cpu64(d->total_blocks)) {
		fprintf(stderr, "vtxfs device replace: thiết bị mới nhỏ hơn thiết bị cần thay thế!\n");
		vtxfs_cli_close(&h, 0);
		vtx_file_bdev_close(&new_bdev);
		return 1;
	}

	d->state_flags = vtx_le32(VTX_DEV_STATE_ONLINE);
	d->total_blocks = vtx_le64(new_bdev.total_blocks);
	d->generation = vtx_le32(vtx_cpu32(d->generation) + 1);

	h.fs->dev_tbl.hdr.generation = vtx_le64(vtx_cpu64(h.fs->dev_tbl.hdr.generation) + 1);
	vtx_dev_table_checksum_compute(&h.fs->dev_tbl);

	new_bdev.ops->write_blocks(&new_bdev, VTXFS_SUPER_RING_PBLK, 1, &h.fs->super_blk);
	new_bdev.ops->write_blocks(&new_bdev, VTXFS_DEV_TABLE_PBLK, VTXFS_DEV_TABLE_BLOCKS, &h.fs->dev_tbl);
	new_bdev.ops->flush(&new_bdev);
	vtx_file_bdev_close(&new_bdev);

	h.bdev.ops->write_blocks(&h.bdev, VTXFS_DEV_TABLE_PBLK, VTXFS_DEV_TABLE_BLOCKS, &h.fs->dev_tbl);
	rc = vtx_sync_checkpoint(h.fs);
	vtxfs_cli_close(&h, rc ? 0 : 1);

	printf("Đã thay thế thành công thiết bị #%u bằng '%s'.\n", dev_id, new_dev_path);
	return rc ? 1 : 0;
}

static const struct cmd_struct device_cmds[] = {
	{ "info",    cmd_device_info,    NULL, "<image|device>" },
	{ "stats",   cmd_device_stats,   NULL, "<image|device>" },
	{ "add",     cmd_device_add,     NULL, "<image|device> <new_dev_path>" },
	{ "remove",  cmd_device_remove,  NULL, "<image|device> <dev_id>" },
	{ "replace", cmd_device_replace, NULL, "<image|device> <dev_id> <new_dev_path>" },
};

const struct cmd_group device_cmd_group = {
	.name = "device",
	.usage = "Quản lý nhóm lưu trữ đa thiết bị (Multi-device JBOD/Linear pool)",
	.cmds = device_cmds,
	.ncmds = sizeof(device_cmds) / sizeof(device_cmds[0]),
};
