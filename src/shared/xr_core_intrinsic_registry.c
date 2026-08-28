/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_core_intrinsic_registry.c - Core intrinsic registry lookup and validation
 */

#include "xr_core_intrinsic.h"
#include "xr_assertion_plan.h"
#include "xr_print_plan.h"

#include <stdio.h>
#include <string.h>

static const XrCoreIntrinsicDesc g_core_intrinsics[] = {
#define XR_CORE_INTRINSIC(id, stable_id, source_name, category, call_form, parameter_shape,        \
                          min_arity, max_arity, result_shape, effect_kind, flow_rule,              \
                          expected_failure_channel, semantic_op, target_applicability,             \
                          diagnostic_name)                                                         \
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
#include "xr_core_intrinsic.def"
#undef XR_CORE_INTRINSIC
};

_Static_assert(sizeof(g_core_intrinsics) / sizeof(g_core_intrinsics[0]) == XR_CORE_BUILTIN_COUNT,
               "core intrinsic descriptor count must match the stable ID domain");

static const char *const g_removed_core_intrinsic_names[] = {
    "likely", "unlikely", "assert_true", "assert_false", "assert_eq", "assert_ne", "assert_throws",
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

static XrAssertionKind assertion_kind_for_op(XrCoreIntrinsicSemanticOp op) {
    switch (op) {
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_CONDITION:
            return XR_ASSERTION_KIND_CONDITION;
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_EQUAL:
            return XR_ASSERTION_KIND_EQUAL;
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_THROWS:
            return XR_ASSERTION_KIND_THROWS;
        case XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_PANICS:
            return XR_ASSERTION_KIND_PANICS;
        default:
            return XR_ASSERTION_KIND_NONE;
    }
}

static uint32_t assertion_required_capabilities(XrAssertionKind kind) {
    uint32_t capabilities = XR_ASSERTION_CAPABILITY_FAILURE_REPORT;
    if (kind == XR_ASSERTION_KIND_THROWS)
        capabilities |= XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY;
    else if (kind == XR_ASSERTION_KIND_PANICS)
        capabilities |= XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY;
    return capabilities;
}

static bool assertion_target_is_valid(uint32_t target) {
    const uint32_t targets = XR_CORE_INTRINSIC_TARGET_VM | XR_CORE_INTRINSIC_TARGET_AOT_HOSTED |
                             XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_ASSERTION_PROVIDER;
    return (target & targets) != 0 && (target & ~targets) == 0;
}

bool xr_assertion_plan_validate(const XrAssertionPlan *plan) {
    if (!plan || plan->schema_version != XR_ASSERTION_PLAN_SCHEMA_VERSION ||
        !xr_location_is_complete(plan->source) || !assertion_target_is_valid(plan->target) ||
        plan->kind <= XR_ASSERTION_KIND_NONE || plan->kind >= XR_ASSERTION_KIND_COUNT ||
        plan->equality_authority >= XR_ASSERTION_EQUALITY_COUNT ||
        plan->expected_failure_channel >= XR_CORE_INTRINSIC_FAILURE_CHANNEL_COUNT ||
        plan->flow_rule >= XR_CORE_INTRINSIC_FLOW_COUNT ||
        plan->effect <= XR_CORE_INTRINSIC_EFFECT_NONE ||
        plan->effect >= XR_CORE_INTRINSIC_EFFECT_COUNT || plan->evaluation_count != plan->arity ||
        plan->evaluation_count > 3 ||
        (plan->flags & ~XR_ASSERTION_PLAN_FLAG_FAILURE_EDGE_COLD) != 0 ||
        plan->flags != XR_ASSERTION_PLAN_FLAG_FAILURE_EDGE_COLD ||
        (plan->required_capabilities & ~XR_ASSERTION_CAPABILITY_ALL) != 0)
        return false;

    const XrCoreIntrinsicDesc *desc = xr_core_intrinsic_by_id(plan->builtin_id);
    if (!desc || desc->category != XR_CORE_INTRINSIC_CATEGORY_ASSERTION ||
        assertion_kind_for_op(desc->semantic_op) != plan->kind || plan->arity < desc->min_arity ||
        plan->arity > desc->max_arity || (desc->target_applicability & plan->target) == 0 ||
        plan->effect != desc->effect || plan->flow_rule != desc->flow_rule ||
        plan->expected_failure_channel != desc->expected_failure_channel ||
        plan->required_capabilities != assertion_required_capabilities(plan->kind))
        return false;

    const bool has_message = plan->arity == desc->max_arity;
    if (plan->message_operand !=
        (has_message ? (uint8_t) (plan->arity - 1u) : XR_ASSERTION_OPERAND_NONE))
        return false;
    unsigned message_steps = 0;
    for (uint8_t i = 0; i < 3; i++) {
        if (i < plan->evaluation_count) {
            if (plan->evaluation_order[i] <= XR_ASSERTION_EVAL_NONE ||
                plan->evaluation_order[i] >= XR_ASSERTION_EVAL_COUNT)
                return false;
            if (plan->evaluation_order[i] == XR_ASSERTION_EVAL_MESSAGE)
                message_steps++;
        } else if (plan->evaluation_order[i] != XR_ASSERTION_EVAL_NONE) {
            return false;
        }
    }
    if (message_steps != (has_message ? 1u : 0u))
        return false;

    switch (plan->kind) {
        case XR_ASSERTION_KIND_CONDITION:
            return plan->builtin_id == XR_CORE_BUILTIN_ASSERT &&
                   plan->equality_authority == XR_ASSERTION_EQUALITY_NONE &&
                   plan->evaluation_order[0] == XR_ASSERTION_EVAL_CONDITION &&
                   (!has_message || plan->evaluation_order[1] == XR_ASSERTION_EVAL_MESSAGE);
        case XR_ASSERTION_KIND_EQUAL:
            return plan->builtin_id == XR_CORE_BUILTIN_ASSERT_EQUAL &&
                   plan->equality_authority == XR_ASSERTION_EQUALITY_LANGUAGE_DEEP &&
                   plan->evaluation_order[0] == XR_ASSERTION_EVAL_ACTUAL &&
                   plan->evaluation_order[1] == XR_ASSERTION_EVAL_EXPECTED &&
                   (!has_message || plan->evaluation_order[2] == XR_ASSERTION_EVAL_MESSAGE);
        case XR_ASSERTION_KIND_THROWS:
        case XR_ASSERTION_KIND_PANICS:
            return plan->builtin_id == (plan->kind == XR_ASSERTION_KIND_THROWS
                                            ? XR_CORE_BUILTIN_ASSERT_THROWS
                                            : XR_CORE_BUILTIN_ASSERT_PANICS) &&
                   plan->equality_authority == XR_ASSERTION_EQUALITY_NONE &&
                   plan->evaluation_order[0] == XR_ASSERTION_EVAL_ACTION &&
                   (!has_message || plan->evaluation_order[1] == XR_ASSERTION_EVAL_MESSAGE);
        default:
            return false;
    }
}

XrAssertionPlanStatus xr_assertion_plan_build(XrCoreBuiltinId builtin_id, uint16_t arity,
                                              XrLocation source, uint32_t target,
                                              uint32_t available_capabilities,
                                              XrAssertionPlan *out) {
    if (!out || !xr_location_is_complete(source) || !assertion_target_is_valid(target))
        return XR_ASSERTION_PLAN_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    const XrCoreIntrinsicDesc *desc = xr_core_intrinsic_by_id(builtin_id);
    XrAssertionKind kind = desc ? assertion_kind_for_op(desc->semantic_op) : XR_ASSERTION_KIND_NONE;
    if (!desc || desc->category != XR_CORE_INTRINSIC_CATEGORY_ASSERTION ||
        kind == XR_ASSERTION_KIND_NONE)
        return XR_ASSERTION_PLAN_NOT_ASSERTION;
    if (arity < desc->min_arity || arity > desc->max_arity)
        return XR_ASSERTION_PLAN_INVALID_ARITY;
    if ((desc->target_applicability & target) != target)
        return XR_ASSERTION_PLAN_UNSUPPORTED_TARGET;

    uint32_t required_capabilities = assertion_required_capabilities(kind);
    /* A target-neutral semantic plan records what it requires.  Only an exact
     * freestanding plan request may claim provider availability here; the
     * TargetPlan builder later proves mixed/portable plans against the frozen
     * target profile. */
    if (target == XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_ASSERTION_PROVIDER &&
        (available_capabilities & required_capabilities) != required_capabilities)
        return XR_ASSERTION_PLAN_MISSING_CAPABILITY;

    out->schema_version = XR_ASSERTION_PLAN_SCHEMA_VERSION;
    out->builtin_id = builtin_id;
    out->kind = kind;
    out->source = source;
    out->equality_authority = kind == XR_ASSERTION_KIND_EQUAL ? XR_ASSERTION_EQUALITY_LANGUAGE_DEEP
                                                              : XR_ASSERTION_EQUALITY_NONE;
    out->expected_failure_channel = desc->expected_failure_channel;
    out->flow_rule = desc->flow_rule;
    out->effect = desc->effect;
    out->target = target;
    out->required_capabilities = required_capabilities;
    out->flags = XR_ASSERTION_PLAN_FLAG_FAILURE_EDGE_COLD;
    out->arity = arity;
    out->message_operand =
        arity == desc->max_arity ? (uint8_t) (arity - 1u) : XR_ASSERTION_OPERAND_NONE;
    out->evaluation_count = (uint8_t) arity;
    switch (kind) {
        case XR_ASSERTION_KIND_CONDITION:
            out->evaluation_order[0] = XR_ASSERTION_EVAL_CONDITION;
            break;
        case XR_ASSERTION_KIND_EQUAL:
            out->evaluation_order[0] = XR_ASSERTION_EVAL_ACTUAL;
            out->evaluation_order[1] = XR_ASSERTION_EVAL_EXPECTED;
            break;
        case XR_ASSERTION_KIND_THROWS:
        case XR_ASSERTION_KIND_PANICS:
            out->evaluation_order[0] = XR_ASSERTION_EVAL_ACTION;
            break;
        default:
            return XR_ASSERTION_PLAN_INVALID_SCHEMA;
    }
    if (out->message_operand != XR_ASSERTION_OPERAND_NONE)
        out->evaluation_order[out->message_operand] = XR_ASSERTION_EVAL_MESSAGE;
    return xr_assertion_plan_validate(out) ? XR_ASSERTION_PLAN_OK
                                           : XR_ASSERTION_PLAN_INVALID_SCHEMA;
}

bool xr_assertion_classify_action_outcome(const XrAssertionPlan *plan,
                                          XrCoreIntrinsicExpectedFailureChannel observed_channel,
                                          XrAssertionFailureKind *failure_kind) {
    if (!failure_kind || !xr_assertion_plan_validate(plan) ||
        (plan->kind != XR_ASSERTION_KIND_THROWS && plan->kind != XR_ASSERTION_KIND_PANICS) ||
        observed_channel >= XR_CORE_INTRINSIC_FAILURE_CHANNEL_COUNT)
        return false;
    if (observed_channel == plan->expected_failure_channel) {
        *failure_kind = XR_ASSERTION_FAILURE_NONE;
        return true;
    }
    if (plan->kind == XR_ASSERTION_KIND_THROWS) {
        *failure_kind = observed_channel == XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC
                            ? XR_ASSERTION_FAILURE_UNEXPECTED_PANIC
                            : XR_ASSERTION_FAILURE_EXPECTED_TYPED_ERROR;
    } else {
        *failure_kind = observed_channel == XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR
                            ? XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR
                            : XR_ASSERTION_FAILURE_EXPECTED_PANIC;
    }
    return true;
}

bool xr_assertion_classify_action_result(const XrAssertionPlan *plan,
                                         XrAssertionActionOutcome outcome,
                                         XrAssertionFailureKind *failure_kind) {
    if (!failure_kind || !xr_assertion_plan_validate(plan) ||
        (plan->kind != XR_ASSERTION_KIND_THROWS && plan->kind != XR_ASSERTION_KIND_PANICS))
        return false;
    return xr_assertion_classify_action_channels(plan->expected_failure_channel, outcome,
                                                 failure_kind);
}

static bool print_target_is_valid(uint32_t target) {
    const uint32_t targets = XR_CORE_INTRINSIC_TARGET_VM | XR_CORE_INTRINSIC_TARGET_AOT_HOSTED |
                             XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_OUTPUT_PROVIDER;
    return (target & targets) != 0 && (target & ~targets) == 0;
}

bool xr_print_plan_validate(const XrPrintPlan *plan) {
    if (!plan || plan->schema_version != XR_PRINT_PLAN_SCHEMA_VERSION ||
        !xr_location_is_complete(plan->source) || !print_target_is_valid(plan->target) ||
        plan->separator != XR_PRINT_SEPARATOR_SPACE ||
        plan->terminator != XR_PRINT_TERMINATOR_NEWLINE ||
        plan->effect <= XR_CORE_INTRINSIC_EFFECT_NONE ||
        plan->effect >= XR_CORE_INTRINSIC_EFFECT_COUNT || plan->flags != XR_PRINT_PLAN_FLAG_NONE ||
        (plan->required_capabilities & ~XR_PRINT_CAPABILITY_ALL) != 0 ||
        plan->required_capabilities != XR_PRINT_CAPABILITY_OUTPUT_WRITE)
        return false;

    const XrCoreIntrinsicDesc *desc = xr_core_intrinsic_by_id(plan->builtin_id);
    return desc && desc->category == XR_CORE_INTRINSIC_CATEGORY_OUTPUT &&
           desc->semantic_op == XR_CORE_INTRINSIC_SEMANTIC_OP_PRINT_GROUP &&
           plan->arity >= desc->min_arity && plan->arity <= desc->max_arity &&
           (desc->target_applicability & plan->target) == plan->target &&
           plan->effect == desc->effect;
}

XrPrintPlanStatus xr_print_plan_build(XrCoreBuiltinId builtin_id, uint16_t arity, XrLocation source,
                                      uint32_t target, uint32_t available_capabilities,
                                      XrPrintPlan *out) {
    if (!out || !xr_location_is_complete(source) || !print_target_is_valid(target))
        return XR_PRINT_PLAN_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    const XrCoreIntrinsicDesc *desc = xr_core_intrinsic_by_id(builtin_id);
    if (!desc || desc->category != XR_CORE_INTRINSIC_CATEGORY_OUTPUT ||
        desc->semantic_op != XR_CORE_INTRINSIC_SEMANTIC_OP_PRINT_GROUP)
        return XR_PRINT_PLAN_NOT_OUTPUT;
    if (arity < desc->min_arity || arity > desc->max_arity)
        return XR_PRINT_PLAN_INVALID_ARITY;
    if ((desc->target_applicability & target) != target)
        return XR_PRINT_PLAN_UNSUPPORTED_TARGET;

    /* A target-neutral plan records what it requires.  Only an exact
     * freestanding request may claim provider availability here; the TargetPlan
     * builder later proves mixed/portable plans against the frozen profile. */
    if (target == XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_OUTPUT_PROVIDER &&
        (available_capabilities & XR_PRINT_CAPABILITY_OUTPUT_WRITE) == 0)
        return XR_PRINT_PLAN_MISSING_CAPABILITY;

    out->schema_version = XR_PRINT_PLAN_SCHEMA_VERSION;
    out->builtin_id = builtin_id;
    out->source = source;
    out->separator = XR_PRINT_SEPARATOR_SPACE;
    out->terminator = XR_PRINT_TERMINATOR_NEWLINE;
    out->effect = desc->effect;
    out->target = target;
    out->required_capabilities = XR_PRINT_CAPABILITY_OUTPUT_WRITE;
    out->flags = XR_PRINT_PLAN_FLAG_NONE;
    out->arity = arity;
    return xr_print_plan_validate(out) ? XR_PRINT_PLAN_OK : XR_PRINT_PLAN_INVALID_SCHEMA;
}

bool xr_core_intrinsic_registry_validate(char *error, size_t error_size) {
    bool seen_ids[XR_CORE_BUILTIN_ID_LIMIT] = {false};
    bool seen_ops[XR_CORE_INTRINSIC_SEMANTIC_OP_COUNT] = {false};

    if (xr_core_intrinsic_count() != XR_CORE_BUILTIN_COUNT)
        return set_error(error, error_size, "unexpected core intrinsic count: %s", "registry");

    for (size_t i = 0; i < xr_core_intrinsic_count(); i++) {
        const XrCoreIntrinsicDesc *a = &g_core_intrinsics[i];
        if (a->id <= XR_CORE_BUILTIN_NONE || a->id >= XR_CORE_BUILTIN_ID_LIMIT || !a->source_name ||
            !a->source_name[0] || !a->diagnostic_name || !a->diagnostic_name[0] ||
            a->call_form != XR_CORE_INTRINSIC_CALL_FORM_DIRECT_ONLY ||
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

        if (a->category == XR_CORE_INTRINSIC_CATEGORY_ASSERTION) {
            XrLocation source = {"<core-intrinsic-registry>", 1, 1, 1, 1};
            XrAssertionPlan plan;
            if (xr_assertion_plan_build(a->id, a->max_arity, source, XR_CORE_INTRINSIC_TARGET_VM,
                                        XR_ASSERTION_CAPABILITY_NONE,
                                        &plan) != XR_ASSERTION_PLAN_OK ||
                !xr_assertion_plan_validate(&plan))
                return set_error(error, error_size,
                                 "core intrinsic cannot produce an assertion plan: %s",
                                 a->source_name);
        }

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
    for (int op = XR_CORE_INTRINSIC_SEMANTIC_OP_NONE + 1; op < XR_CORE_INTRINSIC_SEMANTIC_OP_COUNT;
         op++) {
        if (!seen_ops[op])
            return set_error(error, error_size, "missing core intrinsic semantic operation: %s",
                             "registry");
    }

    if (error && error_size)
        error[0] = '\0';
    return true;
}
