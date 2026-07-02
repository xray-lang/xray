#ifndef XRAY_STDLIB_SYS_H
#define XRAY_STDLIB_SYS_H

#include "../../src/base/xdefs.h"

struct XrModule;
struct XrVMRuntime;

XR_FUNC struct XrModule *xr_load_module_sys(struct XrVMRuntime *isolate);
XR_FUNC void xr_sys_mutex_register_class(struct XrVMRuntime *isolate);
XR_FUNC void xr_sys_rwlock_register_class(struct XrVMRuntime *isolate);
XR_FUNC void xr_sys_condvar_register_class(struct XrVMRuntime *isolate);
XR_FUNC void xr_sys_barrier_register_class(struct XrVMRuntime *isolate);

#endif /* XRAY_STDLIB_SYS_H */
