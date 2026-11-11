/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_result_environment_cases.h - Captured identities returned by applications
 */
#ifndef XIR_RESULT_ENVIRONMENT_CASES_H
#define XIR_RESULT_ENVIRONMENT_CASES_H

typedef struct ResultEnvironmentProbe {
    const EffectCheckedRoots *roots;
    uint32_t expected, cells;
} ResultEnvironmentProbe;

static XrXirStatus result_environment_capture(void *context, const XirExecutionSite *site) {
    ResultEnvironmentProbe *probe = context;
    if (site->kind == XIR_EXECUTION_STORABLE) return XR_XIR_OK;
    CHECK(site->kind == XIR_EXECUTION_CALL && site->protocol == 1 && site->declaration == 1);
    XirExecutionArgument cell;
    XrXirStatus status = xir_execution_argument(probe->roots,site,0,&cell);
    if (status != XR_XIR_OK) return status;
    CHECK(cell.cell && cell.function == 3 && cell.value == probe->expected &&
        cell.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT);
    ++probe->cells;
    return XR_XIR_OK;
}

static XrXirStatus result_environment_case(void) {
    XrXirType fn = XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.element=XR_XIR_NEVER,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_CONSTRUCTED_TYPE_BASE},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_CONSTRUCTED_TYPE_BASE,.element=XR_XIR_NEVER,
            .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED}
    };
    XrXirTypes types = {.nodes=nodes,.count=3};
    XrXirType cell = fn+1;
    XrXirInstruction leaf[] = {{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},{.op=XR_XIR_RETURN,.args={0}}};
    XrXirInstruction reader[] = {{.op=XR_XIR_CELL_READ,.type=XR_XIR_CONSTRUCTED_TYPE_BASE,.args={0}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1},{.op=XR_XIR_RETURN,.args={2}}};
    XrXirInstruction getter = {.op=XR_XIR_RETURN,.args={0}};
    uint32_t arguments[] = {1,2,5,6};
    XrXirInstruction caller[] = {{.op=XR_XIR_FUNCTION_REF,.type=XR_XIR_CONSTRUCTED_TYPE_BASE},
        {.op=XR_XIR_CELL_NEW,.type=XR_XIR_CONSTRUCTED_TYPE_BASE+1,.args={0}},
        {.op=XR_XIR_FUNCTION_REF,.type=XR_XIR_CONSTRUCTED_TYPE_BASE,.args={0,1},.immediate=1},
        {.op=XR_XIR_FUNCTION_REF,.type=XR_XIR_CONSTRUCTED_TYPE_BASE+2,.args={1,1},.immediate=2},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_CONSTRUCTED_TYPE_BASE,.immediate=3},
        {.op=XR_XIR_CELL_NEW,.type=XR_XIR_CONSTRUCTED_TYPE_BASE+1,.args={0}},
        {.op=XR_XIR_FUNCTION_REF,.type=XR_XIR_CONSTRUCTED_TYPE_BASE,.args={2,1},.immediate=1},
        {.op=XR_XIR_FUNCTION_REF,.type=XR_XIR_CONSTRUCTED_TYPE_BASE+2,.args={3,1},.immediate=2},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_CONSTRUCTED_TYPE_BASE,.immediate=7},{.op=XR_XIR_RETURN}};
    XrXirBlock blocks[] = {{.count=2},{.count=3},{.count=1},{.count=10}};
    XrXirFunction functions[] = {{.result=XR_XIR_I64,.instructions=leaf,.instruction_count=2},
        {.parameters=&cell,.parameter_count=1,.result=XR_XIR_I64,.instructions=reader,.instruction_count=3},
        {.parameters=&fn,.parameter_count=1,.result=XR_XIR_CONSTRUCTED_TYPE_BASE,.instructions=&getter,.instruction_count=1},
        {.instructions=caller,.instruction_count=10,.operands=arguments,.operand_count=4}};
    for (uint32_t f = 0; f < 4; ++f) {
        functions[f].blocks = blocks+f; functions[f].block_count = 1; functions[f].error_type = XR_XIR_NEVER;
    }
    XrXirFunctionIdentity identities[] = {{0},{.capture_owner=1,.capture_count=1},{.capture_owner=2,.capture_count=1},{0}};
    XrXirSourceModule source = {0};
    XrXirDeclarations declarations = {.modules=&source,.module_count=1,.functions=identities};
    XrXirModule module = {.types=&types,.functions=functions,.function_count=4,.declarations=&declarations};
    EffectCheckedRoots *roots = NULL;
    XrXirStatus status = effect_checked_roots_build(&effect_context,&module,NULL,NULL,NULL,&roots,false);
    if (status == XR_XIR_OK) {
        if (roots->terms) roots->terms->remaining = &effect_context;
        for (uint32_t n = 0; n < 2 && status == XR_XIR_OK; ++n) {
            bool complete = false;
            uint32_t remainder = 0;
            ResultEnvironmentProbe probe = {roots,n ? 5 : 1,0};
            XirExecutionValue value = {3,n ? 8 : 4,XIR_EXECUTION_CURRENT_ENVIRONMENT};
            status = xir_execution_visit_value_footprint_in(roots,&value,result_environment_capture,&probe,&complete,&remainder);
            if (status != XR_XIR_OK) break;
            CHECK(complete && !remainder && probe.cells == 1);
            uint32_t target = roots->storage_origins.offsets[3]+value.value;
            bool contextual = false;
            for (uint32_t e = roots->storage_origins.heads[target]; e != UINT32_MAX; e = roots->storage_origins.edges[e].next) {
                EffectCheckOriginEdge edge = roots->storage_origins.edges[e];
                contextual |= edge.result && edge.function == 2 && edge.application != UINT32_MAX &&
                    edge.value == roots->function[2].result-roots->function[2].first;
            }
            CHECK(contextual);
        }
    }
    effect_checked_roots_free(roots);
    return status;
}

static void result_environment_ownership(void) {
    EffectMark baseline = effect_mark();
    attempts = 0; injected = false;
    CHECK(result_environment_case() == XR_XIR_OK);
    size_t sites = attempts;
    effect_mark_check(baseline);
    for (size_t site = 0; site < sites; ++site) {
        attempts = 0; injected = false; fail_at = site;
        CHECK(result_environment_case() == XR_XIR_OUT_OF_MEMORY && injected);
        fail_at = SIZE_MAX; effect_mark_check(baseline);
    }
    printf("returned environments: contextual Cell identity and %zu allocation failures PASS\n",sites);
}
#endif
