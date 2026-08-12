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

#endif  // XR_SEMANTIC_BUILDER_H
