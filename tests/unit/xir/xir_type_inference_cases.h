/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_type_inference_cases.h - Independent shape inference expectations
 *
 * KEY CONCEPT:
 *   Fixed prefix, caller symbols and inferred own binders never share authority.
 */
#ifndef XIR_TYPE_INFERENCE_CASES_H
#define XIR_TYPE_INFERENCE_CASES_H
/* Test amalgamation includes the candidate implementation under allocation hooks. */
typedef struct InferenceTestBatch {
    const XrXirTypes *types;
    const XrXirType *prefix;
    uint32_t prefix_count, own_count, caller_parameter_count;
    const XrXirInferencePair *pairs;
    uint32_t pair_count;
} InferenceTestBatch;
/* Test orchestration only; production exposes the single incremental state. */
static XrXirStatus inference_test_run(const InferenceTestBatch *batch,
    XrXirCompileContext *budget, XrXirType *output, uint32_t count) {
    XrXirInferenceRequest request = {batch->types,batch->prefix,batch->prefix_count,
        batch->own_count,batch->caller_parameter_count, NULL};
    XrXirInferenceState *state = NULL;
    XrXirStatus status = xr_xir_compile_inference_begin(budget, &request, &state);
    for (uint32_t p = 0; status == XR_XIR_OK && p < batch->pair_count; ++p)
        status = xr_xir_compile_inference_observe(state, batch->types, batch->pairs[p]);
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_finalize(state, batch->types, output, count);
    xr_xir_compile_inference_dispose(state); return status;
}
static void type_inference_scalar_cases(void) {
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType p1 = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    XrXirType prefix = XR_XIR_STRING, output[2] = {XR_XIR_BOOL,XR_XIR_BOOL};
    XrXirInferencePair pairs[3] = {{p0,XR_XIR_STRING},{p1,XR_XIR_I64},{p1,XR_XIR_I64}};
    InferenceTestBatch request = {NULL,&prefix,1,1,2,pairs,3};
    for (uint32_t mode = 0; mode < 5; ++mode) {
        pairs[0].actual = XR_XIR_STRING; pairs[1].actual = pairs[2].actual = XR_XIR_I64;
        if (mode==1) pairs[2].actual = XR_XIR_STRING;
        if (mode==2) pairs[0].actual = XR_XIR_BOOL;
        if (mode==3) pairs[1].actual = pairs[2].actual = p0;
        if (mode==4) pairs[1].actual = pairs[2].actual = XR_XIR_UNIT;
        output[0] = output[1] = XR_XIR_BOOL;
        XrXirCompileContext budget = interface_context_default();
        XrXirStatus expected = mode==0 || mode==3 ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        CHECK(inference_test_run(&request,&budget,output,2)==expected);
        if (expected==XR_XIR_OK) CHECK(output[0]==XR_XIR_STRING && output[1]==(mode==3 ? p0 : XR_XIR_I64));
        else CHECK(output[0]==XR_XIR_BOOL && output[1]==XR_XIR_BOOL);
        CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
    }
    request.pair_count = 1; pairs[0].actual = XR_XIR_STRING;
    XrXirCompileContext budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,output,2)==XR_XIR_BAD_TYPE); /* phantom own */
}
static void type_inference_nested_cases(void) {
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirTypeNode nodes[4] = {0};
    for (uint32_t i = 0; i < 4; ++i) nodes[i].kind = XR_XIR_TYPE_ARRAY;
    nodes[0].element = p0; nodes[0].parameter_span = 1;
    nodes[1].element = member_case_type(0); nodes[1].parameter_span = 1;
    nodes[2].element = XR_XIR_STRING; nodes[3].element = member_case_type(2);
    XrXirTypes types = {nodes,4,NULL,NULL};
    XrXirInferencePair pair = {member_case_type(1),member_case_type(3)};
    InferenceTestBatch request = {&types,NULL,0,1,0,&pair,1};
    XrXirType output = XR_XIR_BOOL;
    XrXirCompileContext budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,&output,1)==XR_XIR_OK && output==XR_XIR_STRING);
    pair.actual = member_case_type(2); budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,&output,1)==XR_XIR_BAD_TYPE);
    pair.actual = member_case_type(3);
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        attempts = 0; fail_at = site ? site-1 : SIZE_MAX;
        output = XR_XIR_BOOL; budget = interface_context_default();
        CHECK(inference_test_run(&request,&budget,&output,1)==(site ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        if (site) CHECK(output==XR_XIR_BOOL); else sites = attempts;
        CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
    }
    fail_at = SIZE_MAX; printf("Nested type inference: %zu allocation failure sites\n",sites);
    for (uint32_t mode = 0; mode < 3; ++mode) {
        budget = interface_context_default(); output = XR_XIR_BOOL;
        if (!mode) budget = interface_context_limited(STAGE_ALLOCATED_BYTES,STAGE_LIVE_BYTES,0); else if (mode==1) budget = interface_context_limited(STAGE_ALLOCATED_BYTES,0,STAGE_WORK); else budget.limits.frame_bytes = 0;
        CHECK(inference_test_run(&request,&budget,&output,1)==XR_XIR_BUDGET);
        CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes && output==XR_XIR_BOOL);
    }
}
static void type_inference_callable_cases(void) {
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter formal[2] = {{p0,0},{p0,0}}, actual[2] = {{XR_XIR_I64,0},{XR_XIR_I64,0}};
    XrXirTypeNode nodes[2] = {0};
    for (uint32_t n = 0; n < 2; ++n) { nodes[n].kind = XR_XIR_TYPE_CALLABLE; nodes[n].parameter_count = 2; }
    nodes[0].parameters = formal; nodes[0].result = p0; nodes[0].parameter_span = 1;
    nodes[1].parameters = actual; nodes[1].result = XR_XIR_I64;
    XrXirTypes types = {nodes,2,NULL,NULL};
    XrXirInferencePair pair = {member_case_type(0),member_case_type(1)};
    InferenceTestBatch request = {&types,NULL,0,1,0,&pair,1};
    for (uint32_t attack = 0; attack < 4; ++attack) {
        actual[1].type = attack==1 ? XR_XIR_STRING : XR_XIR_I64;
        nodes[1].parameter_count = attack==2 ? 1 : 2;
        nodes[1].flags = attack==3 ? XR_XIR_CALLABLE_NO_SUSPEND : 0;
        XrXirType output = XR_XIR_BOOL; XrXirCompileContext budget = interface_context_default();
        CHECK(inference_test_run(&request,&budget,&output,1)==(attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK(output==(attack ? XR_XIR_BOOL : XR_XIR_I64) && interface_live == stage_owner_count);
    }
}
static void type_inference_nominal_cases(void) {
    ImplementationSemanticFixture f; implementation_semantic_fixture(&f);
    XrXirNominalDeclaration declarations[3] = {f.nominals[0],f.nominals[1],f.nominals[1]};
    declarations[2].name = (XrXirLiteral){"OtherBox",8};
    f.nominal_table.declarations = declarations; f.nominal_table.count = 3;
    XrXirInferencePair pair = {f.box_parameter,f.box_meter};
    InferenceTestBatch request = {&f.types,NULL,0,1,0,&pair,1};
    XrXirType output = XR_XIR_BOOL; XrXirCompileContext budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,&output,1)==XR_XIR_OK && output==f.meter);
    f.nodes[3].nominal.declaration = 2; output = XR_XIR_BOOL; budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,&output,1)==XR_XIR_BAD_TYPE);
    CHECK(output==XR_XIR_BOOL && interface_live == stage_owner_count);
}
static void type_inference_symbol_and_work_cases(void) {
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirTypeNode nodes[2] = {0};
    for (uint32_t n = 0; n < 2; ++n) {
        nodes[n].kind = XR_XIR_TYPE_ARRAY; nodes[n].element = p0; nodes[n].parameter_span = 1;
    }
    XrXirTypes types = {nodes,2,NULL,NULL};
    XrXirInferencePair pairs[2] = {{p0,member_case_type(0)},{p0,member_case_type(1)}};
    InferenceTestBatch request = {&types,NULL,0,1,1,pairs,2};
    XrXirCompileContext measured = interface_context_default();
    XrXirType output = XR_XIR_BOOL;
    CHECK(inference_test_run(&request,&measured,&output,1)==XR_XIR_OK && output==member_case_type(0));
    XrCompileResourceStats stats = stage_stats(&measured);
    interface_temporary_clean(&measured);
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
        XrXirCompileContext context = interface_exact_context(stats,boundary); output = XR_XIR_BOOL;
        CHECK(inference_test_run(&request,&context,&output,1)==(boundary ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(output==(boundary ? XR_XIR_BOOL : member_case_type(0)));
        interface_temporary_clean(&context);
    }
    request.caller_parameter_count = 0;
    XrXirCompileContext context = interface_context_default();
    CHECK(inference_test_run(&request,&context,&output,1)==XR_XIR_BAD_TYPE);
    interface_temporary_clean(&context);
}
static void type_inference_maximum_parameter_cases(void) {
    const uint32_t count = XR_XIR_TYPE_PARAMETER_LIMIT-XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType *prefix = xr_calloc(count-1,sizeof(*prefix));
    XrXirType *output = xr_calloc(count,sizeof(*output)); CHECK(prefix && output);
    for (uint32_t p = 0; p < count-1; ++p) prefix[p] = XR_XIR_I64;
    XrXirType last = (XrXirType)(XR_XIR_TYPE_PARAMETER_LIMIT-1);
    XrXirInferencePair pair = {last,last};
    InferenceTestBatch request = {NULL,prefix,count-1,1,count,&pair,1};
    XrXirCompileContext budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,output,count)==XR_XIR_OK);
    CHECK(output[0]==XR_XIR_I64 && output[count-1]==last && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
    request.own_count = 2; budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,output,count)==XR_XIR_BAD_STRUCTURE);
    request.own_count = 1; request.caller_parameter_count = count-1; budget = interface_context_default();
    CHECK(inference_test_run(&request,&budget,output,count)==XR_XIR_BAD_TYPE);
    xr_free(output); xr_free(prefix); CHECK(interface_live == stage_owner_count);
}
static XrXirStatus inference_partial_run(XrXirCompileContext *budget) {
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType p1 = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_ARRAY; node.element = p0; node.parameter_span = 1;
    XrXirTypes initial = {NULL,0,NULL,NULL}, grown = {&node,1,NULL,NULL};
    XrXirInferenceRequest request = {&initial,NULL,0,2,0, NULL};
    XrXirInferenceState *state = NULL;
    XrXirStatus status = xr_xir_compile_inference_begin(budget, &request, &state);
    XrXirInferenceKnown known = {0}; XrXirType output[2] = {XR_XIR_BOOL,XR_XIR_BOOL};
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_observe(state, &initial, (XrXirInferencePair){p0,XR_XIR_I64});
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_expected_known(state, &grown, member_case_type(0), &known);
    if (status == XR_XIR_OK) CHECK(known.known && known.argument_count==2 && known.arguments[0]==XR_XIR_I64);
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_expected_known(state, &grown, p1, &known);
    if (status == XR_XIR_OK) CHECK(!known.known && !known.arguments && !known.argument_count);
    CHECK(output[0]==XR_XIR_BOOL && output[1]==XR_XIR_BOOL);
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_observe(state, &grown, (XrXirInferencePair){p1,XR_XIR_STRING});
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_finalize(state, &grown, output, 2);
    if (status == XR_XIR_OK) {
        CHECK(output[0]==XR_XIR_I64 && output[1]==XR_XIR_STRING);
        CHECK(xr_xir_compile_inference_observe(state, &grown, (XrXirInferencePair){p1,XR_XIR_STRING})==XR_XIR_BAD_STRUCTURE);
    } else CHECK(output[0]==XR_XIR_BOOL && output[1]==XR_XIR_BOOL);
    xr_xir_compile_inference_dispose(state); return status;
}
static void type_inference_partial_cases(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        attempts = 0; fail_at = site ? site-1 : SIZE_MAX;
        XrXirCompileContext budget = interface_context_default();
        CHECK(inference_partial_run(&budget)==(site ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
        if (!site) sites = attempts;
    }
    fail_at = SIZE_MAX; printf("Partial type inference: %zu allocation failure sites\n",sites);
}
static void type_inference_empty_prefix_cases(void) {
    XrXirCompileContext budget = interface_context_default();
    XrXirInferenceRequest empty = {NULL,NULL,0,0,0, NULL};
    XrXirInferenceState *state = NULL;
    CHECK(xr_xir_compile_inference_begin(&budget, &empty, &state)==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_finalize(state, NULL, NULL, 0)==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_finalize(state, NULL, NULL, 0)==XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_inference_dispose(state); CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
    XrXirType prefix = XR_XIR_STRING;
    XrXirInferencePair pair = {(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_STRING};
    InferenceTestBatch fixed = {NULL,&prefix,1,0,0,&pair,1};
    for (uint32_t conflict = 0; conflict < 2; ++conflict) {
        pair.actual = conflict ? XR_XIR_I64 : XR_XIR_STRING;
        XrXirType output = XR_XIR_BOOL; budget = interface_context_default();
        CHECK(inference_test_run(&fixed,&budget,&output,1)==(conflict ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK(output==(conflict ? XR_XIR_BOOL : XR_XIR_STRING));
        CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
    }
    fixed.pair_count = 0; XrXirType output = XR_XIR_BOOL; budget = interface_context_default();
    CHECK(inference_test_run(&fixed,&budget,&output,1)==XR_XIR_OK && output==prefix);
    CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
}
static void type_inference_failed_finalize_cases(void) {
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirInferenceRequest request = {NULL,NULL,0,1,0, NULL};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirCompileContext budget = interface_context_default();
        XrXirInferenceState *state = NULL;
        CHECK(xr_xir_compile_inference_begin(&budget, &request, &state)==XR_XIR_OK);
        CHECK(xr_xir_compile_inference_observe(state, NULL, (XrXirInferencePair){p0,XR_XIR_I64})==XR_XIR_OK);
        if (mode==1) interface_exhaust_work(&budget);
        if (mode==2) fail_at = attempts; /* First allocation in this observation. */
        XrXirStatus expected = mode==0 ? XR_XIR_BAD_TYPE : mode==1 ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
        CHECK(xr_xir_compile_inference_observe(state, NULL, (XrXirInferencePair){p0,XR_XIR_STRING})==expected);
        fail_at = SIZE_MAX;
        XrXirType output = XR_XIR_BOOL;
        CHECK(xr_xir_compile_inference_finalize(state, NULL, &output, 1)==expected && output==XR_XIR_BOOL);
        xr_xir_compile_inference_dispose(state); CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
    }
}
/* Fixed small shape, capped search: no private allocation sizes or post-return
 * scratch deltas are used to infer peak storage. */
static XrXirStatus inference_boundary_probe(const InferenceTestBatch *batch,
    uint32_t dimension, uint64_t cap) {
    XrXirCompileContext context = dimension ? interface_context_default() :
        interface_context_limited(STAGE_ALLOCATED_BYTES,cap,STAGE_WORK);
    if (dimension) context.limits.frame_bytes = cap;
    XrXirType output = XR_XIR_BOOL;
    XrXirStatus status = inference_test_run(batch,&context,&output,1);
    CHECK(status==XR_XIR_OK || status==XR_XIR_BUDGET);
    CHECK(output==(status==XR_XIR_OK ? XR_XIR_STRING : XR_XIR_BOOL));
    interface_temporary_clean(&context); return status;
}
static void type_inference_exact_capacity_cases(void) {
    XrXirTypeNode nodes[4] = {0};
    for (uint32_t n = 0; n < 4; ++n) nodes[n].kind = XR_XIR_TYPE_ARRAY;
    nodes[0].element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; nodes[0].parameter_span = 1;
    nodes[1].element = member_case_type(0); nodes[1].parameter_span = 1;
    nodes[2].element = XR_XIR_STRING; nodes[3].element = member_case_type(2);
    XrXirTypes types = {nodes,4,NULL,NULL};
    XrXirInferencePair pair = {member_case_type(1),member_case_type(3)};
    InferenceTestBatch batch = {&types,NULL,0,1,0,&pair,1};
    for (uint32_t dimension = 0; dimension < 2; ++dimension) {
        uint64_t low = 0, high = 4096;
        CHECK(inference_boundary_probe(&batch,dimension,low)==XR_XIR_BUDGET);
        CHECK(inference_boundary_probe(&batch,dimension,high)==XR_XIR_OK);
        for (uint32_t step = 0; step < 12 && high-low > 1; ++step) {
            uint64_t middle = low+(high-low)/2;
            if (inference_boundary_probe(&batch,dimension,middle)==XR_XIR_OK) high = middle;
            else low = middle;
        }
        CHECK(high==low+1 && high>0);
        CHECK(inference_boundary_probe(&batch,dimension,high)==XR_XIR_OK);
        CHECK(inference_boundary_probe(&batch,dimension,high-1)==XR_XIR_BUDGET);
    }
}
static void type_inference_relocated_ids_case(void) {
    XrXirTypeNode original = {0}; original.kind = XR_XIR_TYPE_ARRAY; original.element = XR_XIR_I64;
    XrXirTypes initial = {&original,1,NULL,NULL};
    XrXirInferenceRequest request = {&initial,NULL,0,1,0, NULL};
    XrXirCompileContext budget = interface_context_default();
    XrXirInferenceState *state = NULL;
    CHECK(xr_xir_compile_inference_begin(&budget, &request, &state)==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_observe(state, &initial, (XrXirInferencePair){
        (XrXirType)XR_XIR_TYPE_PARAMETER_BASE,member_case_type(0)})==XR_XIR_OK);
    XrXirTypeNode relocated[2] = {original,original};
    relocated[1].element = member_case_type(0);
    XrXirTypes current = {relocated,2,NULL,NULL};
    /* Only the new backing is authoritative after relocation. Poison the retired
     * buffer to expose retained node pointers without invalidating current IDs. */
    memset(&original,0,sizeof(original));
    XrXirType output = XR_XIR_BOOL;
    CHECK(xr_xir_compile_inference_finalize(state, &current, &output, 1)==XR_XIR_OK);
    CHECK(output==member_case_type(0));
    xr_xir_compile_inference_dispose(state); CHECK(interface_live == stage_owner_count && stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
}
static void type_inference_callable_mode_admission_case(void) {
    XrXirType p0 = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter formal = {p0,0}, actual = {XR_XIR_I64,0};
    XrXirTypeNode nodes[2] = {0};
    for (uint32_t n = 0; n < 2; ++n) { nodes[n].kind = XR_XIR_TYPE_CALLABLE; nodes[n].parameter_count = 1; }
    nodes[0].parameters = &formal; nodes[0].result = p0; nodes[0].parameter_span = 1;
    nodes[1].parameters = &actual; nodes[1].result = XR_XIR_I64;
    XrXirTypes types = {nodes,2,NULL,NULL}; XrXirCompileContext budget = interface_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types)==XR_XIR_OK);
    XrXirInferencePair pair = {member_case_type(0),member_case_type(1)};
    InferenceTestBatch batch = {&types,NULL,0,1,0,&pair,1};
    XrXirType output = XR_XIR_BOOL; budget = interface_context_default();
    CHECK(inference_test_run(&batch,&budget,&output,1)==XR_XIR_OK && output==XR_XIR_I64);
    /* Current callable descriptors admit READ (0) only. A different mode fails
     * before inference; bypassing admission would violate this internal API. */
    actual.mode = 1; budget = interface_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types)==XR_XIR_BAD_TYPE);
    CHECK(interface_live == stage_owner_count);
}

static void type_inference_cases(void) {
    type_inference_scalar_cases(); type_inference_nested_cases();
    type_inference_callable_cases(); type_inference_nominal_cases();
    type_inference_symbol_and_work_cases(); type_inference_maximum_parameter_cases();
    type_inference_partial_cases();
    type_inference_empty_prefix_cases(); type_inference_failed_finalize_cases();
    type_inference_exact_capacity_cases(); type_inference_relocated_ids_case();
    type_inference_callable_mode_admission_case();
}
#endif // XIR_TYPE_INFERENCE_CASES_H
