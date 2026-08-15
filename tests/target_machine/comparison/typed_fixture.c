/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_fixture.c - Private typed TargetPlan comparison fixture
 *
 * KEY CONCEPT:
 *   The fixture constructs the same scalar operation named by the comparison
 *   manifest, freezes it through the production plan builders, and invokes the
 *   off-product typed dispatcher without adding a product execution mode.
 */

#include "base/xmalloc.h"
#include "ir/xi.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/target/xr_target_builder.h"
#include "runtime/value/xtype.h"
#include "vm/xr_typed_dispatch.h"
#include "plan/target_profile_test_fixture.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum ComparisonShape {
    COMPARISON_BINARY,
    COMPARISON_RELATION_CFG,
} ComparisonShape;

typedef struct ComparisonCase {
    const char *name;
    ComparisonShape shape;
    XiOp operation;
} ComparisonCase;

static XrType comparison_int = {
    .kind = XR_KIND_INT,
    .id = 1,
    .frozen = true,
};

static XrType comparison_bool = {
    .kind = XR_KIND_BOOL,
    .id = 2,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};

static const ComparisonCase comparison_cases[] = {
    {"add", COMPARISON_BINARY, XI_ADD},
    {"sub", COMPARISON_BINARY, XI_SUB},
    {"mul", COMPARISON_BINARY, XI_MUL},
    {"bit-and", COMPARISON_BINARY, XI_BAND},
    {"bit-or", COMPARISON_BINARY, XI_BOR},
    {"bit-xor", COMPARISON_BINARY, XI_BXOR},
    {"shift-left", COMPARISON_BINARY, XI_SHL},
    {"shift-right", COMPARISON_BINARY, XI_SHR},
    {"divide", COMPARISON_BINARY, XI_DIV},
    {"modulo", COMPARISON_BINARY, XI_MOD},
    {"divide-by-zero", COMPARISON_BINARY, XI_DIV},
    {"modulo-by-zero", COMPARISON_BINARY, XI_MOD},
    {"cfg-eq", COMPARISON_RELATION_CFG, XI_EQ},
    {"cfg-ne", COMPARISON_RELATION_CFG, XI_NE},
    {"cfg-lt", COMPARISON_RELATION_CFG, XI_LT},
    {"cfg-le", COMPARISON_RELATION_CFG, XI_LE},
    {"cfg-gt", COMPARISON_RELATION_CFG, XI_GT},
    {"cfg-ge", COMPARISON_RELATION_CFG, XI_GE},
};

static const ComparisonCase *find_case(const char *name) {
    size_t count = sizeof(comparison_cases) / sizeof(comparison_cases[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(comparison_cases[i].name, name) == 0)
            return &comparison_cases[i];
    }
    return NULL;
}

static bool add_parameters(XiFunc *function, XiBlock *entry) {
    function->nparams = 2;
    function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    if (!function->params)
        return false;
    entry->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &comparison_int);
    function->params[1] = xi_param(function, entry, 1, &comparison_int);
    return function->params[0] != NULL && function->params[1] != NULL;
}

static XiFunc *build_binary_function(const ComparisonCase *spec) {
    XiFunc *function = xi_func_new("target_machine_comparison", &comparison_int);
    if (!function)
        return NULL;
    XiBlock *entry = xi_block_new(function);
    if (!entry || !add_parameters(function, entry)) {
        xi_func_free(function);
        return NULL;
    }
    XiValue *result = xi_value_new(function, entry, spec->operation, &comparison_int, 2);
    if (!result) {
        xi_func_free(function);
        return NULL;
    }
    result->args[0] = function->params[0];
    result->args[1] = function->params[1];
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    return function;
}

static XiFunc *build_relation_function(const ComparisonCase *spec) {
    XiFunc *function = xi_func_new("target_machine_cfg_comparison", &comparison_int);
    if (!function)
        return NULL;
    XiBlock *entry = xi_block_new(function);
    XiBlock *taken = xi_block_new(function);
    XiBlock *untaken = xi_block_new(function);
    if (!entry || !taken || !untaken || !add_parameters(function, entry)) {
        xi_func_free(function);
        return NULL;
    }
    XiValue *relation = xi_value_new(function, entry, spec->operation, &comparison_bool, 2);
    XiValue *sum = xi_value_new(function, taken, XI_ADD, &comparison_int, 2);
    XiValue *difference = xi_value_new(function, untaken, XI_SUB, &comparison_int, 2);
    if (!relation || !sum || !difference) {
        xi_func_free(function);
        return NULL;
    }
    relation->args[0] = function->params[0];
    relation->args[1] = function->params[1];
    sum->args[0] = function->params[0];
    sum->args[1] = function->params[1];
    difference->args[0] = function->params[0];
    difference->args[1] = function->params[1];
    xi_block_set_if(entry, relation, taken, untaken);
    xi_block_set_return(taken, sum);
    xi_block_set_return(untaken, difference);
    taken->sealed = true;
    untaken->sealed = true;
    function->stage = XI_STAGE_OPTIMIZED;
    return function;
}

static XiFunc *build_function(const ComparisonCase *spec) {
    return spec->shape == COMPARISON_RELATION_CFG ? build_relation_function(spec)
                                                  : build_binary_function(spec);
}

static bool parse_i64(const char *text, int64_t *value) {
    char *end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (!text[0] || !end || *end != '\0')
        return false;
    *value = (int64_t) parsed;
    return true;
}

static const char *program_error(XrTypedDispatchStatus status) {
    switch (status) {
        case XR_TYPED_DISPATCH_DIVIDE_BY_ZERO:
            return "division-by-zero";
        case XR_TYPED_DISPATCH_MODULO_BY_ZERO:
            return "modulo-by-zero";
        default:
            return NULL;
    }
}

static void print_observation(int64_t value, const char *error) {
    if (error) {
        printf("{\"value\":null,\"error\":\"%s\","
               "\"termination\":\"error\",",
               error);
    } else {
        printf("{\"value\":%lld,\"error\":null,"
               "\"termination\":\"returned\",",
               (long long) value);
    }
    puts("\"destruction\":{\"allocations\":0,\"releases\":0,"
         "\"drops\":0},\"lifecycle\":[],\"trace\":[]}");
}

static int execute_case(const ComparisonCase *spec, const int64_t arguments[2]) {
    XiFunc *function = build_function(spec);
    if (!function) {
        fputs("typed comparison fixture could not build Xi input\n", stderr);
        return 2;
    }
    XrSemanticPlan *semantic = NULL;
    XrTargetProfile *profile = NULL;
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    xi_func_free(function);
    if (built)
        profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    if (built && profile)
        built = xr_target_plan_build(semantic, profile, &plan, error, sizeof(error));
    if (!built || !profile || !plan) {
        fprintf(stderr, "typed comparison fixture plan build failed: %s\n",
                error[0] ? error : "profile allocation failed");
        xr_target_plan_free(plan);
        xr_target_profile_free(profile);
        xr_semantic_plan_free(semantic);
        return 2;
    }

    XrFingerprint fingerprint = xr_target_plan_fingerprint(plan);
    int64_t value = 0;
    XrTypedDispatchStatus status =
        xr_typed_dispatch_execute_i64(plan, &fingerprint, 0, arguments, 2, &value);
    const char *execution_error = program_error(status);
    if (status == XR_TYPED_DISPATCH_OK || execution_error)
        print_observation(value, execution_error);
    else
        fprintf(stderr, "typed comparison fixture dispatch failed: %d\n", (int) status);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    return status == XR_TYPED_DISPATCH_OK || execution_error ? 0 : 2;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fputs("usage: typed_fixture <case> <left> <right>\n", stderr);
        return 2;
    }
    const ComparisonCase *spec = find_case(argv[1]);
    int64_t arguments[2] = {0};
    if (!spec || !parse_i64(argv[2], &arguments[0]) || !parse_i64(argv[3], &arguments[1])) {
        fputs("typed comparison fixture received an unknown case or argument\n", stderr);
        return 2;
    }
    return execute_case(spec, arguments);
}
