/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_builder.h - Xi to immutable SemanticPlan construction
 */

#ifndef XR_SEMANTIC_BUILDER_H
#define XR_SEMANTIC_BUILDER_H

#include "xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>

struct XiFunc;
struct XiModule;

XR_FUNC bool xr_semantic_plan_build(const struct XiFunc *root, XrSemanticPlan **out, char *error,
                                    size_t error_size);
XR_FUNC bool xr_semantic_plan_build_and_attach(struct XiFunc *root, char *error, size_t error_size);
XR_FUNC bool xr_semantic_plan_build_and_attach_module_set(
    struct XiFunc *root, struct XiModule *const *dependencies, uint32_t dependency_count,
    char *error, size_t error_size);


/* Internal producer authority queries; they retain no frontend pointers. */
struct XrType;
struct XiValue;
XR_FUNC bool xr_semantic_source_type_admits_parameter(
    const struct XiFunc *caller, const struct XrType *type, const XrSemanticPlan *dependency,
    uint32_t parameter, struct XiModule *const *modules, uint32_t module_count);
XR_FUNC bool xr_semantic_source_types_equal(
    const struct XiFunc *caller, const struct XrType *left, const struct XrType *right,
    struct XiModule *const *modules, uint32_t module_count);
XR_FUNC bool xr_semantic_source_constructor_argument_admits(
    const struct XiFunc *caller, const struct XiValue *argument, const XrSemanticPlan *dependency,
    uint32_t parameter, struct XiModule *const *modules, uint32_t module_count);

#endif  // XR_SEMANTIC_BUILDER_H
