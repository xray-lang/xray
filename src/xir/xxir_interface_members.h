/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_interface_members.h - Exact inherited requirement consistency
 */
#ifndef XXIR_INTERFACE_MEMBERS_H
#define XXIR_INTERFACE_MEMBERS_H
#include "xxir_interface.h"

typedef struct XrXirInterfaceClosure XrXirInterfaceClosure;
typedef struct XrXirInterfaceRequirement {
    uint32_t application, origin_interface, member;
    XrXirType signature;
    XrXirLiteral name;
    uint32_t receiver;
} XrXirInterfaceRequirement;

/* Inputs require structural and acyclic validation in one parameter context.
 * Closures borrow immutable input types, nominal metadata and declaration names;
 * they must be freed before those inputs. They own application arrays and newly
 * materialized descriptors. Scratch IDs belong exclusively to closure_types and
 * must not enter persistent XIR; consumers must reify into their own type arena.
 * Failure preserves the budget and leaves output NULL. */
typedef struct XrXirInterfaceClosureRequest {
    const XrXirTypes *source_types, *actual_types;
    const XrXirInterfaceApplication *roots;
    uint32_t root_count;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirInterfaceClosureRequest;
/* Root expressions and the interface catalog belong to source_types. The
 * complete substitution belongs to actual_types, whose pool is borrowed. */
XR_FUNC XrXirStatus xr_xir_interface_closure_substitute(
    const XrXirInterfaceClosureRequest *request, XrXirBudget *budget,
    XrXirInterfaceClosure **output);
XR_FUNC XrXirStatus xr_xir_interface_closure_build(const XrXirInterfaceTable *table,
    const XrXirTypes *types, const XrXirInterfaceApplication *roots, uint32_t root_count,
    XrXirBudget *budget, XrXirInterfaceClosure **output);
XR_FUNC const XrXirTypes *xr_xir_interface_closure_types(const XrXirInterfaceClosure *closure);
XR_FUNC uint32_t xr_xir_interface_closure_application_count(const XrXirInterfaceClosure *closure);
XR_FUNC const XrXirInterfaceApplication *xr_xir_interface_closure_application(
    const XrXirInterfaceClosure *closure, uint32_t index);
XR_FUNC uint32_t xr_xir_interface_closure_requirement_count(const XrXirInterfaceClosure *closure);
XR_FUNC const XrXirInterfaceRequirement *xr_xir_interface_closure_requirement(
    const XrXirInterfaceClosure *closure, uint32_t index);
XR_FUNC void xr_xir_interface_closure_free(XrXirInterfaceClosure *closure);

/* Requires structurally verified descriptors and acyclic interface declarations.
 * Checks every declaration, including unused ones. Original declarations retain
 * all distinct obligations; temporary application deduplication loses no identity.
 * Failure preserves the caller budget and publishes no partial result. */
XR_FUNC XrXirStatus xr_xir_interfaces_verify_members_verified(
    const XrXirInterfaceTable *table, const XrXirTypes *types, XrXirBudget *budget);
#endif // XXIR_INTERFACE_MEMBERS_H
