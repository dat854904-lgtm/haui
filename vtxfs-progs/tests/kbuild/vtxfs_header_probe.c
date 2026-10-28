// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include "vtxfs_mem.h"
#include "vtxfs_engine.h"

static int __init vtxfs_probe_init(void)
{
	struct vtx_key key;

	return vtxfs_mk_key(&key, 123, VTX_TYPE_INODE, 0);
}

static void __exit vtxfs_probe_exit(void)
{
}

module_init(vtxfs_probe_init);
module_exit(vtxfs_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Compile-only validation probe for VTXFS v2.1 P0 core engine");
