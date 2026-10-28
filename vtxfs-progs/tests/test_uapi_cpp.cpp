#include "../uapi/vtxfs.h"
#include <cstddef>
static_assert(VTXFS_VERSION_MAJOR == 2, "v2 major");
static_assert(VTXFS_VERSION_MINOR == 1, "v2.1 minor");
static_assert(sizeof(vtx_ioctl_abi) == 32, "abi layout");
static_assert(sizeof(vtx_ioc_capabilities) == 56, "capability layout");
static_assert(sizeof(vtx_ioc_subvol_args) == 320, "subvol layout");
static_assert(sizeof(vtx_ioc_dev_args) == 600, "device layout");
static_assert(sizeof(vtx_ioc_omap_lookup) == 88, "omap layout");
static_assert(sizeof(vtx_ioc_scrub_args) == 80, "scrub layout");
static_assert(sizeof(vtx_ioc_encoded_io) == 64, "encoded layout");
static_assert(sizeof(vtx_ioc_send_args) == 64, "send layout");
static_assert(sizeof(vtx_ioc_sync) == 32, "sync layout");
static_assert(_IOC_DIR(VTXFS_IOC_GET_ABI) == (_IOC_READ | _IOC_WRITE), "GET_ABI direction");
int main() { return 0; }
