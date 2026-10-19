/*
 * One generated translation unit is intentional: the host adapter is the ABI
 * boundary seen by the VM, while the canonical hosted-fragment entry stays
 * available to other hosts.  Co-locating only those two generated layers lets
 * the platform compiler inline the entry without mixing the AOT and VM runtime
 * implementation headers.  VM object/error operations remain in the separate
 * host bridge (xstdlib_vm_fastpath.c).
 */

#include "xstdlib_vm_fastpaths_generated.h"
#include "xstdlib_vm_fastpaths_generated.c"
#include "../../include/xray_yieldable_abi.h"

#define XR_STDLIB_VM_FASTPATH_EMIT_ADAPTERS 1
#include "xstdlib_vm_fastpaths_generated.inc.c"

XRT_INTERNAL void xr_stdlib_vm_fastpath_release_native(XrValue value) {
    xrt_release(value);
}

XRT_INTERNAL void xr_stdlib_vm_fastpath_retain_native(XrValue value) {
    (void) xrt_retain(value);
}
