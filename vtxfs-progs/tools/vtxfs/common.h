/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef VTXFS_CLI_COMMON_H
#define VTXFS_CLI_COMMON_H

#include <stddef.h>
#include "vtxfs_engine.h"

struct vtxfs_handle {
	struct vtx_bdev bdev;
	struct vtx_fs *fs;
	const char *path;
	int rw;
};

int vtxfs_cli_open(struct vtxfs_handle *h, const char *path, int rw);
void vtxfs_cli_close(struct vtxfs_handle *h, int commit);
void vtxfs_cli_print_uuid(const __u8 u[16]);
const char *vtxfs_cli_subvol_flags_str(__u64 flags, char *buf, size_t n);

int vtxfs_cli_for_each_subvol(struct vtxfs_handle *h,
	int (*cb)(const struct vtx_subvol_root_item *it, __u64 id, void *arg),
	void *arg);

int vtxfs_cli_find_subvol_by_uuid(struct vtxfs_handle *h,
	const __u8 uuid[16], __u64 *id_out);

const char *vtxfs_cli_ftype_name(__u8 t);

/* Selection flags for vtxfs_cli_select_subvol */
#define VTXFS_SELECT_WRITE          (1U << 0)  /* cần quyền ghi */
#define VTXFS_SELECT_ALLOW_REAPING  (1U << 1)  /* cho phép đọc subvol REAPING */
#define VTXFS_SELECT_VIEW           (1U << 2)  /* switch không flush, chỉ đọc */

int vtxfs_cli_select_subvol(struct vtx_fs *fs, __u64 subvol_id,
			    unsigned int flags,
			    struct vtx_subvol_root_item *item_out);

/* Option parsing helpers */
const char *vtxfs_cli_opt(int argc, char **argv, const char *key);
__u64 vtxfs_cli_opt_u64(int argc, char **argv, const char *key,
			__u64 fallback);
int vtxfs_cli_flag(int argc, char **argv, const char *key);
int vtxfs_cli_parse_subvol(int argc, char **argv, __u64 *subvol_out);

#endif
