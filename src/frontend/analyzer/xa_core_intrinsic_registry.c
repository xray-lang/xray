/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_core_intrinsic_registry.c - Core intrinsic registry lookup and validation
 */

#include "../../shared/xr_core_intrinsic.h"

#include <stdio.h>
#include <string.h>

static const XrCoreIntrinsicDesc g_core_intrinsics[] = {
#define XR_CORE_INTRINSIC(id, stable_id, source_name, category, call_form, parameter_shape,       \
                          min_arity, max_arity, result_shape, effect_kind, flow_rule,               \
                          expected_failure_channel, semantic_op, target_applicability,              \
                          diagnostic_name)                                                          \
    {XR_CORE_BUILTIN_##id,                                                                         \
     source_name,                                                                                  \
     XR_CORE_INTRINSIC_CATEGORY_##category,                                                        \
     XR_CORE_INTRINSIC_CALL_FORM_##call_form,                                                      \
     XR_CORE_INTRINSIC_PARAMETER_SHAPE_##parameter_shape,                                          \
     min_arity,                                                                                    \
     max_arity,                                                                                    \
     XR_CORE_INTRINSIC_RESULT_SHAPE_##result_shape,                                                \
     XR_CORE_INTRINSIC_EFFECT_##effect_kind,                                                       \
     XR_CORE_INTRINSIC_FLOW_##flow_rule,                                                           \
     XR_CORE_INTRINSIC_FAILURE_CHANNEL_##expected_failure_channel,                                 \
     XR_CORE_INTRINSIC_SEMANTIC_OP_##semantic_op,                                                  \
     XR_CORE_INTRINSIC_TARGET_##target_applicability,                                              \
     diagnostic_name},
#include "../../shared/xr_core_intrinsic.def"
#undef XR_CORE_INTRINSIC
};

_Static_assert(sizeof(g_core_intrinsics) / sizeof(g_core_intrinsics[0]) == XR_CORE_BUILTIN_COUNT,
               "core intrinsic descriptor count must match the stable ID domain");

static const char *const g_removed_core_intrinsic_names[] = {
    "likely",      "unlikely",    "assert_true", "assert_false",
    "assert_eq",   "assert_ne",   "assert_throws",
};

static bool set_error(char *error, size_t error_size, const char *format, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, format, detail);
    return false;
}

static bool is_removed_source_name(const char *name) {
    for (size_t i = 0;
         i < sizeof(g_removed_core_intrinsic_names) / sizeof(g_removed_core_intrinsic_names[0]);
         i++) {
        if (strcmp(name, g_removed_core_intrinsic_names[i]) == 0)
            return true;
    }
    return false;
}

size_t xr_core_intrinsic_count(void) {
    return sizeof(g_core_intrinsics) / sizeof(g_core_intrinsics[0]);
}

const XrCoreIntrinsicDesc *xr_core_intrinsic_at(size_t index) {
    return index < xr_core_intrinsic_count() ? &g_core_intrinsics[index] : NULL;
}

const XrCoreIntrinsicDesc *xr_core_intrinsic_by_id(XrCoreBuiltinId id) {
    for (size_t i = 0; i < xr_core_intrinsic_count(); i++) {
        if (g_core_intrinsics[i].id == id)
            return &g_core_intrinsics[i];
    }
    return NULL;
}

const XrCoreIntrinsicDesc *xr_core_intrinsic_by_source_name(const char *name, size_t length) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < xr_core_intrinsic_count(); i++) {
        const char *candidate = g_core_intrinsics[i].source_name;
        if (strlen(candidate) == length && memcmp(candidate, name, length) == 0)
            return &g_core_intrinsics[i];
    }
    return NULL;
}

static bool descriptor_contract_is_valid(const XrCoreIntrinsicDesc *desc) {
    switch (desc->semantic_op) {
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_CONDITION:
            return desc->id == XR_CORE_BUILTIN_ASSERT && strcmp(desc->source_name, "assert") == 0 &&
                   desc->category == XR_CORE_INTRINSIC_CATEGORY_ASSERTION &&
                   desc->parameter_shape ==
                       XR_CORE_INTRINSIC_PARAMETER_SHAPE_BOOL_OPTIONAL_MESSAGE &&
                   desc->min_arity == 1 && desc->max_arity == 2 &&
                   desc->effect == XR_CORE_INTRINSIC_EFFECT_MAY_PANIC &&
                   desc->flow_rule == XR_CORE_INTRINSIC_FLOW_ASSERT_TRUE &&
                   desc->expected_failure_channel == XR_CORE_INTRINSIC_FAILURE_CHANNEL_NONE &&
                   desc->target_applicability == XR_CORE_INTRINSIC_TARGET_ASSERTION_ALL;
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_EQUAL:
            return desc->id == XR_CORE_BUILTIN_ASSERT_EQUAL &&
                   strcmp(desc->source_name, "assertEqual") == 0 &&
                   desc->category == XR_CORE_INTRINSIC_CATEGORY_ASSERTION &&
                   desc->parameter_shape ==
                       XR_CORE_INTRINSIC_PARAMETER_SHAPE_SAME_TYPE_PAIR_OPTIONAL_MESSAGE &&
                   desc->min_arity == 2 && desc->max_arity == 3 &&
                   desc->effect == XR_CORE_INTRINSIC_EFFECT_MAY_PANIC &&
                   desc->flow_rule == XR_CORE_INTRINSIC_FLOW_NONE &&
                   desc->expected_failure_channel == XR_CORE_INTRINSIC_FAILURE_CHANNEL_NONE &&
                   desc->target_applicability == XR_CORE_INTRINSIC_TARGET_ASSERTION_ALL;
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_THROWS:
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_PANICS:
            return desc->id == (desc->semantic_op == XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_THROWS
                                    ? XR_CORE_BUILTIN_ASSERT_THROWS
                                    : XR_CORE_BUILTIN_ASSERT_PANICS) &&
                   strcmp(desc->source_name,
                          desc->semantic_op == XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_THROWS
                              ? "assertThrows"
                              : "assertPanics") == 0 &&
                   desc->category == XR_CORE_INTRINSIC_CATEGORY_ASSERTION &&
                   desc->parameter_shape ==
                       XR_CORE_INTRINSIC_PARAMETER_SHAPE_ACTION_OPTIONAL_MESSAGE &&
                   desc->min_arity == 1 && desc->max_arity == 2 &&
                   desc->effect == XR_CORE_INTRINSIC_EFFECT_INVOKES_ACTION_MAY_PANIC &&
                   desc->flow_rule == XR_CORE_INTRINSIC_FLOW_NONE &&
                   desc->expected_failure_channel ==
                       (desc->semantic_op == XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_THROWS
                            ? XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR
                            : XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC) &&
                   desc->target_applicability == XR_CORE_INTRINSIC_TARGET_ASSERTION_ALL;
        case XR_CORE_INTRINSIC_SEMANTIC_OP_PRINT_GROUP:
            return desc->id == XR_CORE_BUILTIN_PRINT && strcmp(desc->source_name, "print") == 0 &&
                   desc->category == XR_CORE_INTRINSIC_CATEGORY_OUTPUT &&
                   desc->parameter_shape == XR_CORE_INTRINSIC_PARAMETER_SHAPE_VARIADIC_VALUES &&
                   desc->min_arity == 0 && desc->max_arity == UINT16_MAX &&
                   desc->effect == XR_CORE_INTRINSIC_EFFECT_OUTPUT_MAY_PANIC &&
                   desc->flow_rule == XR_CORE_INTRINSIC_FLOW_NONE &&
                   desc->expected_failure_channel == XR_CORE_INTRINSIC_FAILURE_CHANNEL_NONE &&
                   desc->target_applicability == XR_CORE_INTRINSIC_TARGET_OUTPUT_ALL;
        default:
            return false;
    }
}

bool xr_core_intrinsic_registry_validate(char *error, size_t error_size) {
    bool seen_ids[XR_CORE_BUILTIN_ID_LIMIT] = {false};
    bool seen_ops[XR_CORE_INTRINSIC_SEMANTIC_OP_COUNT] = {false};

    if (xr_core_intrinsic_count() != XR_CORE_BUILTIN_COUNT)
        return set_error(error, error_size, "unexpected core intrinsic count: %s", "registry");

    for (size_t i = 0; i < xr_core_intrinsic_count(); i++) {
        const XrCoreIntrinsicDesc *a = &g_core_intrinsics[i];
        if (a->id <= XR_CORE_BUILTIN_NONE || a->id >= XR_CORE_BUILTIN_ID_LIMIT ||
            !a->source_name || !a->source_name[0] || !a->diagnostic_name ||
            !a->diagnostic_name[0] || a->call_form != XR_CORE_INTRINSIC_CALL_FORM_DIRECT_ONLY ||
            a->min_arity > a->max_arity || a->result_shape != XR_CORE_INTRINSIC_RESULT_SHAPE_UNIT ||
            a->effect == XR_CORE_INTRINSIC_EFFECT_NONE ||
            a->effect >= XR_CORE_INTRINSIC_EFFECT_COUNT ||
            a->flow_rule >= XR_CORE_INTRINSIC_FLOW_COUNT ||
            a->expected_failure_channel >= XR_CORE_INTRINSIC_FAILURE_CHANNEL_COUNT ||
            a->semantic_op <= XR_CORE_INTRINSIC_SEMANTIC_OP_NONE ||
            a->semantic_op >= XR_CORE_INTRINSIC_SEMANTIC_OP_COUNT ||
            !descriptor_contract_is_valid(a)) {
            return set_error(error, error_size, "invalid core intrinsic descriptor: %s",
                             a->source_name ? a->source_name : "<unnamed>");
        }
        if (is_removed_source_name(a->source_name))
            return set_error(error, error_size, "removed core intrinsic name is registered: %s",
                             a->source_name);
        if (seen_ids[a->id] || seen_ops[a->semantic_op])
            return set_error(error, error_size, "duplicate core intrinsic identity: %s",
                             a->source_name);
        seen_ids[a->id] = true;
        seen_ops[a->semantic_op] = true;

        if (xr_core_intrinsic_by_id(a->id) != a ||
            xr_core_intrinsic_by_source_name(a->source_name, strlen(a->source_name)) != a)
            return set_error(error, error_size, "dead core intrinsic registry row: %s",
                             a->source_name);

        for (size_t j = i + 1; j < xr_core_intrinsic_count(); j++) {
            if (strcmp(a->source_name, g_core_intrinsics[j].source_name) == 0)
                return set_error(error, error_size, "duplicate core intrinsic source name: %s",
                                 a->source_name);
        }
    }

    for (int id = XR_CORE_BUILTIN_NONE + 1; id < XR_CORE_BUILTIN_ID_LIMIT; id++) {
        if (!seen_ids[id])
            return set_error(error, error_size, "missing core intrinsic stable ID: %s", "registry");
    }
    for (int op = XR_CORE_INTRINSIC_SEMANTIC_OP_NONE + 1;
         op < XR_CORE_INTRINSIC_SEMANTIC_OP_COUNT; op++) {
        if (!seen_ops[op])
            return set_error(error, error_size, "missing core intrinsic semantic operation: %s",
                             "registry");
    }

    if (error && error_size)
        error[0] = '\0';
    return true;
}
