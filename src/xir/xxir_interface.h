/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_interface.h - Interface requirement identities and owned applications
 *
 * KEY CONCEPT:
 *   Abstract requirements carry signatures, never executable function bodies.
 */
#ifndef XXIR_INTERFACE_H
#define XXIR_INTERFACE_H
#include "xxir.h"

typedef struct XrXirInterfaceApplication {
    uint32_t declaration;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirInterfaceApplication;

typedef struct XrXirInterfaceMethod {
    XrXirLiteral name;
    XrXirType signature;
    /* Zero denotes the ordinary read receiver. */
    uint32_t receiver;
} XrXirInterfaceMethod;

typedef struct XrXirInterfaceDeclaration {
    XrXirLiteral module, name;
    uint32_t exported;
    const XrXirConstraint *constraints;
    uint32_t parameter_count;
    const XrXirInterfaceApplication *parents;
    uint32_t parent_count;
    const XrXirInterfaceMethod *methods;
    uint32_t method_count;
} XrXirInterfaceDeclaration;

typedef struct XrXirInterfaceTable {
    const XrXirInterfaceDeclaration *declarations;
    uint32_t count;
} XrXirInterfaceTable;

/* Structural validation alone grants no member lookup, conformance or execution.
 * Budgets and outputs publish only after successful complete validation. */
XR_FUNC XrXirStatus xr_xir_interfaces_verify_structure(const XrXirInterfaceTable *table,
    const XrXirTypes *types, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_interfaces_clone(const XrXirInterfaceTable *table,
    const XrXirTypes *types, XrXirBudget *budget, XrXirInterfaceTable **output);
XR_FUNC void xr_xir_interfaces_free(XrXirInterfaceTable *table);
/* Requires a previously verified table; copies no external type-pool owner. */
XR_FUNC XrXirStatus xr_xir_interfaces_copy_verified(const XrXirInterfaceTable *table,
    XrXirInterfaceTable **output);
#endif // XXIR_INTERFACE_H
