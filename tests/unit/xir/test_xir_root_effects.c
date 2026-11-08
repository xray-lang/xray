/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_effects.c - Numeric semantic summary ownership and failure boundaries
 */
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_effect_analysis_owner.h"
#include "xir/xxir_effects.c"
static uint32_t root_find(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==length&&!memcmp(module->functions[f].name,name,length))return f;
    fprintf(stderr,"root oracle missing function %s\n",name);return UINT32_MAX;
}
static XrXirArtifact *root_source(const char *file,XrXirStatus expected) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_ROOT_EFFECT_FIXTURES,file)>0);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(effect_context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ROOT_EFFECT_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,&effect_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure_path={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,&failure_path);
    if(status!=expected)fprintf(stderr,"root source %s status%u %d:%d %s\n",
        file,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected);
    CHECK(expected==XR_XIR_OK?(source.checked!=NULL):(!source.checked&&!source.snapshot));
    XrXirArtifact *checked=source.checked;source.checked=NULL;
    xr_xir_compile_source_failure_free(&failure_path);xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    return checked;
}
static void direct_root_equations(void) {
    XrXirTypeNode callable = {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64,
        .element = XR_XIR_NEVER, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types = {.nodes = &callable, .count = 1};
    XrXirType fn = XR_XIR_CONSTRUCTED_TYPE_BASE;
    uint32_t arguments[] = {0};
    XrXirType parameters[] = {fn};
    XrXirInstruction apply[] = {
        {.op = XR_XIR_CALL_INDIRECT, .type = XR_XIR_I64, .immediate = 0},
        {.op = XR_XIR_RETURN, .args = {1}}
    };
    XrXirInstruction leaf[] = {{.op = XR_XIR_SLOT_INIT}, {.op = XR_XIR_RETURN}};
    XrXirInstruction call[] = {
        {.op = XR_XIR_FUNCTION_REF, .type = fn, .immediate = 1},
        {.op = XR_XIR_CALL, .type = XR_XIR_I64, .args = {0, 1}, .immediate = 0},
        {.op = XR_XIR_RETURN, .args = {1}}
    };
    XrXirInstruction bind[] = {
        {.op = XR_XIR_FUNCTION_REF, .type = fn, .args = {0, 1}, .immediate = 0},
        {.op = XR_XIR_RETURN, .args = {1}}
    };
    XrXirInstruction returned[] = {
        {.op = XR_XIR_FUNCTION_REF, .type = fn, .immediate = 1},
        {.op = XR_XIR_CALL, .type = fn, .args = {0, 1}, .immediate = 3},
        {.op = XR_XIR_CALL_INDIRECT, .type = XR_XIR_I64, .immediate = 1},
        {.op = XR_XIR_RETURN, .args = {2}}
    };
    XrXirFunction functions[] = {
        {.parameters = parameters, .parameter_count = 1, .instructions = apply, .instruction_count = 2},
        {.instructions = leaf, .instruction_count = 2},
        {.instructions = call, .instruction_count = 3, .operands = arguments, .operand_count = 1},
        {.parameters = parameters, .parameter_count = 1, .result = fn,
            .instructions = bind, .instruction_count = 2, .operands = arguments, .operand_count = 1},
        {.instructions = returned, .instruction_count = 4, .operands = arguments, .operand_count = 1}
    };
    XrXirEffectParameter parameter = {.kind = XR_XIR_EFFECT_PARAMETER_VARIABLE};
    XrXirFunctionEffectContract contracts[5] = {{.parameters = &parameter, .parameter_count = 1},
        {0}, {0}, {.parameters = &parameter, .parameter_count = 1}, {0}};
    XrXirModule module = {.functions = functions, .function_count = 5, .types = &types};
    EffectMark mark = effect_mark();
    EffectCheckedRoots *roots = NULL;
    attempts = 0;
    CHECK(effect_checked_roots_build(&effect_context, &module, contracts, NULL, &roots) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(effect_check_bits(roots, roots->function[0].root)[0] == 4);
    CHECK(effect_check_bits(roots, roots->function[1].root)[0] == 1);
    CHECK(effect_check_bits(roots, roots->function[2].root)[0] == 1);
    CHECK(effect_check_bits(roots, roots->function[3].root)[0] == 0);
    CHECK(effect_check_bits(roots, roots->function[3].result)[0] == 4);
    CHECK(effect_check_bits(roots, roots->function[4].root)[0] == 1);
    CHECK(roots->modules == 1 && roots->module_offsets[0] == 0 && roots->module_offsets[1] == 5);
    XrXirRootFormula formula = {0};
    CHECK(effect_checked_root_formula(&effect_context, roots, 0, false, &formula) == XR_XIR_OK);
    CHECK(!formula.constant_mask && formula.term_count == 1 &&
        formula.terms[0].kind == XR_XIR_ROOT_TERM_PARAMETER && !formula.terms[0].index);
    xr_compile_resources_free((void *)formula.terms);
    size_t queries_before = attempts;
    uint32_t mask = UINT32_MAX, argument = 0;
    CHECK(effect_checked_root_mask(roots, roots->function[0].root, &argument, 1, &mask) == XR_XIR_OK && !mask);
    argument = XR_XIR_CALLABLE_ROOT_REQUIRED;
    CHECK(effect_checked_root_mask(roots, roots->function[0].root, &argument, 1, &mask) == XR_XIR_OK && mask == argument);
    CHECK(effect_checked_root_mask(roots, roots->function[0].root, NULL, 0, &mask) == XR_XIR_OK &&
        mask == XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    mask = 91;
    CHECK(effect_checked_root_mask(roots, roots->function[0].root, &argument, 0, &mask) == XR_XIR_BAD_STRUCTURE && mask == 91);
    argument = 1024;
    CHECK(effect_checked_root_mask(roots, roots->function[0].root, &argument, 1, &mask) == XR_XIR_BAD_STRUCTURE && mask == 91);
    CHECK(attempts == queries_before);
    EffectCheckCause cause = {0};
    argument = XR_XIR_CALLABLE_ROOT_REQUIRED;
    CHECK(effect_checked_first_cause(roots, roots->function[0].root, &argument, 1, false, &cause) == XR_XIR_OK &&
        cause.function == 0 && cause.witness.cause == XR_XIR_ROOT_CAUSE_PARAMETER && !cause.witness.slot);
    CHECK(effect_checked_first_cause(roots, roots->function[4].root, NULL, 0, false, &cause) == XR_XIR_OK &&
        cause.function == 4 && cause.witness.cause != XR_XIR_ROOT_CAUSE_NONE);
    CHECK(attempts == queries_before);
    effect_checked_roots_free(roots);
    effect_mark_check(mark);
    for (size_t point = 0; point < sites; ++point) {
        roots = NULL;
        attempts = 0;
        injected = false;
        fail_at = point;
        CHECK(effect_checked_roots_build(&effect_context, &module, contracts, NULL, &roots) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !roots);
        fail_at = SIZE_MAX;
        effect_mark_check(mark);
    }
    const uint64_t finite_limits[] = {128, 4096};
    for (size_t i = 0; i < sizeof(finite_limits) / sizeof(*finite_limits); ++i) {
        XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){
            XR_COMPILE_MEMORY_FINITE, finite_limits[i], true});
        uint64_t held = effect_stats(&finite).held_bytes;
        roots = NULL;
        CHECK(effect_checked_roots_build(&finite, &module, contracts, NULL, &roots) == XR_XIR_MEMORY_LIMIT && !roots);
        effect_owner_free(&finite, held);
        effect_mark_check(mark);
    }
    roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, contracts, NULL, &roots) == XR_XIR_OK);
    memset(functions, 0xCE, sizeof(functions));
    memset(&callable, 0xCE, sizeof(callable));
    memset(apply, 0xCE, sizeof(apply));
    memset(returned, 0xCE, sizeof(returned));
    CHECK(effect_check_bits(roots, roots->function[4].root)[0] == 1);
    CHECK(!effect_check_evaluate(roots, roots->function[4].root));
    effect_checked_roots_free(roots);
    effect_mark_check(mark);
    printf("direct ROOT equations: captures, returns, %zu allocation failures and detached numeric reads PASS\n", sites);
}

static void generic_resource_failures(void) {
    XrXirArtifact *artifact = root_source("root.xr", XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirFunctionEffectContract *contracts = effect_contracts_read(artifact->checked_effects);
    EffectMark mark = effect_mark();
    EffectCheckedRoots *roots = NULL;
    attempts = 0;
    CHECK(effect_checked_roots_build(&effect_context, module, contracts, NULL, &roots) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(roots->functions > roots->templates && roots->terms &&
        effect_terms_scratch_retired(roots->terms));
    effect_checked_roots_free(roots);
    effect_mark_check(mark);
    for (size_t point = 0; point < sites; ++point) {
        roots = NULL;
        attempts = 0;
        injected = false;
        fail_at = point;
        XrXirStatus status = effect_checked_roots_build(&effect_context, module, contracts, NULL, &roots);
        if (status != XR_XIR_OUT_OF_MEMORY || !injected || roots)
            fprintf(stderr, "generic allocation %zu/%zu status%u injected%d roots%p\n", point, sites,
                status, injected, (void *)roots);
        CHECK(status == XR_XIR_OUT_OF_MEMORY && injected && !roots);
        fail_at = SIZE_MAX;
        effect_mark_check(mark);
    }
    roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, module, contracts, NULL, &roots) == XR_XIR_OK);
    uint32_t resolved = root_find(module, "instantiateRequirement");
    effect_artifact_free(artifact);
    XrXirRootFormula formula = {0};
    CHECK(effect_checked_root_formula(&effect_context, roots, resolved, false, &formula) == XR_XIR_OK &&
        !formula.constant_mask && !formula.term_count);
    CHECK(roots->terms->types.interfaces && roots->terms->types.nominals);
    uint32_t mask = UINT32_MAX;
    CHECK(effect_checked_root_mask(roots, roots->function[resolved].root, NULL, 0, &mask) == XR_XIR_OK && !mask);
    effect_checked_roots_free(roots);
    printf("generic numeric binding: %zu allocation failures, retired scratch and producer-dead projection PASS\n", sites);
}

static XrXirStatus parameter_summary_try(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirEffectParameter *parameters) {
    XrXirFunctionEffectContract contracts[3] = {0};
    for (uint32_t f = 0; f < 3; ++f)
        contracts[f] = (XrXirFunctionEffectContract){.parameter_count = 1, .parameters = &parameters[f]};
    EffectParameterGraph graph = {.work = context, .module = *module};
    XrXirStatus status = effect_parameter_graph_prepare(&graph, contracts);
    if (status == XR_XIR_OK) status = effect_parameter_graph_collect(&graph);
    XrXirFunction headers[3];
    memcpy(headers, module->functions, sizeof(headers));
    for (uint32_t f = 0; f < 3; ++f) {
        headers[f].instructions = NULL;
        headers[f].instruction_count = UINT32_MAX;
    }
    graph.module.functions = headers;
    if (status == XR_XIR_OK) status = effect_parameter_graph_finish(&graph, contracts);
    effect_parameter_graph_free(&graph);
    return status;
}
static void parameter_summary_kernel(void) {
    EffectMark baseline = effect_mark();
    XrXirTypeNode callable = {.kind = XR_XIR_TYPE_CALLABLE, .element = XR_XIR_NEVER,
        .result = XR_XIR_UNIT, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types = {.nodes = &callable, .count = 1};
    XrXirType parameter = XR_XIR_CONSTRUCTED_TYPE_BASE;
    uint32_t operand = 0;
    XrXirInstruction rows[3][2] = {
        {{.op = XR_XIR_CALL, .immediate = 1, .type = XR_XIR_CONSTRUCTED_TYPE_BASE, .args = {0, 1}},
            {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_FUNCTION_REF, .immediate = 2, .type = XR_XIR_CONSTRUCTED_TYPE_BASE, .args = {0, 1}},
            {.op = XR_XIR_RETURN, .args = {1}}},
        {{.op = XR_XIR_CALL_INDIRECT, .immediate = 0, .type = XR_XIR_UNIT}, {.op = XR_XIR_RETURN}}
    };
    XrXirFunction functions[3] = {0};
    for (uint32_t f = 0; f < 3; ++f)
        functions[f] = (XrXirFunction){.error_type = XR_XIR_NEVER, .parameters = &parameter, .parameter_count = 1,
            .instructions = rows[f], .instruction_count = 2, .operands = &operand, .operand_count = 1};
    functions[1].result = XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirModule module = {.stage = XR_XIR_CHECKED, .types = &types, .functions = functions, .function_count = 3};
    XrXirEffectParameter parameters[3] = {
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, 0},
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, 0},
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, 0}
    }, empty[3];
    memcpy(empty, parameters, sizeof(empty));
    attempts = 0;
    CHECK(parameter_summary_try(&effect_context, &module, parameters) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(parameters[0].kind == XR_XIR_EFFECT_PARAMETER_FIXED && parameters[0].uses == XR_XIR_EFFECT_USE_FIXED);
    CHECK(parameters[1].kind == XR_XIR_EFFECT_PARAMETER_FIXED && parameters[1].uses ==
        (XR_XIR_EFFECT_USE_CONST_CAPTURE | XR_XIR_EFFECT_USE_RETURN | XR_XIR_EFFECT_USE_INVOKE));
    CHECK(parameters[2].kind == XR_XIR_EFFECT_PARAMETER_VARIABLE && parameters[2].uses == XR_XIR_EFFECT_USE_INVOKE);
    effect_mark_check(baseline);
    for (size_t point = 0; point < sites; ++point) {
        memcpy(parameters, empty, sizeof(parameters));
        attempts = 0; injected = false; fail_at = point;
        CHECK(parameter_summary_try(&effect_context, &module, parameters) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !memcmp(parameters, empty, sizeof(parameters)));
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    CHECK(parameter_summary_try(&finite, &module, parameters) == XR_XIR_MEMORY_LIMIT);
    CHECK(!memcmp(parameters, empty, sizeof(parameters)));
    effect_owner_free(&finite, held);
    effect_mark_check(baseline);
    printf("parameter summaries: capture/forward masks, body-free closure, %zu allocation failures and finite owner PASS\n", sites);
}
static void parameter_summary_owner(void) {
    EffectMark baseline = effect_mark();
    XrXirEffectParameter values[3] = {
        {XR_XIR_EFFECT_PARAMETER_FIXED, XR_XIR_EFFECT_USE_STORE},
        {XR_XIR_EFFECT_PARAMETER_VARIABLE, XR_XIR_EFFECT_USE_INVOKE},
        {XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC, XR_XIR_EFFECT_USE_FORWARD}
    };
    XrXirFunctionEffectContract contracts[3] = {
        {.parameter_count = 2, .parameters = values}, {0},
        {.parameter_count = 1, .parameters = values + 2}
    };
    EffectParameterSummary summary = {0};
    attempts = 0;
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(summary.count == 3 && summary.total == 3 && summary.rows[1].count == 0);
    CHECK(effect_parameter_summary_shape(&effect_context, &summary, 3) == XR_XIR_OK);
    EffectParameterSummary saved = summary;
    attempts = 0;
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_BAD_STRUCTURE);
    CHECK(!attempts && !memcmp(&summary, &saved, sizeof(summary)));
    uint32_t kind = UINT32_MAX;
    CHECK(effect_parameter_summary_get(&summary, 0, 0, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_FIXED);
    CHECK(effect_parameter_summary_get(&summary, 0, 1, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_VARIABLE);
    CHECK(effect_parameter_summary_get(&summary, 2, 0, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC);
    kind = UINT32_MAX;
    CHECK(effect_parameter_summary_get(&summary, 1, 0, &kind) == XR_XIR_BAD_STRUCTURE && kind == UINT32_MAX);
    CHECK(effect_parameter_summary_get(&summary, 3, 0, &kind) == XR_XIR_BAD_STRUCTURE && kind == UINT32_MAX);
    CHECK(effect_parameter_summary_get(&summary, 0, 2, &kind) == XR_XIR_BAD_STRUCTURE && kind == UINT32_MAX);
    effect_parameter_summary_clear(&summary);
    effect_mark_check(baseline);
    for (size_t point = 0; point < sites; ++point) {
        attempts = 0; injected = false; fail_at = point;
        CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !summary.resources && !summary.rows && !summary.values && !summary.count && !summary.total);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirEffectParameter finite_values[32] = {0};
    XrXirFunctionEffectContract large = {.parameter_count = 32, .parameters = finite_values};
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    CHECK(effect_parameter_summary_copy(&finite, &large, 1, &summary) == XR_XIR_MEMORY_LIMIT);
    CHECK(!summary.resources && !summary.rows && !summary.values && !summary.count && !summary.total);
    effect_owner_free(&finite, held);
    values[0].kind = UINT32_MAX;
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_BAD_STRUCTURE);
    CHECK(!summary.rows && !summary.values);
    values[0].kind = XR_XIR_EFFECT_PARAMETER_FIXED;
    effect_mark_check(baseline);

    XrXirStatus status = XR_XIR_OK;
    XrXirFunctionEffectContract *owned = xir_compile_calloc(&effect_context, 3, sizeof(*owned), &status);
    XrXirEffectParameter *inputs = xir_compile_alloc(&effect_context, sizeof(values), &status);
    CHECK(status == XR_XIR_OK && owned && inputs);
    memcpy(owned, contracts, sizeof(contracts));
    memcpy(inputs, values, sizeof(values));
    owned[0].parameters = inputs;
    owned[2].parameters = inputs + 2;
    CHECK(effect_parameter_summary_copy(&effect_context, owned, 3, &summary) == XR_XIR_OK);
    CHECK(effect_parameter_summary_matches(&effect_context, &summary, owned, 3) == XR_XIR_OK);
    inputs[0].kind = XR_XIR_EFFECT_PARAMETER_VARIABLE;
    CHECK(effect_parameter_summary_matches(&effect_context, &summary, owned, 3) == XR_XIR_BAD_STRUCTURE);
    inputs[0] = values[0];
    inputs[0].uses ^= XR_XIR_EFFECT_USE_INVOKE;
    CHECK(effect_parameter_summary_matches(&effect_context, &summary, owned, 3) == XR_XIR_BAD_STRUCTURE);
    xr_compile_resources_free(inputs);
    xr_compile_resources_free(owned);
    memset(values, 0xCE, sizeof(values));
    memset(contracts, 0xCE, sizeof(contracts));
    attempts = 0;
    CHECK(effect_parameter_summary_get(&summary, 0, 0, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_FIXED);
    CHECK(effect_parameter_summary_get(&summary, 0, 1, &kind) == XR_XIR_OK && kind == XR_XIR_EFFECT_PARAMETER_VARIABLE);
    CHECK(!attempts && summary.values[0].uses == XR_XIR_EFFECT_USE_STORE);
    CHECK(effect_parameter_summary_shape(&effect_context, &summary, 3) == XR_XIR_OK);
    effect_parameter_summary_clear(&summary);
    effect_mark_check(baseline);

    memset(contracts, 0, sizeof(contracts));
    CHECK(effect_parameter_summary_copy(&effect_context, contracts, 3, &summary) == XR_XIR_OK);
    CHECK(summary.count == 3 && !summary.total && !summary.values);
    CHECK(effect_parameter_summary_shape(&effect_context, &summary, 3) == XR_XIR_OK);
    effect_parameter_summary_clear(&summary);
    effect_mark_check(baseline);
    printf("owned parameter rows: %zu allocation failures, detached scalar reads and finite owner PASS\n", sites);
}
static void control_summary_streamed(void) {
    EffectMark baseline = effect_mark();
    XrXirInstruction rows[3][2] = {
        {{.op = XR_XIR_CALL, .immediate = 1}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_CALL, .immediate = 2}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_SUSPEND}, {.op = XR_XIR_RETURN}}
    };
    XrXirFunction functions[3] = {0};
    for (uint32_t f = 0; f < 3; ++f)
        functions[f] = (XrXirFunction){.name = "f", .name_length = 1, .error_type = XR_XIR_NEVER,
            .instructions = rows[f], .instruction_count = 2};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .functions = functions, .function_count = 3};
    XirControlBuilder *builder = NULL;
    EffectControlSummary *summary = NULL, *retained = NULL;
    CHECK(xir_effect_control_builder_new(&effect_context, &module, NULL, &builder) == XR_XIR_OK);
    const uint32_t order[] = {1, 0, 2};
    for (uint32_t at = 0; at < 3; ++at) {
        uint32_t f = order[at];
        CHECK(xir_effect_control_begin(builder, &module, f) == XR_XIR_OK);
        CHECK(xir_effect_control_begin(builder, &module, f) == XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effect_control_instruction(builder, &module, f, 1) == XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effect_control_end(builder, &module, f) == XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effect_control_finish(builder, &module, &summary) == XR_XIR_BAD_STRUCTURE && !summary);
        for (uint32_t i = 0; i < 2; ++i)
            CHECK(xir_effect_control_instruction(builder, &module, f, i) == XR_XIR_OK);
        CHECK(xir_effect_control_end(builder, &module, f) == XR_XIR_OK);
        CHECK(xir_effect_control_begin(builder, &module, f) == XR_XIR_BAD_STRUCTURE);
    }
    memset(rows, 0xCE, sizeof(rows));
    for (uint32_t f = 0; f < 3; ++f) {
        functions[f].instructions = NULL;
        functions[f].instruction_count = UINT32_MAX;
    }
    CHECK(xir_effect_control_finish(builder, &module, &summary) == XR_XIR_OK);
    CHECK(xir_effect_control_finish(builder, &module, &retained) == XR_XIR_BAD_STRUCTURE && !retained);
    xir_effect_control_builder_free(builder);
    CHECK(summary->facts[0].suspend == XR_XIR_EFFECT_MAY && summary->causes[0].callee == 1);
    CHECK(summary->causes[0].distance == 2 && summary->causes[1].callee == 2);
    CHECK(xir_effect_control_retain(&effect_context, summary, &retained) == XR_XIR_OK);
    CHECK(xir_effect_control_retain(&effect_context, summary, &retained) == XR_XIR_BAD_STRUCTURE);
    xir_effect_control_free(summary);
    CHECK(retained->causes[2].cause == XR_XIR_EFFECT_CAUSE_SUSPEND);
    xir_effect_control_free(retained);
    effect_mark_check(baseline);
    puts("streamed control summaries close without bodies and retain independent ownership PASS");
}
static void control_summary_kernel(void) {
    parameter_summary_kernel();
    parameter_summary_owner();
    control_summary_streamed();
    const uint32_t count = 8193;
    XrXirFunction *functions = xr_calloc(count, sizeof(*functions));
    XrXirInstruction (*rows)[2] = xr_calloc(count, sizeof(*rows));
    XrXirBlock *blocks = xr_calloc(count, sizeof(*blocks));
    CHECK(functions && rows && blocks);
    for (uint32_t f = 0; f < count; ++f) {
        rows[f][0] = (XrXirInstruction){.op = XR_XIR_CALL, .immediate = f + 1};
        rows[f][1] = (XrXirInstruction){.op = XR_XIR_RETURN};
        blocks[f] = (XrXirBlock){.count = 2};
        functions[f] = (XrXirFunction){.name = "f", .name_length = 1, .error_type = XR_XIR_NEVER,
            .blocks = &blocks[f], .block_count = 1, .instructions = rows[f], .instruction_count = 2};
    }
    rows[count - 1][0] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    XrXirModule module = {.stage = XR_XIR_BUILT, .functions = functions, .function_count = count};
    EffectControlSummary *summary = NULL;
    EffectMark baseline = effect_mark();
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    for (uint32_t f = 0; f < count; ++f) {
        CHECK(summary->facts[f].suspend == XR_XIR_EFFECT_MAY);
        CHECK(summary->facts[f].throws == XR_XIR_EFFECT_NONE);
        CHECK(summary->causes[f].distance == count - f - 1);
    }
    EffectControlSummary *occupied = summary;
    size_t before = attempts;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == summary && attempts == before);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);

    module.function_count = 4;
    rows[2][0].immediate = 0;
    rows[3][0].immediate = 3;
    summary = NULL;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    for (uint32_t f = 0; f < 4; ++f) CHECK(summary->facts[f].suspend == XR_XIR_EFFECT_NONE);
    xir_effect_control_free(summary);

    XrXirInstruction terminal[3] = {
        {.op = XR_XIR_CALL}, {.op = XR_XIR_SUSPEND}, {.op = XR_XIR_RETURN}
    };
    functions[2].instructions = terminal;
    functions[2].instruction_count = blocks[2].count = 3;
    summary = NULL;
    attempts = 0;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    size_t sites = attempts;
    for (uint32_t f = 0; f < 4; ++f)
        CHECK(summary->facts[f].suspend == (f < 3 ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
    CHECK(summary->causes[0].callee == 1 && summary->causes[0].distance == 2);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);
    for (size_t point = 0; point < sites; ++point) {
        summary = NULL;
        attempts = 0;
        injected = false;
        fail_at = point;
        CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !summary);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    summary = NULL;
    CHECK(effect_control_summary_build(&finite, &module, NULL, &summary) == XR_XIR_MEMORY_LIMIT && !summary);
    effect_owner_free(&finite, held);
    effect_mark_check(baseline);

    summary = NULL;
    CHECK(effect_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    memset(functions, 0xCE, count * sizeof(*functions));
    memset(rows, 0xCE, count * sizeof(*rows));
    memset(terminal, 0xCE, sizeof(terminal));
    xr_free(functions);
    xr_free(rows);
    xr_free(blocks);
    CHECK(summary->facts[0].suspend == XR_XIR_EFFECT_MAY && summary->causes[0].callee == 1);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);
    printf("control summaries: iterative long chain, seeded/unseeded SCC, %zu allocation failures and detached owner PASS\n", sites);
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--control-summary")));
    effect_case_begin();
    direct_root_equations();
    generic_resource_failures();
    control_summary_kernel();
    effect_case_end();
    return 0;
}
