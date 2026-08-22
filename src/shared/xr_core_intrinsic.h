/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_core_intrinsic.h - Stable source identities for core intrinsics
 *
 * KEY CONCEPT:
 *   A core builtin ID is resolved once during source binding. Display names
 *   and diagnostics never participate in semantic identity.
 */

#ifndef XR_CORE_INTRINSIC_H
#define XR_CORE_INTRINSIC_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_CORE_BUILTIN_SCHEMA_VERSION UINT32_C(1)

typedef enum XrCoreBuiltinId {
    XR_CORE_BUILTIN_NONE = 0,
#define XR_CORE_INTRINSIC(id, stable_id, source_name, category, call_form, parameter_shape,       \
                          min_arity, max_arity, result_shape, effect_kind, flow_rule,               \
                          expected_failure_channel, semantic_op, target_applicability,              \
                          diagnostic_name)                                                          \
    XR_CORE_BUILTIN_##id = stable_id,
#include "xr_core_intrinsic.def"
#undef XR_CORE_INTRINSIC
    XR_CORE_BUILTIN_ID_LIMIT = 6,
    XR_CORE_BUILTIN_COUNT = 5,
} XrCoreBuiltinId;

typedef enum XrCoreIntrinsicCategory {
    XR_CORE_INTRINSIC_CATEGORY_NONE = 0,
    XR_CORE_INTRINSIC_CATEGORY_ASSERTION,
    XR_CORE_INTRINSIC_CATEGORY_OUTPUT,
    XR_CORE_INTRINSIC_CATEGORY_COUNT,
} XrCoreIntrinsicCategory;

typedef enum XrCoreIntrinsicCallForm {
    XR_CORE_INTRINSIC_CALL_FORM_NONE = 0,
    XR_CORE_INTRINSIC_CALL_FORM_DIRECT_ONLY,
    XR_CORE_INTRINSIC_CALL_FORM_ORDINARY_CALLABLE,
    XR_CORE_INTRINSIC_CALL_FORM_COUNT,
} XrCoreIntrinsicCallForm;

typedef enum XrCoreIntrinsicParameterShape {
    XR_CORE_INTRINSIC_PARAMETER_SHAPE_NONE = 0,
    XR_CORE_INTRINSIC_PARAMETER_SHAPE_BOOL_OPTIONAL_MESSAGE,
    XR_CORE_INTRINSIC_PARAMETER_SHAPE_SAME_TYPE_PAIR_OPTIONAL_MESSAGE,
    XR_CORE_INTRINSIC_PARAMETER_SHAPE_ACTION_OPTIONAL_MESSAGE,
    XR_CORE_INTRINSIC_PARAMETER_SHAPE_VARIADIC_VALUES,
    XR_CORE_INTRINSIC_PARAMETER_SHAPE_COUNT,
} XrCoreIntrinsicParameterShape;

typedef enum XrCoreIntrinsicResultShape {
    XR_CORE_INTRINSIC_RESULT_SHAPE_NONE = 0,
    XR_CORE_INTRINSIC_RESULT_SHAPE_UNIT,
    XR_CORE_INTRINSIC_RESULT_SHAPE_COUNT,
} XrCoreIntrinsicResultShape;

typedef enum XrCoreIntrinsicEffectKind {
    XR_CORE_INTRINSIC_EFFECT_NONE = 0,
    XR_CORE_INTRINSIC_EFFECT_MAY_PANIC,
    XR_CORE_INTRINSIC_EFFECT_INVOKES_ACTION_MAY_PANIC,
    XR_CORE_INTRINSIC_EFFECT_OUTPUT_MAY_PANIC,
    XR_CORE_INTRINSIC_EFFECT_COUNT,
} XrCoreIntrinsicEffectKind;

typedef enum XrCoreIntrinsicFlowRule {
    XR_CORE_INTRINSIC_FLOW_NONE = 0,
    XR_CORE_INTRINSIC_FLOW_ASSERT_TRUE,
    XR_CORE_INTRINSIC_FLOW_COUNT,
} XrCoreIntrinsicFlowRule;

typedef enum XrCoreIntrinsicExpectedFailureChannel {
    XR_CORE_INTRINSIC_FAILURE_CHANNEL_NONE = 0,
    XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR,
    XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC,
    XR_CORE_INTRINSIC_FAILURE_CHANNEL_COUNT,
} XrCoreIntrinsicExpectedFailureChannel;

/* These target-neutral operations select plan construction. Xi operation
 * ownership and executable behavior continue to come from xisa metadata. */
typedef enum XrCoreIntrinsicSemanticOp {
    XR_CORE_INTRINSIC_SEMANTIC_OP_NONE = 0,
    XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_CONDITION,
    XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_EQUAL,
    XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_THROWS,
    XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_PANICS,
    XR_CORE_INTRINSIC_SEMANTIC_OP_PRINT_GROUP,
    XR_CORE_INTRINSIC_SEMANTIC_OP_COUNT,
} XrCoreIntrinsicSemanticOp;

typedef enum XrCoreIntrinsicTargetApplicability {
    XR_CORE_INTRINSIC_TARGET_NONE = 0,
    XR_CORE_INTRINSIC_TARGET_VM = 1u << 0,
    XR_CORE_INTRINSIC_TARGET_AOT_HOSTED = 1u << 1,
    XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_ASSERTION_PROVIDER = 1u << 2,
    XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_OUTPUT_PROVIDER = 1u << 3,
    XR_CORE_INTRINSIC_TARGET_ASSERTION_ALL =
        XR_CORE_INTRINSIC_TARGET_VM | XR_CORE_INTRINSIC_TARGET_AOT_HOSTED |
        XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_ASSERTION_PROVIDER,
    XR_CORE_INTRINSIC_TARGET_OUTPUT_ALL =
        XR_CORE_INTRINSIC_TARGET_VM | XR_CORE_INTRINSIC_TARGET_AOT_HOSTED |
        XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_OUTPUT_PROVIDER,
} XrCoreIntrinsicTargetApplicability;

typedef struct XrCoreIntrinsicDesc {
    XrCoreBuiltinId id;
    const char *source_name;
    XrCoreIntrinsicCategory category;
    XrCoreIntrinsicCallForm call_form;
    XrCoreIntrinsicParameterShape parameter_shape;
    uint16_t min_arity;
    uint16_t max_arity;
    XrCoreIntrinsicResultShape result_shape;
    XrCoreIntrinsicEffectKind effect;
    XrCoreIntrinsicFlowRule flow_rule;
    XrCoreIntrinsicExpectedFailureChannel expected_failure_channel;
    XrCoreIntrinsicSemanticOp semantic_op;
    uint32_t target_applicability;
    const char *diagnostic_name;
} XrCoreIntrinsicDesc;

XR_FUNC const XrCoreIntrinsicDesc *xr_core_intrinsic_by_id(XrCoreBuiltinId id);
XR_FUNC const XrCoreIntrinsicDesc *xr_core_intrinsic_by_source_name(const char *name,
                                                                    size_t length);
XR_FUNC size_t xr_core_intrinsic_count(void);
XR_FUNC const XrCoreIntrinsicDesc *xr_core_intrinsic_at(size_t index);
XR_FUNC bool xr_core_intrinsic_registry_validate(char *error, size_t error_size);

#endif  // XR_CORE_INTRINSIC_H
