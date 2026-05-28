/*
 * xm_metadata_verify.c - Codegen metadata invariant checker (impl).
 *
 * The pure inline verifier lives in xm_metadata_verify.h; this file only hosts
 * the logging + result-mutating wrapper so the header stays free of xlog deps
 * and remains trivially unit-testable.
 */

#include "xm_metadata_verify.h"

#include <stdbool.h>

#include "../base/xdefs.h"
#include "../base/xlog.h"

/* Run xm_verify_metadata and, on the first violation, log a warning and mark
 * the result as failed. Returns true if metadata is valid; false otherwise.
 * arch_align is 1 for x64 (variable-length encoding) and 4 for arm64/rv64
 * (fixed 32-bit instructions). arch_name is the xlog module tag. */
XR_FUNC bool xm_verify_metadata_or_fail(XmCodegenResult *result, uint32_t arch_align,
                                        const char *arch_name) {
    if (result == NULL)
        return false;

    uint32_t err_idx = 0;
    const char *err_kind = NULL;
    XmMetaVerifyError err = xm_verify_metadata(result, arch_align, &err_idx, &err_kind);
    if (err == XM_META_VERIFY_OK)
        return true;

    xr_log_warning(arch_name ? arch_name : "cg", "metadata verifier: %s entry #%u: %s",
                   err_kind ? err_kind : "?", err_idx, xm_meta_verify_strerror(err));
    result->success = false;
    result->error = xm_meta_verify_strerror(err);
    return false;
}
