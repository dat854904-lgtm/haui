// SPDX-License-Identifier: GPL-2.0-only
/*
 * compression.c — compression decision framework. The actual codecs
 * (zstd/lz4/zlib) and VTX_EXTENT_COMPRESSED are DEFERRED per the UAPI
 * feature gate; this provides the stable call sites so the inode/extent
 * path does not change when compression lands.
 */
#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

enum vtx_compr_type vtxfs_select_compression(struct inode *inode)
{
	VTXFS_TRACE_ENTER();

	struct vtxfs_inode *vinode = container_of(inode,
			struct vtxfs_inode, vfs_inode);
	u64 flags = vtx_cpu64(vinode->disk.flags);

	if (flags & VTX_INODE_NOCOMPRESS)
		return VTX_COMPR_NONE;
	if (!(flags & VTX_INODE_COMPRESS))
		return VTX_COMPR_NONE;
	/* DEFERRED: no codec enabled in P0/P1. */
	return VTX_COMPR_NONE;
}

bool vtxfs_compress_extent(struct inode *inode, u64 start, u64 len)
{
	VTXFS_TRACE_ENTER();

	(void)start;
	if (vtxfs_select_compression(inode) == VTX_COMPR_NONE)
		return false;
	/* Too-small extents do not pay for a compressed frame header. */
	if (len < VTXFS_BLOCK_SIZE)
		return false;
	if (len > VTXFS_MAX_COMPRESSED_FRAME_BYTES)
		return false;
	return false; /* codec not yet implemented */
}
