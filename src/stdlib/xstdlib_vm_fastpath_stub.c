/* Bootstrap compiler / disabled-build implementation: no generated entries. */

#include "xstdlib_vm_fastpath.h"

bool xr_stdlib_vm_fastpath_install(XrVMRuntime *isolate, XrModule *module,
                                   const char *module_name) {
    (void) isolate;
    (void) module;
    (void) module_name;
    return true;
}

XrStdlibVmFastpathFn xr_stdlib_vm_fastpath_lookup(const char *module_name,
                                                  const char *member_name) {
    (void) module_name;
    (void) member_name;
    return NULL;
}

size_t xr_stdlib_vm_fastpath_count(void) {
    return 0;
}
