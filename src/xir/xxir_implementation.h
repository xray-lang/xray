/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_implementation.h - Explicit implementation declarations and owned copies
 *
 * KEY CONCEPT:
 *   Complete requirement applications preserve instantiated witness identity.
 */
#ifndef XXIR_IMPLEMENTATION_H
#define XXIR_IMPLEMENTATION_H
#include "xxir_interface.h"

typedef struct XrXirImplementationBinding {
    XrXirInterfaceApplication requirement;
    uint32_t member, function;
} XrXirImplementationBinding;
typedef struct XrXirImplementation {
    uint32_t nominal_declaration;
    XrXirInterfaceApplication interface;
    const XrXirImplementationBinding *bindings;
    uint32_t binding_count;
} XrXirImplementation;
struct XrXirImplementationTable {
    const XrXirImplementation *records;
    uint32_t count;
};
/* NULL is the only empty table. Nested arrays use NULL iff count is zero.
 * Copies require verified identities and preserve the source type pool IDs. */
XR_FUNC XrXirStatus xr_xir_implementations_copy_verified(
    const XrXirImplementationTable *source, XrXirImplementationTable **output);
XR_FUNC void xr_xir_implementations_free(XrXirImplementationTable *table);
#endif // XXIR_IMPLEMENTATION_H
