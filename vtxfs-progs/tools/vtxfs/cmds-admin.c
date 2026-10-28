// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * VTXFS v2.1 — Native C Administrative Suite: Stat, Block Map & Inspector
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "commands.h"
#include "common.h"
#include "vtxfs_disk.h"

#define VTX_BLOCK_SIZE       4096U
#define VTX_VAG_NUM_BLOCKS   32768U
#define VTX_SUPER_PBLK_START 16ULL
#define VTX_CPD_PBLK_START   8ULL
#define VTX_MIRROR0_PBLK     16384ULL
#define VTX_NODE_MAGIC_VAL   0x314E5856U  /* "VXN1" */
#define VTX_VAG_DIR_MAGIC_V  0x31444756U  /* "VGD1" */

#define TYPE_FREE            0
#define TYPE_DATA            1
#define TYPE_CONTAINER_LEAF  2
#define TYPE_CONTAINER_NODE  3
#define TYPE_CONTAINER_OTHER 4

static bool use_color(void)
{
	static int checked = -1;
	if (checked == -1)
		checked = isatty(STDOUT_FILENO);
	return checked == 1;
}

#define C_RESET   (use_color() ? "\033[0m" : "")
#define C_BOLD    (use_color() ? "\033[1m" : "")
#define C_DIM     (use_color() ? "\033[90m" : "")
#define C_RED     (use_color() ? "\033[31m" : "")
#define C_GREEN   (use_color() ? "\033[32m" : "")
#define C_YELLOW  (use_color() ? "\033[33m" : "")
#define C_MAGENTA (use_color() ? "\033[35m" : "")
#define C_CYAN    (use_color() ? "\033[36m" : "")
#define C_BLUE    (use_color() ? "\033[34m" : "")

static const char *get_item_type_name(__u8 t)
{
	switch (t) {
	case VTX_TYPE_ANY: return "ANY";
	case VTX_TYPE_INODE: return "INODE";
	case VTX_TYPE_INODE_REF: return "INODE_REF";
	case VTX_TYPE_XATTR: return "XATTR";
	case VTX_TYPE_DIR_ITEM: return "DIR_ITEM";
	case VTX_TYPE_DIR_INDEX: return "DIR_INDEX";
	case VTX_TYPE_EXTENT: return "EXTENT";
	case VTX_TYPE_SUBVOL_ROOT: return "SUBVOL_ROOT";
	case VTX_TYPE_OMAP_VAL: return "OMAP_VAL";
	case VTX_TYPE_REF: return "REF";
	case VTX_TYPE_SFQ: return "SFQ";
	case VTX_TYPE_REAPER_JOB: return "REAPER_JOB";
	case VTX_TYPE_UUID_MAP: return "UUID_MAP";
	default: return "UNKNOWN";
	}
}

/* Helper to read a 4KB block */
static int read_block_raw(int fd, __u64 pblk, void *buf)
{
	ssize_t n = pread(fd, buf, VTX_BLOCK_SIZE, (off_t)(pblk * VTX_BLOCK_SIZE));
	return (n == (ssize_t)VTX_BLOCK_SIZE) ? 0 : -EIO;
}

static int find_primary_super(int fd, struct vtx_super_block *sb_out, __u64 *pblk_out)
{
	for (__u64 pblk = VTX_SUPER_PBLK_START; pblk < VTX_SUPER_PBLK_START + 16; pblk++) {
		struct vtx_super_block sb;
		if (read_block_raw(fd, pblk, &sb) == 0) {
			if (memcmp(sb.hdr.magic, "VTXFS21", 7) == 0) {
				*sb_out = sb;
				*pblk_out = pblk;
				return 0;
			}
		}
	}
	return -ENOENT;
}

static int find_latest_cpd(int fd, struct vtx_checkpoint_desc *cpd_out, __u64 *pblk_out)
{
	__u64 max_xid = 0;
	bool found = false;
	__u64 slots[9];
	for (int i = 0; i < 8; i++)
		slots[i] = VTX_CPD_PBLK_START + (unsigned)i;
	slots[8] = VTX_MIRROR0_PBLK;

	for (int i = 0; i < 9; i++) {
		struct vtx_checkpoint_desc cpd;
		if (read_block_raw(fd, slots[i], &cpd) == 0) {
			if (memcmp(cpd.hdr.magic, "VTXCP21", 7) == 0) {
				__u64 xid = vtx_cpu64(cpd.hdr.xid);
				if (!found || xid >= max_xid) {
					max_xid = xid;
					*cpd_out = cpd;
					*pblk_out = slots[i];
					found = true;
				}
			}
		}
	}
	return found ? 0 : -ENOENT;
}

/* --------------------------------------------------------------------------
 * Subcommand: stat
 * -------------------------------------------------------------------------- */
int cmd_stat(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs stat <thiết_bị_hoặc_image>\n");
		return 1;
	}
	const char *dev_path = argv[0];
	int fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%sLỗi: Không thể mở '%s': %s (chạy với sudo?)%s\n",
			C_RED, dev_path, strerror(errno), C_RESET);
		return 1;
	}

	struct vtx_super_block sb;
	__u64 sb_pblk = 0;
	if (find_primary_super(fd, &sb, &sb_pblk) != 0) {
		fprintf(stderr, "%sLỗi: Không tìm thấy Superblock VTXFS hợp lệ trên %s!%s\n",
			C_RED, dev_path, C_RESET);
		close(fd);
		return 1;
	}

	struct vtx_checkpoint_desc cpd = {0};
	__u64 cpd_pblk = 0;
	bool has_cpd = (find_latest_cpd(fd, &cpd, &cpd_pblk) == 0);

	__u64 total_blks = vtx_cpu64(sb.hdr.total_blocks);
	double total_gib = (double)(total_blks * VTX_BLOCK_SIZE) / (1024.0 * 1024.0 * 1024.0);
	__u64 used_blks = has_cpd ? vtx_cpu64(cpd.hdr.used_blocks) : 0;
	__u64 free_blks = has_cpd ? vtx_cpu64(cpd.hdr.free_blocks) : total_blks;
	double used_pct = total_blks > 0 ? ((double)used_blks / (double)total_blks * 100.0) : 0.0;
	double free_pct = total_blks > 0 ? ((double)free_blks / (double)total_blks * 100.0) : 0.0;

	printf("==================================================================================\n");
	printf("  %sVTXFS v%u.%u — BÁO CÁO TOÀN DIỆN THÔNG SỐ HỆ THỐNG CÀI ĐẶT & VẬN HÀNH%s\n",
	       C_BOLD, vtx_cpu16(sb.hdr.major), vtx_cpu16(sb.hdr.minor), C_RESET);
	printf("  %sThiết bị: %s (Dung lượng: %.2f GiB / %llu blocks 4KB)%s\n",
	       C_DIM, dev_path, total_gib, (unsigned long long)total_blks, C_RESET);
	printf("==================================================================================\n");

	/* 1. Identifiers */
	printf("\n%s1. ĐỊNH DANH HỆ THỐNG & SUPERBLOCK (IDENTIFIERS & SUPERBLOCK):%s\n", C_CYAN, C_RESET);
	printf("  Nhãn phân vùng (Label)    : %s%s%s\n", C_BOLD, sb.hdr.label[0] ? sb.hdr.label : "(none)", C_RESET);
	printf("  Mã định danh FSID         : ");
	vtxfs_cli_print_uuid(sb.hdr.fsid_uuid);
	printf("\n  Device UUID               : ");
	vtxfs_cli_print_uuid(sb.hdr.device_uuid);
	printf("\n  Metadata UUID             : ");
	vtxfs_cli_print_uuid(sb.hdr.metadata_uuid);
	printf("\n  Superblock PBLK           : Block %llu (Slot %llu)\n",
	       (unsigned long long)sb_pblk, (unsigned long long)(sb_pblk - VTX_SUPER_PBLK_START));
	printf("  Super Generation          : %llu\n", (unsigned long long)vtx_cpu64(sb.hdr.super_generation));

	__u64 mkfs_ns = vtx_cpu64(sb.hdr.mkfs_time_ns);
	if (mkfs_ns > 0) {
		time_t t_sec = (time_t)(mkfs_ns / 1000000000ULL);
		struct tm tm_val;
		localtime_r(&t_sec, &tm_val);
		char time_buf[64];
		strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_val);
		printf("  Thời điểm tạo (mkfs time) : %s\n", time_buf);
	}
	__u16 state = vtx_cpu16(sb.hdr.state);
	printf("  Trạng thái Superblock     : 0x%04x (%s%s%s)\n",
	       state, (state == 1) ? C_GREEN : C_YELLOW,
	       (state == 1) ? "CLEAN" : "DIRTY/ACTIVE", C_RESET);
	printf("  Flags (compat/ro/incompat): 0x%llx / 0x%llx / 0x%llx\n",
	       (unsigned long long)vtx_cpu64(sb.hdr.compat_flags),
	       (unsigned long long)vtx_cpu64(sb.hdr.ro_compat_flags),
	       (unsigned long long)vtx_cpu64(sb.hdr.incompat_flags));

	/* 2. Storage Geometry */
	printf("\n%s2. HÌNH HỌC LƯU TRỮ & DUNG LƯỢNG (STORAGE GEOMETRY & SPACE):%s\n", C_CYAN, C_RESET);
	printf("  Kích thước khối (Block)   : %u bytes (4.0 KiB)\n", vtx_cpu32(sb.hdr.block_size));
	printf("  Kích thước Sector         : %u bytes\n", vtx_cpu32(sb.hdr.sector_size));
	printf("  Kích thước B-Tree Node    : %u bytes\n", vtx_cpu32(sb.hdr.node_size));
	printf("  Khối cấp phát đầu tiên    : PBLK %llu (32 MiB VAG boundary)\n",
	       (unsigned long long)vtx_cpu64(sb.hdr.first_alloc_pblk));
	printf("  Tổng số khối trên đĩa     : %llu blocks (%.2f GiB)\n", (unsigned long long)total_blks, total_gib);
	printf("  Số khối đã sử dụng (Used) : %llu blocks (%.2f MiB, %s%.2f%%%s)\n",
	       (unsigned long long)used_blks, (double)(used_blks * 4) / 1024.0, C_YELLOW, used_pct, C_RESET);
	printf("  Số khối còn trống (Free)  : %llu blocks (%.2f GiB, %s%.2f%%%s)\n",
	       (unsigned long long)free_blks, (double)(free_blks * 4) / (1024.0 * 1024.0), C_GREEN, free_pct, C_RESET);

	/* Progress bar */
	int bar_width = 40;
	int used_chars = (int)((used_pct / 100.0) * (double)bar_width + 0.5);
	if (used_chars > bar_width) used_chars = bar_width;
	int free_chars = bar_width - used_chars;
	printf("  Thanh dung lượng          : [%s", C_YELLOW);
	for (int i = 0; i < used_chars; i++) fputs("█", stdout);
	printf("%s%s", C_RESET, C_GREEN);
	for (int i = 0; i < free_chars; i++) fputs("░", stdout);
	printf("%s] %.1f%% used\n", C_RESET, used_pct);

	/* 3. Transaction & Checkpoint Engine */
	printf("\n%s3. ĐỘNG CƠ GIAO DỊCH & CHECKPOINT (COW TRANSACTION ENGINE):%s\n", C_CYAN, C_RESET);
	if (has_cpd) {
		__u64 xid = vtx_cpu64(cpd.hdr.xid);
		printf("  Committed XID (Thế hệ)    : %s%llu%s\n", C_BOLD, (unsigned long long)xid, C_RESET);
		printf("  Previous XID              : %llu\n", (unsigned long long)vtx_cpu64(cpd.hdr.prev_xid));
		if (cpd_pblk < VTX_MIRROR0_PBLK)
			printf("  CPD Active Ring Slot      : Slot %llu (PBLK %llu)\n",
			       (unsigned long long)(cpd_pblk - VTX_CPD_PBLK_START), (unsigned long long)cpd_pblk);
		else
			printf("  CPD Active Ring Slot      : MIRROR (PBLK %llu)\n", (unsigned long long)cpd_pblk);

		__u64 commit_ns = vtx_cpu64(cpd.hdr.commit_time_ns);
		if (commit_ns > 0) {
			time_t t_sec = (time_t)(commit_ns / 1000000000ULL);
			struct tm tm_val;
			localtime_r(&t_sec, &tm_val);
			char time_buf[64];
			strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_val);
			printf("  Thời điểm Commit cuối     : %s%s%s\n", C_BOLD, time_buf, C_RESET);
		}
		printf("  Reuse-Safe XID Boundary   : %llu\n", (unsigned long long)vtx_cpu64(cpd.hdr.reuse_safe_xid));
		printf("  Mã OID tiếp theo (NextOID): %llu\n", (unsigned long long)vtx_cpu64(cpd.hdr.next_oid));
		printf("  Checkpoint Flags          : 0x%08x\n", vtx_cpu32(cpd.hdr.cp_flags));
	} else {
		printf("  %sKhông tìm thấy CPD hợp lệ!%s\n", C_RED, C_RESET);
	}

	/* 4. Multi-Tree B-Tree Root Pointers */
	printf("\n%s4. CHỈ MỤC CÁC CÂY B-TREE HỆ THỐNG (B-TREE MULTI-ROOT HIERARCHY):%s\n", C_CYAN, C_RESET);
	if (has_cpd) {
		printf("  Root Tree (Cây thư mục gốc): VBLK 0x%llx  (Depth level: %u)\n",
		       (unsigned long long)vtx_cpu64(cpd.hdr.root_tree_vblk), vtx_cpu32(cpd.hdr.root_tree_level));
		printf("  OMAP Meta Tree (Định danh) : VBLK 0x%llx  (Depth level: %u)\n",
		       (unsigned long long)vtx_cpu64(cpd.hdr.omap_meta_tree_vblk), cpd.hdr.tree_levels[0]);
		printf("  Ref Tree (Quản lý Refcount): VBLK 0x%llx  (Depth level: %u)\n",
		       (unsigned long long)vtx_cpu64(cpd.hdr.ref_tree_vblk), cpd.hdr.tree_levels[1]);
		printf("  SFQ Tree (Hàng đợi thu hồi): VBLK 0x%llx  (Depth level: %u)\n",
		       (unsigned long long)vtx_cpu64(cpd.hdr.sfq_tree_vblk), cpd.hdr.tree_levels[2]);
		printf("  Reaper Tree (Dọn dẹp subvol): VBLK 0x%llx  (Depth level: %u)\n",
		       (unsigned long long)vtx_cpu64(cpd.hdr.reaper_tree_vblk), cpd.hdr.tree_levels[3]);
		printf("  UUID Tree (Ánh xạ UUID)    : VBLK 0x%llx  (Depth level: %u)\n",
		       (unsigned long long)vtx_cpu64(cpd.hdr.uuid_tree_vblk), cpd.hdr.tree_levels[4]);
		printf("  VAG Root Header PBLK      : PBLK %llu (Dev %u)\n",
		       (unsigned long long)vtx_cpu64(cpd.hdr.vag_dir_root.pblk), vtx_cpu16(cpd.hdr.vag_dir_root.dev_id));
	}

	/* 5. VAG Topography */
	printf("\n%s5. ĐỊA HÌNH CÁC NHÓM CẤP PHÁT (VAG ALLOCATION TOPOGRAPHY):%s\n", C_CYAN, C_RESET);
	__u64 first_alloc = vtx_cpu64(sb.hdr.first_alloc_pblk);
	__u32 total_vags = (total_blks > first_alloc) ? (uint32_t)((total_blks - first_alloc) / VTX_VAG_NUM_BLOCKS) : 1;
	printf("  Tổng số VAG trên đĩa      : %u VAGs (%u blocks = 128.0 MiB / VAG)\n",
	       total_vags, VTX_VAG_NUM_BLOCKS);
	printf("  ------------------------------------------------------------------------------\n");
	printf("  %-6s %-22s %12s %12s %10s  %s\n", "VAG", "Phạm vi PBLK", "Đã dùng", "Còn trống", "Sử dụng", "Trạng thái");
	printf("  ------------------------------------------------------------------------------\n");

	__u32 max_display = (total_vags < 12) ? total_vags : 12;
	for (__u32 vid = 0; vid < max_display; vid++) {
		__u64 start_p = (vid == 0) ? 32768ULL : (32768ULL + vid * VTX_VAG_NUM_BLOCKS);
		__u64 end_p = start_p + VTX_VAG_NUM_BLOCKS - 1;
		__u64 vag_used = 0;
		if (has_cpd && used_blks > vid * VTX_VAG_NUM_BLOCKS) {
			__u64 rem = used_blks - vid * VTX_VAG_NUM_BLOCKS;
			vag_used = (rem < VTX_VAG_NUM_BLOCKS) ? rem : VTX_VAG_NUM_BLOCKS;
		}
		__u64 vag_free = VTX_VAG_NUM_BLOCKS - vag_used;
		double v_pct = ((double)vag_used / (double)VTX_VAG_NUM_BLOCKS) * 100.0;
		int filled = (int)((v_pct / 10.0) + 0.5);
		if (filled > 10) filled = 10;
		char mini_bar[32];
		int pos = 0;
		for (int i = 0; i < filled; i++) pos += sprintf(mini_bar + pos, "█");
		for (int i = 0; i < 10 - filled; i++) pos += sprintf(mini_bar + pos, "░");

		printf("  %-6u %8llu..%-11llu %10llu blk %10llu blk %9.1f%%  [%s]\n",
		       vid, (unsigned long long)start_p, (unsigned long long)end_p,
		       (unsigned long long)vag_used, (unsigned long long)vag_free, v_pct, mini_bar);
	}
	if (total_vags > 12)
		printf("  ... và %u VAGs tiếp theo còn trống 100%% ...\n", total_vags - 12);
	printf("  ------------------------------------------------------------------------------\n");

	/* 6. Kernel & Hardware Runtime */
	printf("\n%s6. THÔNG SỐ VẬN HÀNH KERNEL LINUX & PHẦN CỨNG (KERNEL & HARDWARE RUNTIME):%s\n", C_CYAN, C_RESET);

	/* Check kernel module */
	bool mod_loaded = false;
	char mod_size[32] = {0};
	char mod_ref[32] = {0};
	FILE *f_mod = fopen("/proc/modules", "r");
	if (f_mod) {
		char line[256];
		while (fgets(line, sizeof(line), f_mod)) {
			char name[64];
			if (sscanf(line, "%63s %31s %31s", name, mod_size, mod_ref) >= 1) {
				if (strcmp(name, "vtxfs") == 0) {
					mod_loaded = true;
					break;
				}
			}
		}
		fclose(f_mod);
	}
	if (mod_loaded)
		printf("  Mô-đun Kernel vtxfs.ko    : %sĐÃ TẢI%s (Dung lượng: %s bytes, Refcount: %s)\n",
		       C_GREEN, C_RESET, mod_size, mod_ref);
	else
		printf("  Mô-đun Kernel vtxfs.ko    : %sCHƯA TẢI (chạy qua Userspace Engine)%s\n",
		       C_YELLOW, C_RESET);

	/* Check mount */
	bool mounted = false;
	char mnt_point[256] = {0};
	char mnt_opts[256] = {0};
	char real_target[PATH_MAX];
	if (realpath(dev_path, real_target)) {
		FILE *f_mnt = fopen("/proc/mounts", "r");
		if (f_mnt) {
			char line[512];
			while (fgets(line, sizeof(line), f_mnt)) {
				char m_dev[256], m_pt[256], m_tp[64], m_op[256];
				if (sscanf(line, "%255s %255s %63s %255s", m_dev, m_pt, m_tp, m_op) >= 4) {
					char real_mdev[PATH_MAX];
					if (realpath(m_dev, real_mdev) && strcmp(real_mdev, real_target) == 0) {
						mounted = true;
						snprintf(mnt_point, sizeof(mnt_point), "%s", m_pt);
						snprintf(mnt_opts, sizeof(mnt_opts), "%s", m_op);
						break;
					}
				}
			}
			fclose(f_mnt);
		}
	}
	if (mounted) {
		printf("  Trạng thái Mount          : %sĐANG HOẠT ĐỘNG%s tại %s%s%s\n",
		       C_GREEN, C_RESET, C_BOLD, mnt_point, C_RESET);
		printf("  Tùy chọn Mount (Options)  : %s\n", mnt_opts);
	} else {
		printf("  Trạng thái Mount          : %sCHƯA GẮN KẾT (UNMOUNTED / OFFLINE - SAFE)%s\n",
		       C_YELLOW, C_RESET);
	}

	printf("\n==================================================================================\n");
	close(fd);
	return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: block
 * -------------------------------------------------------------------------- */
int cmd_block(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 2) {
		fprintf(stderr, "Cách dùng: vtxfs block <thiết_bị_hoặc_image> <block_id> [--vblk]\n");
		return 1;
	}
	const char *dev_path = argv[0];
	char *endptr = NULL;
	errno = 0;
	__u64 block_id = strtoull(argv[1], &endptr, 0);
	if (errno || !endptr || *endptr) {
		fprintf(stderr, "%sLỗi: Số hiệu khối không hợp lệ: '%s'%s\n", C_RED, argv[1], C_RESET);
		return 1;
	}

	bool is_vblk = false;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--vblk") == 0)
			is_vblk = true;
	}

	__u64 pblk = block_id;
	if (is_vblk) {
		pblk = 32768ULL + block_id;
		printf("%sChuyển đổi VBLK 0x%llx (%llu) -> PBLK %llu (Khối vật lý)%s\n\n",
		       C_CYAN, (unsigned long long)block_id, (unsigned long long)block_id, (unsigned long long)pblk, C_RESET);
	}

	int fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%sLỗi: Không thể mở '%s': %s%s\n", C_RED, dev_path, strerror(errno), C_RESET);
		return 1;
	}

	__u8 raw[VTX_BLOCK_SIZE];
	if (read_block_raw(fd, pblk, raw) != 0) {
		fprintf(stderr, "%sLỗi: Không thể đọc PBLK %llu từ %s!%s\n", C_RED, (unsigned long long)pblk, dev_path, C_RESET);
		close(fd);
		return 1;
	}

	printf("==================================================================================\n");
	printf("  %sVTXFS — GIẢI MÃ & KIỂM TRA CHI TIẾT KHỐI VẬT LÝ (BLOCK INSPECTOR)%s\n", C_BOLD, C_RESET);
	printf("  %sThiết bị: %s | PBLK: %llu (Byte offset: %llu .. %llu)%s\n",
	       C_DIM, dev_path, (unsigned long long)pblk,
	       (unsigned long long)(pblk * VTX_BLOCK_SIZE),
	       (unsigned long long)((pblk + 1) * VTX_BLOCK_SIZE - 1), C_RESET);
	printf("==================================================================================\n");

	/* 1. All zeros check */
	bool all_zeros = true;
	for (size_t i = 0; i < VTX_BLOCK_SIZE; i++) {
		if (raw[i] != 0) {
			all_zeros = false;
			break;
		}
	}
	if (all_zeros) {
		printf("\n  %s[!] KHỐI RỖNG HOÀN TOÀN (ALL ZEROS / UNALLOCATED BLOCK)%s\n", C_YELLOW, C_RESET);
		printf("  Tất cả 4,096 bytes đều là 0x00. Khối này chưa được cấp phát hoặc vừa được trim/zero.\n");
		close(fd);
		return 0;
	}

	/* 2. Check Magic & Auto-Decode */
	__u32 node_magic = 0;
	memcpy(&node_magic, raw + 48, sizeof(node_magic));
	node_magic = vtx_cpu32(node_magic);

	/* Check Superblock */
	if (memcmp(raw + 32, "VTXFS21\0", 8) == 0 || memcmp(raw + 32, "VTXFS21", 7) == 0) {
		const struct vtx_super_block *sb = (const struct vtx_super_block *)(const void *)raw;
		printf("\n  %s===> PHÁT HIỆN: MASTER SUPERBLOCK (VTXFS21) <===%s\n", C_GREEN, C_RESET);
		printf("  Phiên bản định dạng      : VTXFS v%u.%u\n", vtx_cpu16(sb->hdr.major), vtx_cpu16(sb->hdr.minor));
		printf("  Nhãn hệ thống (Label)    : %s%s%s\n", C_BOLD, sb->hdr.label[0] ? sb->hdr.label : "(none)", C_RESET);
		printf("  Mã FSID UUID             : ");
		vtxfs_cli_print_uuid(sb->hdr.fsid_uuid);
		printf("\n  Device UUID              : ");
		vtxfs_cli_print_uuid(sb->hdr.device_uuid);
		printf("\n  Metadata UUID            : ");
		vtxfs_cli_print_uuid(sb->hdr.metadata_uuid);
		printf("\n  Superblock Generation    : %llu\n", (unsigned long long)vtx_cpu64(sb->hdr.super_generation));
		printf("  Tổng số khối trên đĩa    : %llu blocks\n", (unsigned long long)vtx_cpu64(sb->hdr.total_blocks));
		printf("  Khối cấp phát đầu tiên   : PBLK %llu\n", (unsigned long long)vtx_cpu64(sb->hdr.first_alloc_pblk));
		__u16 st = vtx_cpu16(sb->hdr.state);
		printf("  Trạng thái hoạt động     : 0x%04x (%s%s%s)\n", st, (st == 1) ? C_GREEN : C_YELLOW,
		       (st == 1) ? "CLEAN" : "DIRTY", C_RESET);
	}
	/* Check CPD */
	else if (memcmp(raw + 32, "VTXCP21\0", 8) == 0 || memcmp(raw + 32, "VTXCP21", 7) == 0) {
		const struct vtx_checkpoint_desc *cpd = (const struct vtx_checkpoint_desc *)(const void *)raw;
		printf("\n  %s===> PHÁT HIỆN: CHECKPOINT DESCRIPTOR (CPD / VTXCP21) <===%s\n", C_GREEN, C_RESET);
		printf("  Mã giao dịch (XID)       : %s%llu%s\n", C_BOLD, (unsigned long long)vtx_cpu64(cpd->hdr.xid), C_RESET);
		printf("  Previous XID             : %llu\n", (unsigned long long)vtx_cpu64(cpd->hdr.prev_xid));
		__u64 commit_ns = vtx_cpu64(cpd->hdr.commit_time_ns);
		if (commit_ns > 0) {
			time_t t_sec = (time_t)(commit_ns / 1000000000ULL);
			struct tm tm_val;
			localtime_r(&t_sec, &tm_val);
			char time_buf[64];
			strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_val);
			printf("  Thời điểm Commit bền vững: %s%s%s\n", C_BOLD, time_buf, C_RESET);
		}
		printf("  Số khối đã sử dụng       : %llu blocks\n", (unsigned long long)vtx_cpu64(cpd->hdr.used_blocks));
		printf("  Số khối còn trống        : %llu blocks\n", (unsigned long long)vtx_cpu64(cpd->hdr.free_blocks));
		printf("  Root Tree Root VBLK      : 0x%llx (Level %u)\n",
		       (unsigned long long)vtx_cpu64(cpd->hdr.root_tree_vblk), vtx_cpu32(cpd->hdr.root_tree_level));
		printf("  OMAP Meta Tree VBLK      : 0x%llx (Level %u)\n",
		       (unsigned long long)vtx_cpu64(cpd->hdr.omap_meta_tree_vblk), cpd->hdr.tree_levels[0]);
		printf("  Ref Tree Root VBLK       : 0x%llx (Level %u)\n",
		       (unsigned long long)vtx_cpu64(cpd->hdr.ref_tree_vblk), cpd->hdr.tree_levels[1]);
		printf("  SFQ Tree Root VBLK       : 0x%llx (Level %u)\n",
		       (unsigned long long)vtx_cpu64(cpd->hdr.sfq_tree_vblk), cpd->hdr.tree_levels[2]);
		printf("  Reaper Tree Root VBLK    : 0x%llx (Level %u)\n",
		       (unsigned long long)vtx_cpu64(cpd->hdr.reaper_tree_vblk), cpd->hdr.tree_levels[3]);
		printf("  VAG Root Header PBLK     : PBLK %llu\n", (unsigned long long)vtx_cpu64(cpd->hdr.vag_dir_root.pblk));
	}
	/* Check B-Tree Node */
	else if (node_magic == VTX_NODE_MAGIC_VAL) {
		const struct vtx_node_hdr *nhdr = (const struct vtx_node_hdr *)(const void *)raw;
		__u16 flags = vtx_cpu16(nhdr->flags);
		bool is_leaf = (flags & VTX_NODE_FLAG_LEAF) != 0;
		bool is_root = (flags & VTX_NODE_FLAG_ROOT) != 0;
		__u16 nitems = vtx_cpu16(nhdr->nitems);
		__u16 f_head = vtx_cpu16(nhdr->free_head8) * 8;
		__u16 f_tail = vtx_cpu16(nhdr->free_tail8) * 8;

		printf("\n  %s===> PHÁT HIỆN: B-TREE SLOTTED NODE (VXN1) <===%s\n", C_GREEN, C_RESET);
		printf("  Loại Node                : %s%s%s\n", C_BOLD,
		       is_leaf ? "LEAF (Khối lá - Chứa Items)" : "INTERNAL (Khối trong - Con trỏ rẽ nhánh)", C_RESET);
		printf("  Cấp độ cây (Level)       : %u (%s)\n", nhdr->level, is_root ? "Gốc cây / Root" : "Nhánh");
		printf("  Định danh OID / Owner    : OID %llu / Owner %llu\n",
		       (unsigned long long)vtx_cpu64(nhdr->oid), (unsigned long long)vtx_cpu64(nhdr->owner_id));
		printf("  Self VBLK                : 0x%llx (%llu)\n",
		       (unsigned long long)vtx_cpu64(nhdr->self_vblk), (unsigned long long)vtx_cpu64(nhdr->self_vblk));
		printf("  Thế hệ giao dịch (XID)   : %llu\n", (unsigned long long)vtx_cpu64(nhdr->xid));
		printf("  Số lượng Items (NItems)  : %s%u%s\n", C_BOLD, nitems, C_RESET);
		printf("  Vùng trống (Free Space)  : %d bytes (Head: %u, Tail: %u)\n",
		       (int)(f_tail - f_head), f_head, f_tail);

		/* Print items */
		printf("\n  %sDANH SÁCH CÁC MỤC DỮ LIỆU (ITEMS IN NODE):%s\n", C_BOLD, C_RESET);
		printf("  ----------------------------------------------------------------------------\n");
		printf("  %-4s %-32s %-14s %-18s\n", "STT", "Khóa (Key)", "Kiểu Item", "Vị trí (Offset/Len)");
		printf("  ----------------------------------------------------------------------------\n");

		size_t hdr_sz = sizeof(struct vtx_node_hdr);
		const struct vtx_node_entry *entries = (const struct vtx_node_entry *)(const void *)(raw + hdr_sz);
		__u32 show_count = (nitems < 32) ? nitems : 32;

		for (__u32 i = 0; i < show_count; i++) {
			__u64 ot = vtx_cpu64(entries[i].key.oid_and_type);
			__u64 item_oid = VTX_KEY_GET_OID(ot);
			__u8 item_type = VTX_KEY_GET_TYPE(ot);
			__u64 k_off = vtx_cpu64(entries[i].key.offset);
			__u16 v_off = vtx_cpu16(entries[i].slot.val_off8) * 8;
			__u16 v_len = vtx_cpu16(entries[i].slot.val_len);

			printf("  %-4u (OID=%-6llu Off=%-8llu) %s%-20s%s %u..%u (%uB)\n",
			       i, (unsigned long long)item_oid, (unsigned long long)k_off,
			       C_CYAN, get_item_type_name(item_type), C_RESET,
			       v_off, v_off + v_len, v_len);

			if (v_off + v_len <= VTX_BLOCK_SIZE) {
				const __u8 *val_ptr = raw + v_off;
				if (item_type == VTX_TYPE_DIR_ITEM && v_len >= sizeof(struct vtx_dir_item)) {
					const struct vtx_dir_item *di = (const struct vtx_dir_item *)(const void *)val_ptr;
					__u16 nlen = vtx_cpu16(di->name_len);
					const char *dname = (const char *)(val_ptr + sizeof(struct vtx_dir_item));
					printf("       └──> %sTệp/Thư mục%s: %s%.*s%s (Inode=%llu, Loại=%s)\n",
					       C_GREEN, C_RESET, C_BOLD, (int)nlen, dname, C_RESET,
					       (unsigned long long)vtx_cpu64(di->target_ino), vtxfs_cli_ftype_name(di->file_type));
				} else if (item_type == VTX_TYPE_INODE && v_len >= sizeof(struct vtx_inode_item)) {
					const struct vtx_inode_item *ii = (const struct vtx_inode_item *)(const void *)val_ptr;
					printf("       └──> %sInode Spec%s: Size=%lluB, Alloc=%lluB, Mode=0%o, Links=%u\n",
					       C_YELLOW, C_RESET, (unsigned long long)vtx_cpu64(ii->size_bytes),
					       (unsigned long long)vtx_cpu64(ii->alloc_bytes),
					       vtx_cpu32(ii->mode), vtx_cpu32(ii->nlink));
				} else if (item_type == VTX_TYPE_EXTENT && v_len >= sizeof(struct vtx_extent_item)) {
					const struct vtx_extent_item *ei = (const struct vtx_extent_item *)(const void *)val_ptr;
					__u32 fl = vtx_cpu32(ei->flags);
					const char *fl_str = (fl & VTX_EXTENT_INLINE) ? "INLINE" :
					                     ((fl & VTX_EXTENT_SHARED) ? "SHARED" : "NORMAL");
					printf("       └──> %sExtent%s: VBLK=0x%llx, Số khối=%u (%lluB), Kiểu=%s\n",
					       C_MAGENTA, C_RESET, (unsigned long long)vtx_cpu64(ei->vblk), vtx_cpu32(ei->physical_blocks),
					       (unsigned long long)vtx_cpu32(ei->logical_bytes), fl_str);
				} else if (item_type == VTX_TYPE_SUBVOL_ROOT && v_len >= sizeof(struct vtx_subvol_root_item)) {
					const struct vtx_subvol_root_item *sv = (const struct vtx_subvol_root_item *)(const void *)val_ptr;
					printf("       └──> %sSubvolume Root%s: Root VBLK=0x%llx, XID=%llu, Subvol ID=%llu\n",
					       C_CYAN, C_RESET, (unsigned long long)vtx_cpu64(sv->root_node_vblk),
					       (unsigned long long)vtx_cpu64(sv->create_xid), (unsigned long long)vtx_cpu64(sv->subvol_id));
				}
			}
		}
		if (nitems > 32)
			printf("  ... và %u mục khác ...\n", nitems - 32);
		printf("  ----------------------------------------------------------------------------\n");
	} else {
		/* Fallback text scan */
		int print_count = 0;
		char cur_str[80];
		int cur_len = 0;
		bool found_text = false;

		for (size_t i = 0; i < VTX_BLOCK_SIZE; i++) {
			unsigned char b = raw[i];
			if ((b >= 32 && b <= 126) || b == '\t' || b == '\n' || b == '\r') {
				if (cur_len < 75)
					cur_str[cur_len++] = (char)b;
			} else {
				if (cur_len >= 4) {
					cur_str[cur_len] = '\0';
					if (print_count == 0) {
						printf("\n  %s===> PHÁT HIỆN: KHỐI DỮ LIỆU TỆP TIN (FILE CONTENT DATA BLOCK) <===%s\n",
						       C_CYAN, C_RESET);
						printf("  Tìm thấy các chuỗi ký tự văn bản có nghĩa bên trong khối:\n");
						found_text = true;
					}
					if (print_count < 6)
						printf("    • %s%s%s\n", C_BOLD, cur_str, C_RESET);
					print_count++;
				}
				cur_len = 0;
			}
		}
		if (!found_text) {
			printf("\n  %s===> KHỐI DỮ LIỆU NHỊ PHÂN (OPAQUE BINARY DATA BLOCK) <===%s\n", C_DIM, C_RESET);
		} else if (print_count > 6) {
			printf("    ... và %d chuỗi khác ...\n", print_count - 6);
		}
	}

	/* 3. Hex & ASCII Dump */
	printf("\n  %sXUẤT MÃ HEX & ASCII CHI TIẾT (HEXDUMP 4096 BYTES):%s\n", C_BOLD, C_RESET);
	printf("  ----------------------------------------------------------------------------\n");

	const __u8 *prev_chunk = NULL;
	bool star_printed = false;

	for (size_t row = 0; row < VTX_BLOCK_SIZE; row += 16) {
		const __u8 *chunk = raw + row;
		if (prev_chunk && memcmp(chunk, prev_chunk, 16) == 0) {
			if (!star_printed) {
				printf("  *\n");
				star_printed = true;
			}
			continue;
		}
		star_printed = false;
		prev_chunk = chunk;

		printf("  %08zx  ", row);
		for (size_t i = 0; i < 16; i++) {
			printf("%02x ", chunk[i]);
			if (i == 7) printf(" ");
		}
		printf(" |");
		for (size_t i = 0; i < 16; i++) {
			printf("%c", (chunk[i] >= 32 && chunk[i] <= 126) ? (char)chunk[i] : '.');
		}
		printf("|\n");
	}
	printf("  %08x\n", VTX_BLOCK_SIZE);
	printf("  ----------------------------------------------------------------------------\n");

	close(fd);
	return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: map & bitmap
 * -------------------------------------------------------------------------- */
int cmd_map(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs map <thiết_bị_hoặc_image> [--vag=N] [--cols=N] [--compact] [--ranges=N]\n");
		return 1;
	}
	const char *dev_path = argv[0];
	int vag_filter = -1;
	int cols = 64;
	bool compact = false;
	int max_ranges = 25;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--vag=", 6) == 0) {
			vag_filter = atoi(argv[i] + 6);
		} else if (strcmp(argv[i], "--vag") == 0 && i + 1 < argc) {
			vag_filter = atoi(argv[++i]);
		} else if (strncmp(argv[i], "--cols=", 7) == 0) {
			cols = atoi(argv[i] + 7);
		} else if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "-s") == 0) {
			return cmd_sys_index(cmd, argc, argv);
		} else if (strcmp(argv[i], "--compact") == 0) {
			compact = true;
		} else if (strncmp(argv[i], "--ranges=", 9) == 0) {
			max_ranges = atoi(argv[i] + 9);
		}
	}
	if (cols <= 0 || cols > 128) cols = 64;

	struct vtxfs_handle h;
	int rc = vtxfs_cli_open(&h, dev_path, 0);
	if (rc != 0) {
		fprintf(stderr, "%sLỗi: Không thể mở VTXFS trên %s (%s)%s\n",
			C_RED, dev_path, strerror(-rc), C_RESET);
		return 1;
	}

	__u32 total_vags = h.fs->vag.total_vags;
	int start_vag = 0;
	int end_vag = (total_vags < 4) ? (int)total_vags : 4;
	if (vag_filter >= 0) {
		if ((unsigned)vag_filter >= total_vags) {
			fprintf(stderr, "%sLỗi: VAG #%d vượt quá tổng số %u VAGs!%s\n",
				C_RED, vag_filter, total_vags, C_RESET);
			vtxfs_cli_close(&h, 0);
			return 1;
		}
		start_vag = vag_filter;
		end_vag = vag_filter + 1;
	}

	printf("==================================================================================\n");
	printf("  %sVTXFS — BẢN ĐỒ PHÂN BỐ KHỐI VẬT LÝ (BLOCK ALLOCATION BITMAP MAP)%s\n", C_BOLD, C_RESET);
	printf("  %sThiết bị: %s | Tổng số VAG: %u | Khối mỗi VAG: %u blocks (128 MiB)%s\n",
	       C_DIM, dev_path, total_vags, VTX_VAG_NUM_BLOCKS, C_RESET);
	printf("==================================================================================\n");
	printf("\n  %sCHÚ GIẢI KÝ HIỆU (MAP LEGEND):%s\n", C_BOLD, C_RESET);
	printf("    %s■%s : Khối DỮ LIỆU TỆP TIN (File Data Payload Extents)\n", C_GREEN, C_RESET);
	printf("    %s%sC%s : Khối CONTAINER / SIÊU DỮ LIỆU (B-Tree Leaves, Internal Nodes, VAG Roots)\n", C_YELLOW, C_BOLD, C_RESET);
	printf("    %s·%s : Khối CÒN TRỐNG (Free unallocated blocks)\n", C_DIM, C_RESET);
	printf("    %sS%s : Vùng Superblock & Bootstrap Reserve (Blocks 0..31)\n\n", C_CYAN, C_RESET);

	/* Raw fd for block inspection */
	int fd = open(dev_path, O_RDONLY);

	__u8 *block_types = malloc(VTX_VAG_NUM_BLOCKS);
	if (!block_types) {
		vtxfs_cli_close(&h, 0);
		if (fd >= 0) close(fd);
		return 1;
	}

	for (int vid = start_vag; vid < end_vag; vid++) {
		const __u8 *bitmap = h.fs->vag.bitmaps[vid];
		if (!bitmap) continue;
		__u64 start_pblk = vtx_cpu64(h.fs->vag.descs[vid].start_pblk);
		__u64 end_pblk = start_pblk + VTX_VAG_NUM_BLOCKS - 1;

		memset(block_types, TYPE_FREE, VTX_VAG_NUM_BLOCKS);

		/* Classify each block in this VAG */
		if (fd >= 0) {
			const size_t CHUNK_BLKS = 512;
			const size_t CHUNK_BYTES = CHUNK_BLKS * VTX_BLOCK_SIZE;
			__u8 *chunk_buf = malloc(CHUNK_BYTES);

			if (chunk_buf) {
				for (size_t c_start = 0; c_start < VTX_VAG_NUM_BLOCKS; c_start += CHUNK_BLKS) {
					size_t byte_s = c_start >> 3;
					size_t byte_e = (c_start + CHUNK_BLKS) >> 3;
					bool has_any_bit = false;
					for (size_t b = byte_s; b < byte_e; b++) {
						if (bitmap[b] != 0) {
							has_any_bit = true;
							break;
						}
					}
					if (!has_any_bit)
						continue;

					__u64 chunk_pblk = start_pblk + c_start;
					ssize_t n_read = pread(fd, chunk_buf, CHUNK_BYTES, (off_t)(chunk_pblk * VTX_BLOCK_SIZE));
					if (n_read <= 0) continue;

					size_t num_in_chunk = (size_t)n_read / VTX_BLOCK_SIZE;
					for (size_t b = 0; b < num_in_chunk; b++) {
						size_t blk_idx = c_start + b;
						int bit = (bitmap[blk_idx >> 3] >> (blk_idx & 7)) & 1;
						if (bit == 0) continue;

						const __u8 *b_bytes = chunk_buf + b * VTX_BLOCK_SIZE;
						__u32 node_m = 0;
						memcpy(&node_m, b_bytes + 48, sizeof(node_m));
						node_m = vtx_cpu32(node_m);

						__u32 vag_m = 0;
						memcpy(&vag_m, b_bytes + 16, sizeof(vag_m));
						vag_m = vtx_cpu32(vag_m);

						if (node_m == VTX_NODE_MAGIC_VAL) {
							__u8 level = b_bytes[54];
							block_types[blk_idx] = (level == 0) ? TYPE_CONTAINER_LEAF : TYPE_CONTAINER_NODE;
						} else if (vag_m == VTX_VAG_DIR_MAGIC_V) {
							block_types[blk_idx] = TYPE_CONTAINER_OTHER;
						} else if (memcmp(b_bytes + 32, "VTXCP21", 7) == 0 ||
						           memcmp(b_bytes + 32, "VTXFS21", 7) == 0 ||
						           memcmp(b_bytes + 32, "VDT1", 4) == 0) {
							block_types[blk_idx] = TYPE_CONTAINER_OTHER;
						} else {
							block_types[blk_idx] = TYPE_DATA;
						}
					}
				}
				free(chunk_buf);
			}
		}

		/* Compute statistics */
		size_t data_count = 0, leaf_count = 0, node_count = 0, other_meta_count = 0, free_count = 0;
		for (size_t i = 0; i < VTX_VAG_NUM_BLOCKS; i++) {
			switch (block_types[i]) {
			case TYPE_DATA: data_count++; break;
			case TYPE_CONTAINER_LEAF: leaf_count++; break;
			case TYPE_CONTAINER_NODE: node_count++; break;
			case TYPE_CONTAINER_OTHER: other_meta_count++; break;
			default: free_count++; break;
			}
		}
		size_t container_count = leaf_count + node_count + other_meta_count;
		double data_pct = (double)data_count / (double)VTX_VAG_NUM_BLOCKS * 100.0;
		double cont_pct = (double)container_count / (double)VTX_VAG_NUM_BLOCKS * 100.0;
		double free_pct = (double)free_count / (double)VTX_VAG_NUM_BLOCKS * 100.0;

		printf("----------------------------------------------------------------------------------\n");
		printf("  %sVAG #%d%s | Phạm vi vật lý: %sPBLK %llu .. %llu%s | %u blocks (128.00 MiB)\n",
		       C_CYAN, vid, C_RESET, C_BOLD, (unsigned long long)start_pblk, (unsigned long long)end_pblk, C_RESET, VTX_VAG_NUM_BLOCKS);
		printf("  ├─ %s■ Khối DỮ LIỆU (Data)%s     : %s%zu%s blks (%.2f%%) [%.2f MiB]\n",
		       C_GREEN, C_RESET, C_BOLD, data_count, C_RESET, data_pct, (double)(data_count * 4) / 1024.0);
		printf("  ├─ %s%sC Khối CONTAINER (Meta)%s   : %s%zu%s blks (%.2f%%) [%.2f MiB] (%zu Leaves, %zu Nodes, %zu System)\n",
		       C_YELLOW, C_BOLD, C_RESET, C_BOLD, container_count, C_RESET, cont_pct,
		       (double)(container_count * 4) / 1024.0, leaf_count, node_count, other_meta_count);
		printf("  └─ %s· Khối CÒN TRỐNG (Free)%s   : %s%zu%s blks (%.2f%%) [%.2f MiB]\n",
		       C_DIM, C_RESET, C_DIM, free_count, C_RESET, free_pct, (double)(free_count * 4) / 1024.0);
		printf("----------------------------------------------------------------------------------\n");

		/* Heatmap Grid */
		int blk_per_char = compact ? 32 : 16;
		printf("  Bản đồ nhiệt (%d blocks/ký tự, %d cột/hàng):\n", blk_per_char, cols);

		int num_cells = VTX_VAG_NUM_BLOCKS / blk_per_char;
		int row_cells = 0;
		__u64 cur_row_pblk = start_pblk;

		for (int c_idx = 0; c_idx < num_cells; c_idx++) {
			if (row_cells == 0) {
				printf("    %7llu |", (unsigned long long)cur_row_pblk);
			}

			int n_c = 0, n_d = 0;
			for (int b = 0; b < blk_per_char; b++) {
				__u8 t = block_types[c_idx * blk_per_char + b];
				if (t == TYPE_CONTAINER_LEAF || t == TYPE_CONTAINER_NODE || t == TYPE_CONTAINER_OTHER)
					n_c++;
				else if (t == TYPE_DATA)
					n_d++;
			}

			if (n_c > 0)
				printf("%s%sC%s", C_YELLOW, C_BOLD, C_RESET);
			else if (n_d > 0)
				printf("%s■%s", C_GREEN, C_RESET);
			else
				printf("%s·%s", C_DIM, C_RESET);

			row_cells++;
			if (row_cells == cols || c_idx == num_cells - 1) {
				__u64 end_row_pblk = cur_row_pblk + row_cells * blk_per_char - 1;
				printf("| %llu\n", (unsigned long long)end_row_pblk);
				cur_row_pblk += row_cells * blk_per_char;
				row_cells = 0;
			}
		}

		/* Contiguous ranges breakdown */
		printf("\n  %sBẢNG PHÂN BỐ DẢI KHỐI CHI TIẾT (CONTIGUOUS RANGE BREAKDOWN):%s\n", C_BOLD, C_RESET);
		printf("    ----------------------------------------------------------------------------------\n");
		printf("    %-26s %-14s %10s %12s  %s\n", "Phạm vi PBLK", "Loại khối", "Số khối", "Dung lượng", "Chi tiết nhận diện");
		printf("    ----------------------------------------------------------------------------------\n");

		struct range_entry {
			int cat;
			size_t start;
			size_t len;
		};
		struct range_entry *ranges = malloc(sizeof(struct range_entry) * VTX_VAG_NUM_BLOCKS);
		size_t n_ranges = 0;

		if (ranges) {
			int cur_cat = -1;
			size_t run_start = 0;
			size_t run_len = 0;

			for (size_t b = 0; b < VTX_VAG_NUM_BLOCKS; b++) {
				__u8 t = block_types[b];
				int cat = (t == TYPE_FREE) ? TYPE_FREE : ((t == TYPE_DATA) ? TYPE_DATA : TYPE_CONTAINER_OTHER);
				if (cat != cur_cat) {
					if (cur_cat != -1) {
						ranges[n_ranges].cat = cur_cat;
						ranges[n_ranges].start = run_start;
						ranges[n_ranges].len = run_len;
						n_ranges++;
					}
					cur_cat = cat;
					run_start = b;
					run_len = 1;
				} else {
					run_len++;
				}
			}
			if (cur_cat != -1) {
				ranges[n_ranges].cat = cur_cat;
				ranges[n_ranges].start = run_start;
				ranges[n_ranges].len = run_len;
				n_ranges++;
			}

			size_t limit = (max_ranges > 0 && (size_t)max_ranges < n_ranges) ? (size_t)max_ranges : n_ranges;
			for (size_t r = 0; r < limit; r++) {
				__u64 p_s = start_pblk + ranges[r].start;
				__u64 p_e = p_s + ranges[r].len - 1;
				char size_str[32];
				if (ranges[r].len < 256)
					snprintf(size_str, sizeof(size_str), "%zu KiB", ranges[r].len * 4);
				else
					snprintf(size_str, sizeof(size_str), "%.2f MiB", (double)(ranges[r].len * 4) / 1024.0);

				const char *cat_styled = "";
				const char *cat_color = "";
				const char *note = "";
				if (ranges[r].cat == TYPE_CONTAINER_OTHER) {
					cat_styled = "CONTAINER";
					cat_color = C_YELLOW;
					note = "Siêu dữ liệu hệ thống / B-Tree Node";
				} else if (ranges[r].cat == TYPE_DATA) {
					cat_styled = "DỮ LIỆU";
					cat_color = C_GREEN;
					note = "Dữ liệu tệp tin (File Data Extent)";
				} else {
					cat_styled = "CÒN TRỐNG";
					cat_color = C_DIM;
					note = "Dải trống (Unallocated Free Space)";
				}

				printf("    %8llu .. %-12llu %s%-14s%s %8zu blk %12s  %s%s%s\n",
				       (unsigned long long)p_s, (unsigned long long)p_e,
				       cat_color, cat_styled, C_RESET, ranges[r].len, size_str, C_DIM, note, C_RESET);
			}

			if (n_ranges > limit) {
				size_t rem_data = 0, rem_cont = 0, rem_free = 0;
				for (size_t r = limit; r < n_ranges; r++) {
					if (ranges[r].cat == TYPE_DATA) rem_data += ranges[r].len;
					else if (ranges[r].cat == TYPE_CONTAINER_OTHER) rem_cont += ranges[r].len;
					else rem_free += ranges[r].len;
				}
				printf("    ... và %zu dải khác (%s%zu khối dữ liệu%s, %s%zu khối container%s, %s%zu khối trống%s) ...\n",
				       n_ranges - limit, C_GREEN, rem_data, C_RESET, C_YELLOW, rem_cont, C_RESET, C_DIM, rem_free, C_RESET);
			}
			printf("    ----------------------------------------------------------------------------------\n\n");
			free(ranges);
		}
	}

	free(block_types);
	if (fd >= 0) close(fd);
	vtxfs_cli_close(&h, 0);
	return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: sys-index (System Reserved Area 128 MiB Catalog & Virtual View)
 * -------------------------------------------------------------------------- */
int cmd_sys_index(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs sys-index <thiết_bị_hoặc_image>\n");
		return 1;
	}
	const char *dev_path = argv[0];
	int cols = 64;

	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--cols=", 7) == 0) {
			cols = atoi(argv[i] + 7);
		}
	}
	if (cols <= 0 || cols > 128) cols = 64;

	int fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%sLỗi: Không thể mở '%s': %s%s\n", C_RED, dev_path, strerror(errno), C_RESET);
		return 1;
	}

	/* Read Superblock at PBLK 16 */
	struct vtx_super_block sb = {0};
	if (read_block_raw(fd, VTX_SUPER_PBLK_START, (__u8 *)&sb) != 0 ||
	    (memcmp(sb.hdr.magic, "VTXFS21\0", 8) != 0 && memcmp(sb.hdr.magic, "VTXFS21", 7) != 0)) {
		fprintf(stderr, "%sLỗi: Không tìm thấy Superblock VTXFS21 hợp lệ tại PBLK %llu trên %s!%s\n",
		        C_RED, (unsigned long long)VTX_SUPER_PBLK_START, dev_path, C_RESET);
		close(fd);
		return 1;
	}

	/* Read CPD ring (8..15) and find active CPD */
	int active_cpd_slot = -1;
	__u64 active_xid = 0;
	struct vtx_checkpoint_desc active_cpd = {0};

	for (size_t s = 0; s < VTXFS_CPD_RING_SLOTS; s++) {
		struct vtx_checkpoint_desc cpd = {0};
		if (read_block_raw(fd, VTX_CPD_PBLK_START + s, (__u8 *)&cpd) == 0) {
			if (memcmp(cpd.hdr.magic, "VTXCP21", 7) == 0 &&
			    memcmp(cpd.hdr.fsid_uuid, sb.hdr.fsid_uuid, 16) == 0) {
				__u64 x = vtx_cpu64(cpd.hdr.xid);
				if (active_cpd_slot == -1 || x > active_xid) {
					active_xid = x;
					active_cpd_slot = (int)s;
					active_cpd = cpd;
				}
			}
		}
	}

	/* Active VAG Root info */
	__u64 vag_root_pblk = (active_cpd_slot >= 0) ? vtx_cpu64(active_cpd.hdr.vag_dir_root.pblk) : 0;
	struct vtx_vag_dir_root vag_root = {0};
	bool has_vag_root = false;
	__u64 vag_leaf_pblk = 0;
	__u32 total_vags = 0;
	__u64 first_bm_pblk = 0;
	__u64 last_bm_pblk = 0;

	if (vag_root_pblk > 0 && read_block_raw(fd, vag_root_pblk, (__u8 *)&vag_root) == 0) {
		if (vtx_cpu32(vag_root.hdr.magic) == VTX_VAG_DIR_MAGIC_V) {
			has_vag_root = true;
			total_vags = (__u32)vtx_cpu64(vag_root.hdr.total_vags);
			vag_leaf_pblk = vtx_cpu64(vag_root.entries[0].start_pblk);
			if (vag_leaf_pblk > 0 && total_vags > 0) {
				first_bm_pblk = vag_leaf_pblk + 16ULL;
				last_bm_pblk = first_bm_pblk + total_vags - 1;
			}
		}
	}

	printf("==================================================================================\n");
	printf("  %sVTXFS v2.1 — BẢNG CHỈ MỤC & ĐỊA HÌNH VÙNG HỆ THỐNG (SYSTEM INDEX CATALOG)%s\n", C_BOLD, C_RESET);
	printf("  %sThiết bị: %s | Phạm vi: PBLK 0 .. 32767 (128.00 MiB / 32,768 blocks 4KB)%s\n",
	       C_DIM, dev_path, C_RESET);
	printf("==================================================================================\n\n");

	/* 1. Header Identifiers */
	printf("%s1. THÔNG SỐ VÙNG BẢO VỆ HỆ THỐNG (SYSTEM RESERVED METRIC):%s\n", C_CYAN, C_RESET);
	printf("  Nhãn hệ thống (Label)    : %s%s%s\n", C_BOLD, sb.hdr.label[0] ? sb.hdr.label : "(none)", C_RESET);
	printf("  Mã FSID UUID             : ");
	vtxfs_cli_print_uuid(sb.hdr.fsid_uuid);
	printf("\n  Kích thước vùng Hệ thống : 32768 blocks (128.00 MiB) [PBLK 0 .. 32767]\n");
	printf("  Điểm bắt đầu VAG 0       : PBLK %llu (Khối cấp phát dữ liệu đầu tiên)\n",
	       (unsigned long long)vtx_cpu64(sb.hdr.first_alloc_pblk));
	printf("  Checkpoint Active hiện tại: Slot %d (PBLK %llu, Committed XID %llu)\n",
	       active_cpd_slot, (unsigned long long)(VTX_CPD_PBLK_START + active_cpd_slot),
	       (unsigned long long)active_xid);
	printf("  Thư mục VAG Root Active  : PBLK %llu (XID %llu, Quản lý %u VAGs)\n",
	       (unsigned long long)vag_root_pblk,
	       (unsigned long long)(has_vag_root ? vtx_cpu64(vag_root.hdr.xid) : 0),
	       total_vags);
	printf("  Neo dự phòng cấp 2 (Mirror 0): PBLK %llu (Vị trí 64.00 MiB)\n\n",
	       (unsigned long long)VTX_MIRROR0_PBLK);

	/* 2. System Index Table */
	printf("%s2. BẢNG MỤC LỤC CHỈ MỤC CÁC THÀNH PHẦN HỆ THỐNG (SYSTEM INDEX TABLE):%s\n", C_CYAN, C_RESET);
	printf("  ------------------------------------------------------------------------------------------------------\n");
	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "Thành phần hệ thống", "Phạm vi PBLK", "Dung lượng", "Trạng thái", "Chi tiết nhận diện kỹ thuật");
	printf("  ------------------------------------------------------------------------------------------------------\n");

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "BOOT_RESERVED", "0 .. 3", "16 KiB", "KHỞI ĐỘNG", "Neo khởi động Partition MBR/GPT Protection");

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "DEVICE_TABLE (VDT1)", "4 .. 7", "16 KiB", "ACTIVE", "Bảng ánh xạ cấu hình thiết bị & UUID");

	char cpd_desc[128];
	snprintf(cpd_desc, sizeof(cpd_desc), "8 Slots vòng tròn (Slot %d ACTIVE - XID %llu, BLAKE3-256)",
	         active_cpd_slot, (unsigned long long)active_xid);
	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "CPD_PRIMARY_RING", "8 .. 15", "32 KiB", "ĐA THẾ HỆ", cpd_desc);

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "SUPERBLOCK_RING", "16 .. 31", "64 KiB", "ACTIVE", "16 Slots (Slot 0 Master Gen 1, Slot 1 Backup)");

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "WAL_JOURNAL_RING", "32 .. 287", "1.00 MiB", "FAST-PATH", "256 blocks vòng ghi nhật ký bền vững");

	char vag_r_str[32], vag_l_str[32], vag_b_str[32];
	snprintf(vag_r_str, sizeof(vag_r_str), "%llu", (unsigned long long)vag_root_pblk);
	snprintf(vag_l_str, sizeof(vag_l_str), "%llu .. %llu",
	         (unsigned long long)vag_leaf_pblk, (unsigned long long)(vag_leaf_pblk + 15));
	snprintf(vag_b_str, sizeof(vag_b_str), "%llu .. %llu",
	         (unsigned long long)first_bm_pblk, (unsigned long long)last_bm_pblk);

	char vag_r_desc[128];
	snprintf(vag_r_desc, sizeof(vag_r_desc), "Thư mục gốc 2 tầng VAG Directory Root (XID %llu, %u VAGs)",
	         (unsigned long long)(has_vag_root ? vtx_cpu64(vag_root.hdr.xid) : 0), total_vags);
	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "ACTIVE_VAG_ROOT", vag_r_str, "4 KiB", "ACTIVE", vag_r_desc);

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "ACTIVE_VAG_LEAVES", vag_l_str, "64 KiB", "ACTIVE", "16 blocks lá mô tả cấu trúc 2048 VAGs");

	char vag_b_desc[128];
	snprintf(vag_b_desc, sizeof(vag_b_desc), "%u Bitmap blocks cấp phát (4KB/VAG, CRC32C Verified)", total_vags);
	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "ACTIVE_VAG_BITMAPS", vag_b_str, "924 KiB", "ACTIVE", vag_b_desc);

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "IP_BOOT_HISTORY", "288 .. 4383", "16.00 MiB", "COW SLOTS", "8 slots động 2 MiB xoay vòng lưu vết VAG Directory");

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "BOOT_MIRROR_0", "16384 .. 16415", "128 KiB", "BẢO VỆ KÉP", "Bản sao đối xứng khẩn cấp tại 64 MiB (Dev, CPD, Super)");

	printf("  %-22s %-16s %-10s %-12s %s\n",
	       "SYSTEM_EXPANSION", "4384 .. 32767", "110.88 MiB", "DỰ TRỮ", "Không gian trống sẵn sàng mở rộng tính năng mới");
	printf("  ------------------------------------------------------------------------------------------------------\n\n");

	/* 3. High-Density Heatmap for System 128 MiB (32,768 blocks) */
	printf("%s3. BẢN ĐỒ NHIỆT KHỐI HỆ THỐNG (SYSTEM HEATMAP - 32 blocks / ký tự, 64 cột / hàng):%s\n", C_CYAN, C_RESET);
	printf("  %sKý hiệu:%s  %sR%s: Boot  %sD%s: DevTable  %sC%s: CPD  %sS%s: Super  %sW%s: WAL  %sV%s: VAG-Dir  %sB%s: Bitmap  %sM%s: Mirror  %s·%s: Dự trữ\n\n",
	       C_DIM, C_RESET,
	       C_CYAN, C_RESET, C_BLUE, C_RESET, C_GREEN, C_RESET, C_YELLOW, C_RESET,
	       C_MAGENTA, C_RESET, C_CYAN, C_RESET, C_GREEN, C_RESET, C_YELLOW, C_RESET, C_DIM, C_RESET);

	const int blk_per_char = 32;
	const int num_cells = 32768 / blk_per_char; /* 1024 cells */
	int row_cells = 0;
	__u64 cur_row_pblk = 0;

	for (int c = 0; c < num_cells; c++) {
		if (row_cells == 0) {
			printf("    %5llu |", (unsigned long long)cur_row_pblk);
		}

		__u64 cell_start = (__u64)c * blk_per_char;
		__u64 cell_end = cell_start + blk_per_char - 1;

		/* Classify cell */
		if (cell_start <= 3) {
			printf("%sR%s", C_CYAN, C_RESET);
		} else if (cell_start >= 4 && cell_start <= 7) {
			printf("%sD%s", C_BLUE, C_RESET);
		} else if (cell_start >= 8 && cell_start <= 15) {
			printf("%sC%s", C_GREEN, C_RESET);
		} else if (cell_start >= 16 && cell_start <= 31) {
			printf("%sS%s", C_YELLOW, C_RESET);
		} else if (cell_start >= 32 && cell_start <= 287) {
			printf("%sW%s", C_MAGENTA, C_RESET);
		} else if (cell_start >= 16384 && cell_start <= 16415) {
			printf("%sM%s", C_YELLOW, C_RESET);
		} else if (vag_root_pblk >= cell_start && vag_root_pblk <= cell_end) {
			printf("%sV%s", C_CYAN, C_RESET);
		} else if (vag_leaf_pblk >= cell_start && (vag_leaf_pblk + 15) <= cell_end) {
			printf("%sV%s", C_CYAN, C_RESET);
		} else if (first_bm_pblk <= cell_end && last_bm_pblk >= cell_start) {
			printf("%sB%s", C_GREEN, C_RESET);
		} else {
			printf("%s·%s", C_DIM, C_RESET);
		}

		row_cells++;
		if (row_cells == cols || c == num_cells - 1) {
			__u64 end_row_pblk = cur_row_pblk + row_cells * blk_per_char - 1;
			printf("| %llu\n", (unsigned long long)end_row_pblk);
			cur_row_pblk += row_cells * blk_per_char;
			row_cells = 0;
		}
	}
	printf("\n");

	close(fd);
	return 0;
}

/* --------------------------------------------------------------------------
 * bitmap --summary: per-VAG allocation overview in one pass
 * (word-wise scan via vtx_vag_free_stats instead of N separate invocations)
 * -------------------------------------------------------------------------- */
static int bitmap_summary(const char *dev_path)
{
	struct vtxfs_handle h;
	int rc = vtxfs_cli_open(&h, dev_path, 0);
	if (rc != 0) {
		fprintf(stderr, "vtxfs: Không thể mở '%s': %s\n", dev_path, strerror(-rc));
		return 1;
	}

	__u64 tot_used = 0, tot_free = 0;
	printf("VAG   %10s %10s %7s %10s %6s\n", "used", "free", "runs", "longest", "frag%");
	for (__u32 v = 0; v < h.fs->vag.total_vags; v++) {
		const __u8 *bm = h.fs->vag.bitmaps[v];
		__u32 runs = 0, longest = 0, used = 0;

		if (!bm || vtx_vag_free_stats(&h.fs->vag, v, &runs, &longest) != 0)
			continue;
		for (__u32 i = 0; i < VTX_VAG_NUM_BLOCKS / 8; i++)
			used += (__u32)__builtin_popcount(bm[i]);
		__u32 fr = VTX_VAG_NUM_BLOCKS - used;
		/* fragmentation: share of free space NOT in the longest free run */
		unsigned frag = fr ? (unsigned)(100ULL * (fr - longest) / fr) : 0U;
		printf("%-5u %10u %10u %7u %10u %5u%%\n", v, used, fr, runs, longest, frag);
		tot_used += used;
		tot_free += fr;
	}
	printf("TOTAL %10llu %10llu  (%u VAGs x %u blocks, xid=%llu)\n",
	       (unsigned long long)tot_used, (unsigned long long)tot_free,
	       h.fs->vag.total_vags, VTX_VAG_NUM_BLOCKS,
	       (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.xid));
	vtxfs_cli_close(&h, 0);
	return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: bitmap (exports machine-readable VTXFS_BITMAP_V1)
 * -------------------------------------------------------------------------- */
int cmd_bitmap(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs bitmap <thiết_bị_hoặc_image> [vag_id] [offset [count]]\n"
				"          vtxfs bitmap <thiết_bị_hoặc_image> --summary\n");
		return 1;
	}
	const char *dev_path = argv[0];
	unsigned long vag_id = 0;
	if (argc >= 2 && (!strcmp(argv[1], "--summary") || !strcmp(argv[1], "-a"))) {
		return bitmap_summary(dev_path);
	}
	if (argc >= 2) {
		vag_id = strtoul(argv[1], NULL, 10);
	}

	struct vtxfs_handle h;
	int rc = vtxfs_cli_open(&h, dev_path, 0);
	if (rc != 0) {
		fprintf(stderr, "vtxfs: Không thể mở '%s': %s\n", dev_path, strerror(-rc));
		return 1;
	}

	if (vag_id >= h.fs->vag.total_vags) {
		fprintf(stderr, "vtxfs: vag_id %lu vượt quá %u\n", vag_id, h.fs->vag.total_vags);
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	const __u8 *bitmap = h.fs->vag.bitmaps[vag_id];
	if (!bitmap) {
		fprintf(stderr, "vtxfs: Bitmap VAG %lu không tồn tại\n", vag_id);
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	__u32 allocated = 0;
	for (__u32 i = 0; i < VTX_VAG_NUM_BLOCKS; i++) {
		allocated += (bitmap[i >> 3] >> (i & 7U)) & 1U;
	}
	__u32 free_blocks = VTX_VAG_NUM_BLOCKS - allocated;
	__u64 start_pblk = vtx_cpu64(h.fs->vag.descs[vag_id].start_pblk);

	printf("format=VTXFS_BITMAP_V1\n");
	printf("vag=%lu\n", vag_id);
	printf("total_vags=%u\n", h.fs->vag.total_vags);
	printf("vag_blocks=%u\n", VTX_VAG_NUM_BLOCKS);
	printf("block_bytes=%u\n", VTX_BLOCK_SIZE);
	printf("start_pblk=%llu\n", (unsigned long long)start_pblk);
	printf("start_offset=0\n");
	printf("count=%u\n", VTX_VAG_NUM_BLOCKS);
	printf("allocated=%u\n", allocated);
	printf("free_blocks=%u\n", free_blocks);
	printf("xid=%llu\n", (unsigned long long)vtx_cpu64(h.fs->cp_blk.hdr.xid));
	printf("bitmap_hex=");
	for (size_t i = 0; i < (VTX_VAG_NUM_BLOCKS + 7) / 8; i++) {
		printf("%02x", bitmap[i]);
	}
	printf("\n");

	vtxfs_cli_close(&h, 0);
	return 0;
}
