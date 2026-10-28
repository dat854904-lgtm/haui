// SPDX-License-Identifier: GPL-2.0-only
/*
 * ioctl.c — centralized ioctl dispatch.
 *
 * Feature-gate policy (UAPI contract): VTX_IOC_SUBVOL_CREATE /
 * VTX_IOC_SNAP_CREATE / VTX_IOC_SUBVOL_DESTROY and all other RESERVED
 * commands return -EOPNOTSUPP until:
 *   1) a UAPI protocol revision freezes their structs/flags,
 *   2) engine-side ioctl handlers exist and are crash-tested,
 *   3) tests/run.sh grows kernel-side coverage for them.
 * Subvolume/snapshot exposure today is via `mount -o subvol=<id>`.
 */
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/fiemap.h>
#include <linux/ratelimit.h>
#include <linux/capability.h>
#include "vtxfs_drv.h"
#include "ctree.h"
#include "vtxfs_subsys.h"
#include "vag.h"
#include "vtxfs_trace.h"


static int vtxfs_ioctl_reserved(unsigned int cmd)
{
	VTXFS_TRACE_ENTER();

	pr_info_ratelimited("vtxfs: ioctl 0x%08x is RESERVED (feature gate not opened; see docs/P0_CONTRACTS.md)\n",
			    cmd);
	return -EOPNOTSUPP;
}

static long vtxfs_do_fiemap(struct file *file, unsigned long arg)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = file_inode(file);
	struct fiemap fm;
	struct fiemap_extent_info fi = { 0 };
	int ret;

	if (copy_from_user(&fm, (void __user *)arg, sizeof(fm)))
		return -EFAULT;
	if (fm.fm_flags & ~(FIEMAP_FLAG_SYNC | FIEMAP_FLAG_XATTR))
		return -EBADR;

	fi.fi_flags = fm.fm_flags;
	fi.fi_extents_max = fm.fm_extent_count;
	fi.fi_extents_start =
		(struct fiemap_extent __user *)(unsigned long)
		((char __user *)arg + sizeof(fm));

	ret = vtxfs_fiemap(inode, &fi, fm.fm_start, fm.fm_length);
	if (ret < 0)
		return ret;

	fm.fm_mapped_extents = fi.fi_extents_mapped;
	fm.fm_flags = fi.fi_flags;
	return copy_to_user((void __user *)arg, &fm, sizeof(fm))
		? -EFAULT : 0;
}

long vtxfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	VTXFS_TRACE_ENTER();

	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct vtxfs_sb_info *sbi = VTXFS_SB(sb);
	void __user *argp = (void __user *)arg;
	int ret;

	switch (cmd) {
	case VTXFS_IOC_GET_ABI: {
		struct vtx_ioctl_abi abi;

		if (copy_from_user(&abi, argp, sizeof(abi)))
			return -EFAULT;
		vtxfs_op_begin(sbi);
		ret = vtx_ioctl_get_abi(sbi->fs, &abi);
		vtxfs_op_end(sbi);
		if (ret)
			return ret;
		return copy_to_user(argp, &abi, sizeof(abi)) ? -EFAULT : 0;
	}
	case VTXFS_IOC_GET_CAPABILITIES: {
		struct vtx_ioc_capabilities caps;

		if (copy_from_user(&caps, argp, sizeof(caps)))
			return -EFAULT;
		vtxfs_op_begin(sbi);
		ret = vtx_ioctl_get_capabilities(sbi->fs, &caps);
		vtxfs_op_end(sbi);
		if (ret)
			return ret;
		return copy_to_user(argp, &caps, sizeof(caps)) ? -EFAULT : 0;
	}
	case VTX_IOC_START_SYNC:
	case VTX_IOC_WAIT_SYNC: {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		struct vtx_ioc_sync sync_args;

		if (copy_from_user(&sync_args, argp, sizeof(sync_args)))
			return -EFAULT;
		if (sync_args.struct_size != sizeof(sync_args) ||
		    sync_args.flags != 0 ||
		    sync_args.reserved[0] || sync_args.reserved[1])
			return -EINVAL;
		if (sb_rdonly(sb))
			return -EROFS;
		vtxfs_commit_begin(sbi);
		ret = vtx_sync_checkpoint(sbi->fs);
		if (ret == 0)
			sync_args.xid = sbi->fs->committed_xid;
		vtxfs_commit_end(sbi);
		if (ret)
			return ret;
		return copy_to_user(argp, &sync_args, sizeof(sync_args))
			? -EFAULT : 0;
	}
	case FS_IOC_FIEMAP:
		return vtxfs_do_fiemap(file, arg);

	case VTX_IOC_OMAP_LOOKUP: {
		struct vtx_ioc_omap_lookup omap_args;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(&omap_args, argp, sizeof(omap_args)))
			return -EFAULT;
		vtxfs_op_begin(sbi);
		ret = vtx_ioctl_omap_lookup(sbi->fs, &omap_args);
		vtxfs_op_end(sbi);
		if (ret)
			return ret;
		return copy_to_user(argp, &omap_args, sizeof(omap_args))
			? -EFAULT : 0;
	}
	case VTX_IOC_SCRUB: {
		struct vtx_ioc_scrub_args scrub_args;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(&scrub_args, argp, sizeof(scrub_args)))
			return -EFAULT;
		vtxfs_op_begin(sbi);
		ret = vtx_ioctl_scrub(sbi->fs, &scrub_args);
		vtxfs_op_end(sbi);
		if (ret)
			return ret;
		return copy_to_user(argp, &scrub_args, sizeof(scrub_args))
			? -EFAULT : 0;
	}


	/* RESERVED / DEFERRED — feature gate closed (see header comment). */
	case VTX_IOC_SUBVOL_CREATE:
	case VTX_IOC_SNAP_CREATE:
	case VTX_IOC_SUBVOL_DESTROY:
	case VTX_IOC_DEV_ADD:
	case VTX_IOC_DEV_REMOVE:
	case VTX_IOC_DEV_REPLACE:
	case VTX_IOC_DEV_INFO:
	case VTX_IOC_ENCODED_READ:
	case VTX_IOC_ENCODED_WRITE:
	case VTX_IOC_SEND:
	case VTX_IOC_RECEIVE:
		return vtxfs_ioctl_reserved(cmd);


	default:
		return -ENOTTY;
	}
}
