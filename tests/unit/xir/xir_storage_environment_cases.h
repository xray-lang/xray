/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_storage_environment_cases.h - Bounded hidden callable environments
 *
 * Real function-reference and execution bindings reach the hidden use point.
 * Borrowed, opaque and incomplete inputs cannot establish an empty environment.
 */
#ifndef XIR_STORAGE_ENVIRONMENT_CASES_H
#define XIR_STORAGE_ENVIRONMENT_CASES_H

typedef enum StorageEnvironmentCaseKind {
    STORAGE_ENVIRONMENT_CAPTURE, STORAGE_ENVIRONMENT_OPAQUE,
    STORAGE_ENVIRONMENT_UNBOUND, STORAGE_ENVIRONMENT_CYCLE, STORAGE_ENVIRONMENT_CONTRACT,
    STORAGE_ENVIRONMENT_ESCAPING_CONTRACT
} StorageEnvironmentCaseKind;

typedef struct StorageEnvironmentOriginProbe {
    EffectCheckedRoots *owner;
    XrXirType captured;
    bool opaque, hidden;
    uint32_t count;
} StorageEnvironmentOriginProbe;

static XrXirStatus storage_environment_origin_oracle(void *opaque, const XirExecutionSite *site) {
    StorageEnvironmentOriginProbe *probe = opaque;
    if (probe->hidden) {
        CHECK(site->kind == XIR_EXECUTION_CAPTURE_INPUT);
        XirExecutionArgument input;
        XirExecutionParameter parameter;
        CHECK(xir_execution_argument(probe->owner, site, 0, &input) == XR_XIR_OK);
        CHECK(xir_execution_parameter(probe->owner, site, 0, &parameter) == XR_XIR_OK);
        CHECK(input.function == 3 && !input.value && parameter.hidden && !parameter.cell);
    } else if (probe->opaque) CHECK(site->kind == XIR_EXECUTION_STORABLE);
    else {
        CHECK(site->kind == XIR_EXECUTION_CALL && site->protocol == 1 &&
            site->declaration == 1 && site->argument_count == 1);
        XirExecutionArgument actual;
        CHECK(xir_execution_argument(probe->owner, site, 0, &actual) == XR_XIR_OK);
        CHECK(!actual.function && !actual.value && actual.type == probe->captured);
        CHECK(actual.cell == xr_xir_type_is_cell(xir_execution_types(probe->owner), actual.type));
    }
    ++probe->count;
    return XR_XIR_OK;
}

static XrXirStatus storage_environment_capture_oracle(void *opaque, const XirStorageCapture *capture) {
    uint32_t *count = opaque;
    if (capture->kind != XIR_STORAGE_CAPTURE_HIDDEN_FN || capture->function != 3 ||
        capture->identity.function != 3 || capture->identity.value ||
        capture->contents == UINT32_MAX || capture->exposure != UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    ++*count;
    return XR_XIR_OK;
}

static void storage_environment_guard_case(const char *name, XrXirType captured,
    StorageEnvironmentCaseKind kind, uint32_t expected_remainder) {
    EffectMark baseline = effect_mark();
    XrXirType callable = XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_CALLABLE, .result=XR_XIR_UNIT, .element=XR_XIR_NEVER,
            .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_SLICE, .element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_PTR, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TUPLE},
        {.kind=XR_XIR_TYPE_CELL, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE, .result=XR_XIR_CONSTRUCTED_TYPE_BASE, .element=XR_XIR_NEVER,
            .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CALLABLE, .result=XR_XIR_UNIT, .element=XR_XIR_NEVER,
            .flags=XR_XIR_CALLABLE_ROOT_NONE | XR_XIR_CALLABLE_NO_SUSPEND | XR_XIR_CALLABLE_C_ABI}
    };
    XrXirCallableParameter tuple = {.type=XR_XIR_CONSTRUCTED_TYPE_BASE + 1};
    nodes[3].parameters = &tuple; nodes[3].parameter_count = 1;
    XrXirTypes types = {.nodes=nodes, .count=7};
    XrXirType outer_type = kind == STORAGE_ENVIRONMENT_OPAQUE ? callable + 5 : captured;
    uint32_t outer_arguments[] = {0, 1}, forwarded = 0;
    XrXirInstruction outer[] = {
        {.op=XR_XIR_FUNCTION_REF, .type=callable, .args={0, 1}, .immediate=1},
        {.op=XR_XIR_CALL, .args={1, 1}, .immediate=2},
        {.op=XR_XIR_RETURN}
    };
    XrXirInstruction ret = {.op=XR_XIR_RETURN};
    XrXirInstruction forward[] = {
        {.op=XR_XIR_FUNCTION_REF, .type=callable, .args={0, 1}, .immediate=3},
        {.op=XR_XIR_CALL_INDIRECT, .immediate=1},
        {.op=XR_XIR_RETURN}
    };
    XrXirInstruction hidden[] = {{.op=XR_XIR_CALL_INDIRECT}, {.op=XR_XIR_RETURN}};
    XrXirBlock blocks[] = {{.count=3}, {.count=1}, {.count=3}, {.count=2}};
    XrXirFunction functions[] = {
        {.parameters=&outer_type, .parameter_count=1, .instructions=outer, .instruction_count=3,
            .operands=outer_arguments, .operand_count=2},
        {.parameters=&captured, .parameter_count=1, .instructions=&ret, .instruction_count=1},
        {.parameters=&callable, .parameter_count=1, .instructions=forward, .instruction_count=3,
            .operands=&forwarded, .operand_count=1},
        {.parameters=&callable, .parameter_count=1, .instructions=hidden, .instruction_count=2}
    };
    for (uint32_t f = 0; f < 4; ++f) {
        functions[f].blocks = blocks + f; functions[f].block_count = 1;
        functions[f].error_type = XR_XIR_NEVER;
    }
    if (kind == STORAGE_ENVIRONMENT_OPAQUE)
        outer[0] = (XrXirInstruction){.op=XR_XIR_CALL_INDIRECT, .type=callable};
    if (kind == STORAGE_ENVIRONMENT_UNBOUND) {
        functions[2].instructions = &ret; functions[2].instruction_count = blocks[2].count = 1;
        functions[2].operands = NULL; functions[2].operand_count = 0;
    }
    if (kind == STORAGE_ENVIRONMENT_CYCLE)
        forward[1] = (XrXirInstruction){.op=XR_XIR_CALL, .args={0, 1}, .immediate=2};
    uint32_t escaping_parameter=0;
    if (kind == STORAGE_ENVIRONMENT_CONTRACT || kind == STORAGE_ENVIRONMENT_ESCAPING_CONTRACT) {
        outer[1] = ret;
        functions[0].instruction_count = blocks[0].count = 2;
    }
    if (kind == STORAGE_ENVIRONMENT_ESCAPING_CONTRACT) {
        functions[2].escaping_parameters=&escaping_parameter;
        functions[2].escaping_parameter_count=1;
    }
    XrXirConstraint constraint = {0};
    XrXirGeneric generics[4] = {{0}};
    if (captured == XR_XIR_TYPE_PARAMETER_BASE) {
        generics[0] = (XrXirGeneric){.constraints=&constraint, .parameter_count=1,
            .arguments=&captured, .argument_count=1};
        generics[1] = (XrXirGeneric){.constraints=&constraint, .parameter_count=1};
        outer[0].type_arguments[1] = 1;
    }
    XrXirFunctionIdentity identities[] = {{0}, {.capture_owner=1, .capture_count=1},
        {0}, {.capture_owner=3, .capture_count=1}};
    XrXirSourceModule source_module = {0};
    XrXirDeclarations declarations = {.modules=&source_module, .module_count=1, .functions=identities};
    XrXirModule module = {.types=&types, .functions=functions, .function_count=4,
        .declarations=&declarations, .generics=generics};
    EffectCheckedRoots *roots = NULL;
    XrXirStatus status = effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, false);
    if (status != XR_XIR_OK) fprintf(stderr,"environment %s type%u construction status%u\n",name,(uint32_t)captured,status);
    CHECK(status == XR_XIR_OK && roots && roots->storage);
    if (roots->terms) roots->terms->remaining = &effect_context;
    /* Establish the exact ROOT path before testing storage's environment
     * guard. A Source borrow gate or malformed body cannot satisfy this test. */
    StorageEnvironmentOriginProbe probe = {roots, captured, kind == STORAGE_ENVIRONMENT_OPAQUE, false, 0};
    XirExecutionValue origin = {0, 1, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    bool complete = false;
    uint32_t remainder = UINT32_MAX;
    CHECK(xir_execution_visit_value_environment_inputs_in(roots, &origin, storage_environment_origin_oracle,
        &probe, &complete, &remainder) == XR_XIR_OK && complete && !remainder && probe.count == 1);
    probe.hidden = true; probe.count = 0;
    origin = (XirExecutionValue){3, 0, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    CHECK(xir_execution_visit_value_environment_inputs_in(roots, &origin, storage_environment_origin_oracle,
        &probe, &complete, &remainder) == XR_XIR_OK && complete && !remainder && probe.count == 1);
    bool tested = false;
    for (uint32_t round = 0; round < 32 && !tested; ++round) {
        CHECK(xir_execution_refresh_storage(roots) == XR_XIR_OK);
        uint32_t captures = 0;
        XirStorageValuePoint point = {3, 0, {3, 0, XIR_EXECUTION_CURRENT_ENVIRONMENT}, XIR_STORAGE_BEFORE};
        status = xir_storage_flow_visit_value_captures(roots->storage, &point, XIR_STORAGE_QUERY_CONTENTS,
            storage_environment_capture_oracle, &captures);
        remainder = xir_storage_flow_remainder(roots->storage);
        if (expected_remainder && status == XR_XIR_UNRESOLVED && (remainder & expected_remainder)) tested = true;
        else {
            if (status != XR_XIR_OK && (status != XR_XIR_UNRESOLVED || remainder != XIR_EXECUTION_REMAINDER_STORAGE))
                fprintf(stderr,"environment %s type%u round%u status%u remainder%u\n",name,(uint32_t)captured,round,status,remainder);
            CHECK(status == XR_XIR_OK || (status == XR_XIR_UNRESOLVED && remainder == XIR_EXECUTION_REMAINDER_STORAGE));
            bool changed = false;
            CHECK(xir_storage_flow_link(roots->storage, roots, &changed) == XR_XIR_OK);
            if (status == XR_XIR_OK && !changed) {
                CHECK(!expected_remainder && captures == 1);
                tested = true;
            }
        }
    }
    CHECK(tested);
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
}

static void storage_environment_guard_cases(void) {
    const XrXirType captures[] = {XR_XIR_UNIT, XR_XIR_BOOL, XR_XIR_RUNE, XR_XIR_I64, XR_XIR_F64, XR_XIR_STRING,
        XR_XIR_CONSTRUCTED_TYPE_BASE + 6};
    for (uint32_t n = 0; n < sizeof(captures) / sizeof(captures[0]); ++n)
        storage_environment_guard_case("closed", captures[n], STORAGE_ENVIRONMENT_CAPTURE, 0);
    const XrXirType rejected[] = {XR_XIR_CONSTRUCTED_TYPE_BASE + 1, XR_XIR_CONSTRUCTED_TYPE_BASE + 2,
        XR_XIR_CONSTRUCTED_TYPE_BASE + 3, XR_XIR_TYPE_PARAMETER_BASE, XR_XIR_CONSTRUCTED_TYPE_BASE + 4};
    for (uint32_t n = 0; n < sizeof(rejected) / sizeof(rejected[0]); ++n)
        storage_environment_guard_case("borrowed-or-generic", rejected[n], STORAGE_ENVIRONMENT_CAPTURE,
            XIR_EXECUTION_REMAINDER_OPAQUE);
    storage_environment_guard_case("opaque-callee", XR_XIR_UNIT, STORAGE_ENVIRONMENT_OPAQUE, XIR_EXECUTION_REMAINDER_OPAQUE);
    storage_environment_guard_case("unbound", XR_XIR_I64, STORAGE_ENVIRONMENT_UNBOUND, XIR_EXECUTION_REMAINDER_PARAMETER);
    storage_environment_guard_case("cycle", XR_XIR_I64, STORAGE_ENVIRONMENT_CYCLE, XIR_EXECUTION_REMAINDER_PARAMETER);
    storage_environment_guard_case("declaration-contract", XR_XIR_I64, STORAGE_ENVIRONMENT_CONTRACT, 0);
    storage_environment_guard_case("escaping-declaration-contract", XR_XIR_I64, STORAGE_ENVIRONMENT_ESCAPING_CONTRACT, 0);
}

#endif
