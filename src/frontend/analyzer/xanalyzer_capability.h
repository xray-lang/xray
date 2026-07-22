/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Compiler-owned type capability registry. Capabilities attach only to
 * canonical native type IDs or trusted standard-library declarations; call
 * site spelling never grants a capability.
 */

#ifndef XANALYZER_CAPABILITY_H
#define XANALYZER_CAPABILITY_H

#include "../../base/xdefs.h"
#include "../../runtime/value/xtype.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XaTypeCapability {
    XA_TYPE_CAP_NONE = 0,
    XA_TYPE_CAP_INTERIOR_MUTABLE = 1u << 0,
    XA_TYPE_CAP_SYNC_SHAREABLE = 1u << 1,
} XaTypeCapability;

XR_FUNC uint32_t xa_type_capability_flags(const XrType *type);
XR_FUNC bool xa_type_has_capabilities(const XrType *type, uint32_t required);
XR_FUNC uint32_t xa_declared_type_capability_flags(const char *file_path,
                                                   const char *declaration_name);

#endif  // XANALYZER_CAPABILITY_H
