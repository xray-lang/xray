/* Build-time generated stdlib-native entries installed over script exports. */

#ifndef XSTDLIB_VM_FASTPATH_H
#define XSTDLIB_VM_FASTPATH_H

#include <stdbool.h>
#include <stddef.h>
#include "../../include/xray_value_abi.h"
#include "../../include/xray_hosted_fragment_abi.h"

typedef struct XrVMRuntime XrVMRuntime;
typedef struct XrModule XrModule;
typedef XrValue (*XrStdlibVmFastpathFn)(XrVMRuntime *isolate, XrValue *args, int nargs);

bool xr_stdlib_vm_fastpath_install(XrVMRuntime *isolate, XrModule *module,
                                   const char *module_name);
XrStdlibVmFastpathFn xr_stdlib_vm_fastpath_lookup(const char *module_name,
                                                  const char *member_name);
size_t xr_stdlib_vm_fastpath_count(void);

#endif /* XSTDLIB_VM_FASTPATH_H */
