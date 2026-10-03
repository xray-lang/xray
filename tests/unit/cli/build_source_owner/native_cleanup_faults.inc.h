/* Exercise CLI cleanup control flow around the real, fully executed owner. */
#include "app/toolchain/xtc_xir_native_operation.h"
#undef xtc_xir_native_operation_close
#undef xtc_xir_native_operation_cleanup_diagnostic
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_close(
    XtcXirNativeOperation **owner, uint32_t steps);
XR_FUNC const XtcXirNativeOperationDiagnostic *xtc_xir_native_operation_cleanup_diagnostic(
    const XtcXirNativeOperation *owner);
static const char *native_cleanup_mode;
static unsigned native_cleanup_calls;
static bool native_cleanup_synthetic;
static XtcXirNativeOperationDiagnostic native_cleanup_diagnostic;
XR_FUNC XtcXirNativeOperationStatus xr_cli_test_operation_close(
    XtcXirNativeOperation **owner, uint32_t steps) {
    native_cleanup_synthetic=false;
    if (owner && *owner && native_cleanup_mode) {
        ++native_cleanup_calls;
        bool pending=!strcmp(native_cleanup_mode,"pending") && native_cleanup_calls<=2;
        bool transient=!strcmp(native_cleanup_mode,"recovered") && native_cleanup_calls==1;
        bool permanent=!strcmp(native_cleanup_mode,"terminal");
        if (pending || transient || permanent) {
            native_cleanup_synthetic=true;
            native_cleanup_diagnostic=(XtcXirNativeOperationDiagnostic){0};
            native_cleanup_diagnostic.status=pending ? XTC_XIR_NATIVE_PENDING : XTC_XIR_NATIVE_IO;
            native_cleanup_diagnostic.domain=XTC_XIR_NATIVE_WORKSPACE;
            native_cleanup_diagnostic.code=pending ? XTC_XIR_WORKSPACE_PENDING : XTC_XIR_WORKSPACE_IO;
            native_cleanup_diagnostic.os_error=pending ? 0 : ERROR_ACCESS_DENIED;
            return native_cleanup_diagnostic.status;
        }
    }
    return xtc_xir_native_operation_close(owner,steps);
}
XR_FUNC const XtcXirNativeOperationDiagnostic *xr_cli_test_operation_cleanup_diagnostic(
    const XtcXirNativeOperation *owner) {
    return native_cleanup_synthetic ? &native_cleanup_diagnostic :
        xtc_xir_native_operation_cleanup_diagnostic(owner);
}
