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

typedef enum XaJsonCapabilityReason {
    XA_JSON_CAPABILITY_OK = 0,
    XA_JSON_CAPABILITY_FUNCTION_FIELD,
    XA_JSON_CAPABILITY_NON_STRING_MAP_KEY,
    XA_JSON_CAPABILITY_OPEN_ROW_TARGET,
    XA_JSON_CAPABILITY_UNSUPPORTED_RECURSIVE_ALIAS,
    XA_JSON_CAPABILITY_MISSING_DERIVE_SIDECAR,
    XA_JSON_CAPABILITY_NON_ENCODABLE_NATIVE_HANDLE,
    XA_JSON_CAPABILITY_NON_DECODABLE_NATIVE_HANDLE,
    XA_JSON_CAPABILITY_UNINSTANTIATED_TYPE_PARAMETER,
    XA_JSON_CAPABILITY_TUPLE_TARGET_NOT_DECODABLE,
    XA_JSON_CAPABILITY_UNSUPPORTED_TYPE,
} XaJsonCapabilityReason;

typedef struct XaJsonCapabilityResult {
    bool supported;
    XaJsonCapabilityReason reason;
    const XrType *blocking_type;
} XaJsonCapabilityResult;

XR_FUNC uint32_t xa_type_capability_flags(const XrType *type);
XR_FUNC bool xa_type_has_capabilities(const XrType *type, uint32_t required);
XR_FUNC uint32_t xa_stdlib_type_capability_flags(const char *module_name,
                                                 const char *declaration_name);
XR_FUNC uint32_t xa_declared_type_capability_flags(const char *file_path,
                                                   const char *declaration_name);
XR_FUNC XaJsonCapabilityResult xa_json_encodable(const XrType *type);
XR_FUNC XaJsonCapabilityResult xa_json_decodable(const XrType *type);
XR_FUNC const char *xa_json_capability_reason_name(XaJsonCapabilityReason reason);

#endif  // XANALYZER_CAPABILITY_H
