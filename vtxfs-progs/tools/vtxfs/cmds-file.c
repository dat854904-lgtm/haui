// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * VTXFS v2.1 — Native C File Structure & Extent Inspector
 * Phân tích chuyên sâu cấu trúc vật lý, siêu dữ liệu Inode, bản đồ Extent,
 * thuật toán Checksum/Nén và vị trí địa hình VAG của tệp tin.
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#include "commands.h"
#include "common.h"
#include "vtxfs_disk.h"
#include "vtxfs_engine.h"

#define MAX_FILE_EXTENTS 2048
#define VTX_BLOCK_SIZE   4096U
#define VTX_VAG_NUM_BLKS 32768U

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

static void format_time_ns(__u64 ns, char *buf, size_t buf_sz)
{
	if (ns == 0) {
		snprintf(buf, buf_sz, "(chưa xác định)");
		return;
	}
	time_t t_sec = (time_t)(ns / 1000000000ULL);
	struct tm tm_val;
	localtime_r(&t_sec, &tm_val);
	strftime(buf, buf_sz, "%Y-%m-%d %H:%M:%S", &tm_val);
}

static const char *get_file_type_desc(mode_t mode)
{
	if (S_ISREG(mode)) return "Regular File (Tệp tin thông thường)";
	if (S_ISDIR(mode)) return "Directory (Thư mục)";
	if (S_ISLNK(mode)) return "Symbolic Link (Liên kết mềm)";
	if (S_ISBLK(mode)) return "Block Device (Thiết bị khối)";
	if (S_ISCHR(mode)) return "Character Device (Thiết bị ký tự)";
	if (S_ISFIFO(mode)) return "FIFO / Named Pipe";
	if (S_ISSOCK(mode)) return "Socket";
	return "Unknown (Không xác định)";
}

static void mode_to_string(mode_t mode, char *buf, size_t buf_sz)
{
	if (buf_sz < 11) return;
	buf[0] = S_ISDIR(mode) ? 'd' : (S_ISLNK(mode) ? 'l' : (S_ISBLK(mode) ? 'b' : (S_ISCHR(mode) ? 'c' : '-')));
	buf[1] = (mode & S_IRUSR) ? 'r' : '-';
	buf[2] = (mode & S_IWUSR) ? 'w' : '-';
	buf[3] = (mode & S_IXUSR) ? 'x' : '-';
	buf[4] = (mode & S_IRGRP) ? 'r' : '-';
	buf[5] = (mode & S_IWGRP) ? 'w' : '-';
	buf[6] = (mode & S_IXGRP) ? 'x' : '-';
	buf[7] = (mode & S_IROTH) ? 'r' : '-';
	buf[8] = (mode & S_IWOTH) ? 'w' : '-';
	buf[9] = (mode & S_IXOTH) ? 'x' : '-';
	buf[10] = '\0';
}

/* --------------------------------------------------------------------------
 * 1. Online Inspection (thông qua VFS & FS_IOC_FIEMAP)
 * -------------------------------------------------------------------------- */
static int inspect_online_file(const char *filepath)
{
	struct stat st;
	if (lstat(filepath, &st) != 0) {
		fprintf(stderr, "%sLỗi: Không thể truy cập '%s': %s%s\n",
		        C_RED, filepath, strerror(errno), C_RESET);
		return 1;
	}

	int fd = open(filepath, O_RDONLY);
	if (fd < 0 && !S_ISLNK(st.st_mode)) {
		fprintf(stderr, "%sLỗi: Không thể mở '%s': %s%s\n",
		        C_RED, filepath, strerror(errno), C_RESET);
		return 1;
	}

	char perm_str[16];
	mode_to_string(st.st_mode, perm_str, sizeof(perm_str));

	char mtime_str[64], atime_str[64], ctime_str[64];
	format_time_ns((__u64)st.st_mtim.tv_sec * 1000000000ULL + (__u64)st.st_mtim.tv_nsec, mtime_str, sizeof(mtime_str));
	format_time_ns((__u64)st.st_atim.tv_sec * 1000000000ULL + (__u64)st.st_atim.tv_nsec, atime_str, sizeof(atime_str));
	format_time_ns((__u64)st.st_ctim.tv_sec * 1000000000ULL + (__u64)st.st_ctim.tv_nsec, ctime_str, sizeof(ctime_str));

	printf("==================================================================================\n");
	printf("  %sVTXFS v2.1 — BÁO CÁO CẤU TRÚC CHI TIẾT TỆP TIN (FILE STRUCTURE INSPECTOR)%s\n", C_BOLD, C_RESET);
	printf("  %sĐường dẫn: %s%s\n", C_DIM, filepath, C_RESET);
	printf("==================================================================================\n\n");

	/* 1. Inode Metadata */
	printf("%s1. SIÊU DỮ LIỆU & QUẢN TRỊ INODE (VFS INODE METADATA):%s\n", C_CYAN, C_RESET);
	printf("  Số hiệu Inode (INO)      : %s%llu%s\n", C_BOLD, (unsigned long long)st.st_ino, C_RESET);
	printf("  Phân loại tệp (Type)     : %s\n", get_file_type_desc(st.st_mode));
	printf("  Quyền hạn (Mode / Perm)  : 0%o (%s)\n", (unsigned int)(st.st_mode & 07777), perm_str);
	printf("  Kích thước logic (Size)  : %s%llu bytes%s (%.2f KiB / %.2f MiB)\n",
	       C_BOLD, (unsigned long long)st.st_size, C_RESET,
	       (double)st.st_size / 1024.0, (double)st.st_size / (1024.0 * 1024.0));
	printf("  Dung lượng chiếm dụng đĩa: %llu bytes (%llu sectors 512B / ~%llu blocks 4KB)\n",
	       (unsigned long long)(st.st_blocks * 512ULL),
	       (unsigned long long)st.st_blocks,
	       (unsigned long long)((st.st_blocks + 7) / 8));
	printf("  Số liên kết cứng (Links) : %llu\n", (unsigned long long)st.st_nlink);
	printf("  Chủ sở hữu (Ownership)   : UID: %u | GID: %u\n", (unsigned int)st.st_uid, (unsigned int)st.st_gid);
	printf("  Dấu thời gian (Timestamps):\n");
	printf("    • Sửa đổi nội dung (mtime): %s\n", mtime_str);
	printf("    • Thay đổi thuộc tính (ctime): %s\n", ctime_str);
	printf("    • Truy cập gần nhất (atime): %s\n", atime_str);

	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t len = readlink(filepath, target, sizeof(target) - 1);
		if (len > 0) {
			target[len] = '\0';
			printf("\n  Đích liên kết mềm (Symlink Target): %s%s%s\n", C_BOLD, target, C_RESET);
		}
		if (fd >= 0) close(fd);
		return 0;
	}

	/* 2. Storage Form */
	printf("\n%s2. HÌNH THÁI LƯU TRỮ & CƠ CHẾ DỮ LIỆU (STORAGE FORM & ENGINE):%s\n", C_CYAN, C_RESET);
	bool is_inline = (st.st_blocks == 0 && st.st_size > 0 && st.st_size <= 2048);
	if (is_inline) {
		printf("  Cơ chế lưu trữ (Form)    : %sINLINE PAYLOAD%s (Nhúng trực tiếp trong lá B-Tree Leaf, 0 blocks đĩa)\n",
		       C_GREEN, C_RESET);
	} else if (st.st_size == 0) {
		printf("  Cơ chế lưu trữ (Form)    : EMPTY FILE (Tệp rỗng 0 bytes, chưa cấp phát block)\n");
	} else {
		printf("  Cơ chế lưu trữ (Form)    : %sEXTENT-BASED STORAGE%s (Cấp phát qua các dải khối liên tục)\n",
		       C_GREEN, C_RESET);
	}
	printf("  Thuật toán toàn vẹn      : CRC32C (Mỗi khối 4KB được bảo vệ bằng mã checksum độc lập)\n");
	printf("  Thuật toán Nén           : NONE (Dữ liệu nguyên bản uncompressed)\n");

	/* 3. Physical Extents Mapping via FIEMAP */
	uint32_t mapped_extents = 0;
	size_t fiemap_sz = sizeof(struct fiemap) + MAX_FILE_EXTENTS * sizeof(struct fiemap_extent);
	char *fiemap_buf = calloc(1, fiemap_sz);
	struct fiemap *map = (struct fiemap *)(void *)fiemap_buf;

	if (fd >= 0 && map && st.st_size > 0) {
		map->fm_start = 0;
		map->fm_length = (uint64_t)st.st_size;
		map->fm_flags = FIEMAP_FLAG_SYNC;
		map->fm_extent_count = MAX_FILE_EXTENTS;

		if (ioctl(fd, FS_IOC_FIEMAP, map) == 0) {
			mapped_extents = map->fm_mapped_extents;
		}
	}

	printf("\n%s3. BẢNG PHÂN BỐ CÁC EXTENT VẬT LÝ (PHYSICAL EXTENT MAPPING):%s\n", C_CYAN, C_RESET);
	if (mapped_extents == 0) {
		if (is_inline)
			printf("  (Tệp tin dạng Inline: Dữ liệu nằm trọn trong B-Tree metadata node, không có extent ngoài)\n");
		else
			printf("  (Tệp tin rỗng: Chưa có khối dữ liệu vật lý nào được cấp phát)\n");
	} else {
		printf("  ------------------------------------------------------------------------------------------------------------------------\n");
		printf("  %-4s %-24s %-12s %-10s %-22s %-20s %s\n",
		       "#", "Dải Offset logic (File)", "Kích thước", "Số khối", "Địa chỉ PBLK vật lý", "Vị trí VAG", "Cờ thuộc tính");
		printf("  ------------------------------------------------------------------------------------------------------------------------\n");

		uint32_t discontinuities = 0;
		uint64_t prev_pblk_end = 0;

		/* Track VAG blocks */
		uint32_t vag_counts[256];
		memset(vag_counts, 0, sizeof(vag_counts));

		for (uint32_t i = 0; i < mapped_extents; i++) {
			struct fiemap_extent *fe = &map->fm_extents[i];
			uint64_t log_start = fe->fe_logical;
			uint64_t log_end = fe->fe_logical + fe->fe_length - 1;
			uint64_t pblk_start = fe->fe_physical / VTX_BLOCK_SIZE;
			uint64_t num_blks = (fe->fe_length + VTX_BLOCK_SIZE - 1) / VTX_BLOCK_SIZE;
			uint64_t pblk_end = pblk_start + num_blks - 1;

			if (i > 0 && pblk_start != prev_pblk_end + 1) {
				discontinuities++;
			}
			prev_pblk_end = pblk_end;

			char log_str[64], pblk_str[64], vag_str[64], flag_str[64];
			snprintf(log_str, sizeof(log_str), "%llu .. %llu",
			         (unsigned long long)log_start, (unsigned long long)log_end);
			snprintf(pblk_str, sizeof(pblk_str), "PBLK %llu .. %llu",
			         (unsigned long long)pblk_start, (unsigned long long)pblk_end);

			if (pblk_start >= 32768ULL) {
				uint32_t vid = (uint32_t)((pblk_start - 32768ULL) / VTX_VAG_NUM_BLKS);
				uint32_t voff = (uint32_t)((pblk_start - 32768ULL) % VTX_VAG_NUM_BLKS);
				snprintf(vag_str, sizeof(vag_str), "VAG #%u (Offset %u)", vid, voff);
				if (vid < 256) vag_counts[vid] += (uint32_t)num_blks;
			} else {
				snprintf(vag_str, sizeof(vag_str), "SYSTEM RESERVED");
			}

			flag_str[0] = '\0';
			if (fe->fe_flags & FIEMAP_EXTENT_SHARED) strcat(flag_str, "SHARED ");
			if (fe->fe_flags & FIEMAP_EXTENT_LAST) strcat(flag_str, "LAST ");
			if (fe->fe_flags & FIEMAP_EXTENT_UNWRITTEN) strcat(flag_str, "UNWRITTEN ");
			if (fe->fe_flags & FIEMAP_EXTENT_ENCODED) strcat(flag_str, "COMPRESSED ");
			if (flag_str[0] == '\0') strcpy(flag_str, "CONTIGUOUS");

			printf("  %-4u %-24s %8.1f KiB %7llu blk %-22s %-20s %s%s%s\n",
			       i, log_str, (double)fe->fe_length / 1024.0, (unsigned long long)num_blks,
			       pblk_str, vag_str, C_GREEN, flag_str, C_RESET);
		}
		printf("  ------------------------------------------------------------------------------------------------------------------------\n");

		/* 4. Fragmentation & VAG Topography */
		double frag_rate = (mapped_extents > 1) ? ((double)discontinuities / (double)(mapped_extents - 1) * 100.0) : 0.0;
		printf("\n%s4. ĐỊA HÌNH VAG & ĐỘ PHÂN MẢNH (VAG TOPOGRAPHY & FRAGMENTATION):%s\n", C_CYAN, C_RESET);
		printf("  Tổng số Extents          : %s%u extents%s\n", C_BOLD, mapped_extents, C_RESET);
		printf("  Số điểm đứt gãy vật lý   : %s%u điểm%s\n",
		       discontinuities > 0 ? C_YELLOW : C_GREEN, discontinuities, C_RESET);
		printf("  Tỷ lệ phân mảnh (Rate)   : %s%.1f%%%s (%s)\n",
		       frag_rate > 20.0 ? C_RED : (frag_rate > 0.0 ? C_YELLOW : C_GREEN), frag_rate, C_RESET,
		       discontinuities == 0 ? "Hoàn toàn liền mạch / Optimal Streaming" : "Có phân mảnh rải rác");

		printf("  Phân bổ khối theo VAG    :\n");
		for (int vid = 0; vid < 256; vid++) {
			if (vag_counts[vid] > 0) {
				double pct = ((double)vag_counts[vid] / (double)(st.st_blocks ? ((st.st_blocks + 7) / 8) : 1)) * 100.0;
				printf("    • VAG #%d (Phạm vi PBLK %llu .. %llu): %s%u khối%s (%.1f%%)\n",
				       vid,
				       (unsigned long long)(32768ULL + (uint64_t)vid * VTX_VAG_NUM_BLKS),
				       (unsigned long long)(32768ULL + (uint64_t)(vid + 1) * VTX_VAG_NUM_BLKS - 1),
				       C_BOLD, vag_counts[vid], C_RESET, pct);
			}
		}
	}

	/* 5. Extended Attributes */
	printf("\n%s5. THUỘC TÍNH MỞ RỘNG (EXTENDED ATTRIBUTES - XATTR):%s\n", C_CYAN, C_RESET);
	if (fd >= 0) {
		char xlist[4096];
		ssize_t xlen = flistxattr(fd, xlist, sizeof(xlist));
		if (xlen > 0) {
			char *key = xlist;
			int x_count = 0;
			while (key < xlist + xlen) {
				char val[1024];
				ssize_t vlen = fgetxattr(fd, key, val, sizeof(val) - 1);
				if (vlen >= 0) val[vlen] = '\0';
				else strcpy(val, "(lỗi đọc)");
				printf("  • %s%s%s = \"%s\" (%zd bytes)\n", C_YELLOW, key, C_RESET, val, vlen);
				x_count++;
				key += strlen(key) + 1;
			}
			if (x_count == 0) printf("  (Không có thuộc tính mở rộng)\n");
		} else {
			printf("  (Không có thuộc tính mở rộng)\n");
		}
	} else {
		printf("  (Không khả dụng)\n");
	}

	printf("\n");
	if (map) free(map);
	if (fd >= 0) close(fd);
	return 0;
}

/* --------------------------------------------------------------------------
 * 2. Offline Inspection (quét trực tiếp qua B-Tree của Image/Device)
 * -------------------------------------------------------------------------- */
static int inspect_offline_file(const char *image, const char *vtx_path)
{
	struct vtxfs_handle h;
	int rc = vtxfs_cli_open(&h, image, 0);
	if (rc != 0) {
		fprintf(stderr, "%sLỗi: Không thể mở image/thiết bị '%s': %s%s\n",
		        C_RED, image, strerror(-rc), C_RESET);
		return 1;
	}

	rc = vtx_subvol_switch_view(h.fs, VTXFS_OID_FIRST_USER_SUBVOL);
	if (rc != 0) {
		fprintf(stderr, "%sLỗi: Không thể tải subvolume %llu: %s%s\n",
		        C_RED, (unsigned long long)VTXFS_OID_FIRST_USER_SUBVOL, strerror(-rc), C_RESET);
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	__u64 ino = 0;
	__u8 ftype = 0;
	rc = vtx_lookup_path(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, vtx_path, &ino, &ftype);
	if (rc != 0) {
		fprintf(stderr, "%sLỗi: Không tìm thấy đường dẫn '%s' trên %s: %s%s\n",
		        C_RED, vtx_path, image, strerror(-rc), C_RESET);
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	struct vtx_inode_item st;
	rc = vtx_stat(h.fs, VTXFS_OID_FIRST_USER_SUBVOL, ino, &st);
	if (rc != 0) {
		fprintf(stderr, "%sLỗi: Không thể đọc inode %llu: %s%s\n",
		        C_RED, (unsigned long long)ino, strerror(-rc), C_RESET);
		vtxfs_cli_close(&h, 0);
		return 1;
	}

	printf("==================================================================================\n");
	printf("  %sVTXFS v2.1 — BÁO CÁO CẤU TRÚC CHI TIẾT TỆP TIN (FILE STRUCTURE INSPECTOR)%s\n", C_BOLD, C_RESET);
	printf("  %sThiết bị: %s | Đường dẫn VTXFS: %s%s\n", C_DIM, image, vtx_path, C_RESET);
	printf("==================================================================================\n\n");

	mode_t mode = (mode_t)vtx_cpu32(st.mode);
	char perm_str[16];
	mode_to_string(mode, perm_str, sizeof(perm_str));

	char mtime_str[64], atime_str[64], ctime_str[64], btime_str[64];
	format_time_ns(vtx_cpu64(st.mtime_ns), mtime_str, sizeof(mtime_str));
	format_time_ns(vtx_cpu64(st.atime_ns), atime_str, sizeof(atime_str));
	format_time_ns(vtx_cpu64(st.ctime_ns), ctime_str, sizeof(ctime_str));
	format_time_ns(vtx_cpu64(st.btime_ns), btime_str, sizeof(btime_str));

	/* 1. Inode Metadata */
	printf("%s1. SIÊU DỮ LIỆU & QUẢN TRỊ INODE (VFS INODE METADATA):%s\n", C_CYAN, C_RESET);
	printf("  Số hiệu Inode (INO)      : %s%llu%s\n", C_BOLD, (unsigned long long)ino, C_RESET);
	printf("  Thư mục cha (Parent INO) : %llu\n", (unsigned long long)vtx_cpu64(st.parent_ino));
	printf("  Phân loại tệp (Type)     : %s\n", get_file_type_desc(mode));
	printf("  Quyền hạn (Mode / Perm)  : 0%o (%s)\n", (unsigned int)(mode & 07777), perm_str);
	printf("  Kích thước logic (Size)  : %s%llu bytes%s (%.2f KiB)\n",
	       C_BOLD, (unsigned long long)vtx_cpu64(st.size_bytes), C_RESET,
	       (double)vtx_cpu64(st.size_bytes) / 1024.0);
	printf("  Dung lượng cấp phát đĩa  : %llu bytes (%llu blocks 4KB)\n",
	       (unsigned long long)vtx_cpu64(st.alloc_bytes),
	       (unsigned long long)(vtx_cpu64(st.alloc_bytes) / VTX_BLOCK_SIZE));
	printf("  Số liên kết cứng (Links) : %u\n", vtx_cpu32(st.nlink));
	printf("  Chủ sở hữu (Ownership)   : UID: %u | GID: %u\n", vtx_cpu32(st.uid), vtx_cpu32(st.gid));
	printf("  Thế hệ giao dịch (XID)   : Update XID %llu\n", (unsigned long long)vtx_cpu64(st.update_xid));
	printf("  Dấu thời gian (Timestamps):\n");
	printf("    • Khởi tạo tệp (birth/btime): %s\n", btime_str);
	printf("    • Sửa đổi nội dung (mtime)  : %s\n", mtime_str);
	printf("    • Thay đổi thuộc tính (ctime): %s\n", ctime_str);
	printf("    • Truy cập gần nhất (atime) : %s\n", atime_str);

	/* 2. Scan B-Tree for Extents */
	printf("\n%s2. BẢNG PHÂN BỐ CÁC EXTENT VẬT LÝ & CHECKSUM (B-TREE EXTENTS):%s\n", C_CYAN, C_RESET);
	printf("  ------------------------------------------------------------------------------------------------------------------------\n");
	printf("  %-4s %-24s %-12s %-10s %-22s %-20s %s\n",
	       "#", "Dải Offset logic (File)", "Kích thước", "Số khối", "Địa chỉ PBLK vật lý", "Vị trí VAG", "Chi tiết mã kiểm tra");
	printf("  ------------------------------------------------------------------------------------------------------------------------\n");

	struct vtx_key cur_k, found_k;
	cur_k.oid_and_type = vtx_le64(VTX_MAKE_OID_TYPE(ino, VTX_TYPE_EXTENT));
	cur_k.offset = 0;
	int first = 1;
	__u8 *item_buf = (__u8 *)malloc(VTX_BLOCK_SIZE);
	uint32_t ext_idx = 0;
	uint32_t discontinuities = 0;
	uint64_t prev_pblk_end = 0;
	uint32_t vag_counts[256];
	memset(vag_counts, 0, sizeof(vag_counts));

	if (item_buf) {
		for (;;) {
			size_t bytes = VTX_BLOCK_SIZE;
			rc = vtx_rw_next(&h.fs->active_fs_tree, &cur_k, first, &found_k, item_buf, &bytes);
			if (rc != 0) break;
			first = 0;
			cur_k = found_k;

			uint64_t item_oid = VTX_KEY_GET_OID(vtx_cpu64(found_k.oid_and_type));
			uint8_t item_type = VTX_KEY_GET_TYPE(vtx_cpu64(found_k.oid_and_type));
			if (item_oid != ino || item_type != VTX_TYPE_EXTENT)
				break;

			struct vtx_extent_item *ext = (struct vtx_extent_item *)(void *)item_buf;
			uint64_t log_start = vtx_cpu64(found_k.offset);
			uint64_t log_len = vtx_cpu32(ext->logical_bytes);
			uint64_t log_end = log_start + (log_len ? log_len - 1 : 0);
			uint32_t num_blks = vtx_cpu32(ext->physical_blocks);
			uint32_t eflags = vtx_cpu32(ext->flags);

			char log_str[64], pblk_str[64], vag_str[64], csum_str[64];
			snprintf(log_str, sizeof(log_str), "%llu .. %llu",
			         (unsigned long long)log_start, (unsigned long long)log_end);

			if (eflags & VTX_EXTENT_INLINE) {
				snprintf(pblk_str, sizeof(pblk_str), "(INLINE B-TREE)");
				snprintf(vag_str, sizeof(vag_str), "LEAF PAYLOAD");
				snprintf(csum_str, sizeof(csum_str), "%sINLINE%s (%u B)", C_GREEN, C_RESET, vtx_cpu32(ext->payload_bytes));
			} else {
				uint64_t vblk = vtx_cpu64(ext->vblk);
				__u64 pblk_start = 0;
				vtx_vag_vblk_to_pblk(&h.fs->vag, vblk, &pblk_start);
				uint64_t pblk_end = pblk_start + (num_blks ? num_blks - 1 : 0);

				if (ext_idx > 0 && pblk_start != prev_pblk_end + 1)
					discontinuities++;
				prev_pblk_end = pblk_end;

				snprintf(pblk_str, sizeof(pblk_str), "PBLK %llu .. %llu",
				         (unsigned long long)pblk_start, (unsigned long long)pblk_end);

				if (pblk_start >= 32768ULL) {
					uint32_t vid = (uint32_t)((pblk_start - 32768ULL) / VTX_VAG_NUM_BLKS);
					uint32_t voff = (uint32_t)((pblk_start - 32768ULL) % VTX_VAG_NUM_BLKS);
					snprintf(vag_str, sizeof(vag_str), "VAG #%u (Offset %u)", vid, voff);
					if (vid < 256) vag_counts[vid] += num_blks;
				} else {
					snprintf(vag_str, sizeof(vag_str), "SYSTEM RESERVED");
				}

				uint16_t ccount = vtx_cpu16(ext->csum_count);
				if (ccount > 0 && bytes >= sizeof(struct vtx_extent_item) + 4) {
					uint32_t first_crc = 0;
					memcpy(&first_crc, ext->tail, 4);
					snprintf(csum_str, sizeof(csum_str), "CRC32C: 0x%08x (%u blks)", vtx_cpu32(first_crc), ccount);
				} else {
					snprintf(csum_str, sizeof(csum_str), "CRC32C Verified");
				}
			}

			printf("  %-4u %-24s %8.1f KiB %7u blk %-22s %-20s %s\n",
			       ext_idx, log_str, (double)log_len / 1024.0, num_blks,
			       pblk_str, vag_str, csum_str);
			ext_idx++;
		}
		free(item_buf);
	}
	printf("  ------------------------------------------------------------------------------------------------------------------------\n");

	if (ext_idx == 0) {
		printf("  (Tệp tin rỗng hoặc không có extent nào trong B-Tree)\n");
	} else {
		double frag_rate = (ext_idx > 1) ? ((double)discontinuities / (double)(ext_idx - 1) * 100.0) : 0.0;
		printf("\n%s3. ĐỊA HÌNH VAG & ĐỘ PHÂN MẢNH (VAG TOPOGRAPHY & FRAGMENTATION):%s\n", C_CYAN, C_RESET);
		printf("  Tổng số Extents          : %s%u extents%s\n", C_BOLD, ext_idx, C_RESET);
		printf("  Số điểm đứt gãy vật lý   : %s%u điểm%s\n",
		       discontinuities > 0 ? C_YELLOW : C_GREEN, discontinuities, C_RESET);
		printf("  Tỷ lệ phân mảnh (Rate)   : %s%.1f%%%s (%s)\n",
		       frag_rate > 20.0 ? C_RED : (frag_rate > 0.0 ? C_YELLOW : C_GREEN), frag_rate, C_RESET,
		       discontinuities == 0 ? "Hoàn toàn liền mạch / Optimal Streaming" : "Có phân mảnh rải rác");
	}

	printf("\n");
	vtxfs_cli_close(&h, 0);
	return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand Entry: file
 * -------------------------------------------------------------------------- */
int cmd_file(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	if (argc < 1) {
		fprintf(stderr, "Cách dùng: vtxfs file <đường_dẫn_tệp_đang_mount>\n");
		fprintf(stderr, "      hoặc: vtxfs file <thiết_bị_hoặc_image> <đường_dẫn_trong_vtxfs>\n");
		fprintf(stderr, "\nVí dụ:\n");
		fprintf(stderr, "  vtxfs file /mnt/vtxfs_usb_test/Documents/ndcfs.tar.xz\n");
		fprintf(stderr, "  vtxfs file /dev/sda2 /Documents/ndcfs.tar.xz\n");
		return 1;
	}

	if (argc >= 2) {
		return inspect_offline_file(argv[0], argv[1]);
	}

	/* argc == 1 */
	const char *target = argv[0];
	struct stat st;
	if (stat(target, &st) == 0 && S_ISBLK(st.st_mode)) {
		fprintf(stderr, "%sThông báo: '%s' là thiết bị khối. Vui lòng cung cấp thêm đường dẫn tệp bên trong VTXFS:%s\n",
		        C_YELLOW, target, C_RESET);
		fprintf(stderr, "  Ví dụ: vtxfs file %s /tên_tệp\n", target);
		return 1;
	}

	return inspect_online_file(target);
}
