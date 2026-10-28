/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef VTXFS_COMMANDS_H
#define VTXFS_COMMANDS_H

#include <stddef.h>

struct cmd_struct;
struct cmd_group;

struct cmd_struct {
	const char *name;
	int (*fn)(const struct cmd_struct *cmd, int argc, char **argv);
	const struct cmd_group *group;
	const char *usage_short;
};

struct cmd_group {
	const char *name;
	const char *usage;
	const struct cmd_struct *cmds;
	size_t ncmds;
};

int vtxfs_dispatch(const struct cmd_group *grp, int argc, char **argv);
void vtxfs_print_group_usage(const struct cmd_group *grp);

/* Các nhóm lệnh định nghĩa ở từng cmds-*.c */
extern const struct cmd_group filesystem_cmd_group;
extern const struct cmd_group subvolume_cmd_group;
extern const struct cmd_group snapshot_cmd_group;
extern const struct cmd_group inspect_cmd_group;
extern const struct cmd_group rescue_cmd_group;
extern const struct cmd_group restore_cmd_group;
extern const struct cmd_group property_cmd_group;
extern const struct cmd_group balance_cmd_group;
extern const struct cmd_group defrag_cmd_group;
extern const struct cmd_group reaper_cmd_group;
extern const struct cmd_group device_cmd_group;
extern const struct cmd_group scrub_cmd_group;
extern const struct cmd_group quota_cmd_group;
extern const struct cmd_group frag_cmd_group;
extern const struct cmd_group image_cmd_group;

/* Lệnh lá ở top-level */
extern int cmd_check(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_format(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_label(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_reflink(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_defragment(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_send(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_receive(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_frag(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_stat(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_map(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_block(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_bitmap(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_bench(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_ctl(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_sys_index(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_file(const struct cmd_struct *cmd, int argc, char **argv);
extern int cmd_cp(const struct cmd_struct *cmd, int argc, char **argv);

#endif
