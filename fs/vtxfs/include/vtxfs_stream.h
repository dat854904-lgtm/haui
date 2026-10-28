/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
/*
 * vtxfs_stream.h — Định dạng và giao thức dòng truyền phát VTXFS (VTXSEND1).
 * Hỗ trợ xuất (send) và nhập phục hồi (receive) toàn vẹn subvolume / snapshot.
 */
#ifndef _VTXFS_STREAM_H
#define _VTXFS_STREAM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "vtxfs_engine.h"

#define VTXFS_SEND_STREAM_MAGIC    "VTXSEND1"
#define VTXFS_SEND_STREAM_VERSION  1
#define VTXFS_STREAM_MAX_PAYLOAD   (32U * 1024U * 1024U) /* 32 MiB */
#define VTXFS_STREAM_CHUNK_SIZE    (64U * 1024U)         /* 64 KiB chunk */

/* Mã lệnh trong dòng truyền phát (Stream Command Codes) */
enum vtx_stream_cmd {
	VTX_CMD_UNSPEC       = 0,
	VTX_CMD_SUBVOL       = 1,   /* Khởi tạo metadata subvolume gốc */
	VTX_CMD_MKDIR        = 2,   /* Tạo thư mục */
	VTX_CMD_MKFILE       = 3,   /* Tạo tệp thông thường */
	VTX_CMD_WRITE        = 4,   /* Ghi khối dữ liệu vào tệp */
	VTX_CMD_SYMLINK      = 5,   /* Tạo liên kết mềm (symlink) */
	VTX_CMD_LINK         = 6,   /* Tạo liên kết cứng (hardlink) */
	VTX_CMD_SET_XATTR    = 7,   /* Thiết lập thuộc tính mở rộng (xattr) */
	VTX_CMD_UTIMES       = 8,   /* Cập nhật mốc thời gian atime/mtime */
	VTX_CMD_TRUNCATE     = 9,   /* Cắt/mở rộng kích thước tệp */
	VTX_CMD_CHMOD        = 10,  /* Đặt quyền truy cập */
	VTX_CMD_CHOWN        = 11,  /* Đặt quyền sở hữu uid/gid */
	VTX_CMD_END          = 99,  /* Kết thúc dòng truyền phát, xác thực toàn vẹn */
};

/* Đầu đề dòng truyền phát (Stream Header - đúng 128 bytes) */
struct vtx_stream_header {
	__u8   magic[8];            /* "VTXSEND1" */
	__le32 version;             /* 1 */
	__le32 flags;               /* 0 hoặc các cờ tương thích */
	__u8   fsid[16];            /* UUID của hệ thống tệp gốc */
	__le64 subvol_id;           /* ID subvolume nguồn */
	__le64 parent_subvol_id;    /* ID subvolume cha (nếu là snapshot) */
	__le64 root_xid;            /* Transaction / Generation XID */
	char   subvol_name[64];     /* Tên subvolume */
	__le32 num_commands;        /* Dự kiến số lệnh (hoặc 0 nếu streaming) */
	__u8   reserved[8];         /* Dự trữ */
	__le32 header_crc;          /* Castagnoli CRC32c cho 124 byte đầu */
} __attribute__((packed));

/* Đầu đề của từng gói lệnh (Packet Command Header - 12 bytes) */
struct vtx_stream_cmd_hdr {
	__le16 cmd;                 /* Mã lệnh enum vtx_stream_cmd */
	__le16 flags;               /* Cờ phụ trợ của lệnh */
	__le32 payload_len;         /* Chiều dài dữ liệu payload kèm theo */
	__le32 crc32c;              /* CRC32c của dữ liệu payload */
} __attribute__((packed));

/* Thống kê truyền phát dòng dữ liệu */
struct vtx_stream_stats {
	__u64 total_dirs;
	__u64 total_files;
	__u64 total_symlinks;
	__u64 total_hardlinks;
	__u64 total_xattrs;
	__u64 total_bytes;
	__u64 total_packets;
	__u32 stream_crc;
	double elapsed_sec;
};

/* Cấu hình cho thao tác send */
struct vtx_send_options {
	const char *source_path;    /* Đường dẫn thư mục hoặc thiết bị/ảnh */
	const char *out_file;       /* Đường dẫn tệp xuất ra (NULL = stdout) */
	__u64 subvol_id;            /* Subvolume ID nếu là bdev (0 = mặc định) */
	int verbose;                /* In chi tiết tiến trình */
};

/* Cấu hình cho thao tác receive */
struct vtx_receive_options {
	const char *target_path;    /* Đường dẫn thư mục phục hồi đích */
	const char *in_file;        /* Đường dẫn tệp stream vào (NULL = stdin) */
	int verbose;                /* In chi tiết tiến trình */
};

#ifdef __cplusplus
extern "C" {
#endif

/* API thực thi truyền phát */
int vtx_stream_send(const struct vtx_send_options *opts,
		    struct vtx_stream_stats *stats_out);

int vtx_stream_receive(const struct vtx_receive_options *opts,
		       struct vtx_stream_stats *stats_out);

#ifdef __cplusplus
}
#endif

#endif /* _VTXFS_STREAM_H */
