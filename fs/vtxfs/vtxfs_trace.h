/* SPDX-License-Identifier: GPL-2.0-only */
/* VTXFS kernel function-entry audit. Enable with dynamic_debug, never by default.
 *
 * Each call site is a separate dynamic-debug entry so that callers including
 * static helpers and VFS callbacks can be enabled selectively by file/function.
 * Do NOT use pr_info or unconditional printk here: these are hot paths.
 * pr_debug_ratelimited limits bursts after explicit opt-in.
 */
#ifndef VTXFS_TRACE_H
#define VTXFS_TRACE_H

#include <linux/printk.h>
#include <linux/sched.h>

#define VTXFS_TRACE_ENTER() \
	pr_debug_ratelimited("vtxfs-call enter %s() pid=%d\n", \
			     __func__, task_pid_nr(current))

#define VTXFS_TRACE_EVENT(fmt, ...) \
	pr_debug_ratelimited("vtxfs-event %s(): " fmt, \
			     __func__, ##__VA_ARGS__)

#endif /* VTXFS_TRACE_H */
