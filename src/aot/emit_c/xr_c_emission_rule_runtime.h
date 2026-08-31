/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_rule_runtime.h - Typed C emission rule evaluation boundary
 *
 * KEY CONCEPT:
 *   A structural locator fills raw, immutable facts without deciding whether
 *   a recipe is valid. Independent generated evaluators then build and verify
 *   the decision. Neither evaluator may call the other.
 */

#ifndef XR_C_EMISSION_RULE_RUNTIME_H
#define XR_C_EMISSION_RULE_RUNTIME_H

#include "xr_c_emission_schema.h"
#include "xr_c_emission_rule_ids_gen.h"
#include "../../ir/xi.h"
#include "../../plan/semantic/xr_semantic_array_member_shape.h"
#include "../../plan/target/xr_target_plan.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum XrCEmissionRuleMatch {
    XR_C_EMISSION_RULE_NOT_APPLICABLE = 0,
    XR_C_EMISSION_RULE_EXACT,
    XR_C_EMISSION_RULE_MALFORMED,
} XrCEmissionRuleMatch;

typedef struct XrCEmissionRuleFacts {
    uint16_t member;
    uint16_t opcode;
    uint16_t operand_count;
    uint16_t result_kind;
    uint8_t intrinsic;
    uint8_t element_access;
    uint8_t reference_action;
    uint8_t reference_drop;
    uint8_t element_managed_reference;
    uint8_t call_convention;
    uint8_t target_kind;
    uint8_t layout_kind;
    uint8_t call_storage;
    uint8_t layout_storage;
    uint8_t argument_ownership[2];
    uint8_t argument_storage[2];
    uint16_t caller_register_kind[2];
    uint16_t caller_memory_kind[2];
    bool operation_result_bound;
    bool call_result_bound;
    bool arguments_structurally_exact;
} XrCEmissionRuleFacts;

typedef struct XrCEmissionRuleDecision {
    uint16_t rule_id;
    uint8_t recipe;
    uint8_t rep;
    uint8_t storage;
    const char *symbol;
} XrCEmissionRuleDecision;

#include "xr_c_emission_rule_build_gen.inc.c"
#include "xr_c_emission_rule_verify_gen.inc.c"

#endif  // XR_C_EMISSION_RULE_RUNTIME_H
