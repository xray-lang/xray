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
#include "xir/xxir_source_calls.h"
#include "xir/xxir_source_corpus.h"
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
    if (expected==XR_XIR_OK) {
        CHECK(xr_xir_compile_source_snapshot_calls(source.snapshot)==NULL);
        XrXirSourceCorpusRequest corpus={&request,NULL,0,NULL,0};
        XrXirSourceResult observed={0};
        CHECK(xr_xir_compile_source_check_corpus(&corpus,&observed,NULL,NULL)==XR_XIR_OK);
        const XrXirSourceCalls *calls=xr_xir_compile_source_snapshot_calls(observed.snapshot);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(observed.snapshot);
        CHECK(calls && view && calls->declaration_count==view->declaration_count);
        xr_xir_compile_source_result_free(&observed);
    }
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
    for (uint32_t f = 0; f < 5; ++f) functions[f].error_type = XR_XIR_NEVER;
    XrXirEffectParameter parameter = {.kind = XR_XIR_EFFECT_PARAMETER_VARIABLE};
    XrXirFunctionEffectContract contracts[5] = {{.parameters = &parameter, .parameter_count = 1},
        {0}, {0}, {.parameters = &parameter, .parameter_count = 1}, {0}};
    XrXirModule module = {.functions = functions, .function_count = 5, .types = &types};
    EffectMark mark = effect_mark();
    EffectCheckedRoots *roots = NULL;
    attempts = 0;
    CHECK(effect_checked_roots_build(&effect_context, &module, contracts, NULL, NULL, &roots, true) == XR_XIR_OK);
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
        CHECK(effect_checked_roots_build(&effect_context, &module, contracts, NULL, NULL, &roots, true) == XR_XIR_OUT_OF_MEMORY);
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
        CHECK(effect_checked_roots_build(&finite, &module, contracts, NULL, NULL, &roots, true) == XR_XIR_MEMORY_LIMIT && !roots);
        effect_owner_free(&finite, held);
        effect_mark_check(mark);
    }
    roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, contracts, NULL, NULL, &roots, true) == XR_XIR_OK);
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
    CHECK(effect_checked_roots_build(&effect_context, module, contracts, NULL, NULL, &roots, true) == XR_XIR_OK);
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
        XrXirStatus status = effect_checked_roots_build(&effect_context, module, contracts, NULL, NULL, &roots, true);
        if (status != XR_XIR_OUT_OF_MEMORY || !injected || roots)
            fprintf(stderr, "generic allocation %zu/%zu status%u injected%d roots%p\n", point, sites,
                status, injected, (void *)roots);
        CHECK(status == XR_XIR_OUT_OF_MEMORY && injected && !roots);
        fail_at = SIZE_MAX;
        effect_mark_check(mark);
    }
    roots = NULL;
    EffectCheckedRoots *interfaces = NULL;
    XrXirCompileContext producer = effect_context;
    CHECK(effect_checked_roots_collect(&producer, module, contracts, NULL, NULL, &interfaces, false) == XR_XIR_OK);
    CHECK(interfaces->terms && effect_terms_scratch_retired(interfaces->terms));
    memset(&producer, 0xCE, sizeof(producer));
    CHECK(effect_checked_roots_link_take(&effect_context, module, NULL, &interfaces, &roots, true, NULL) == XR_XIR_OK && !interfaces);
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

static XrXirStatus root_control_summary_build(const XrXirCompileContext *context,
    const XrXirModule *module, XrCompileRequestControl *control, EffectControlSummary **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectCheckedRoots *roots = NULL;
    XrXirStatus status = effect_checked_roots_build(context, module, NULL, control, NULL, &roots, true);
    if (status == XR_XIR_OK) status = effect_checked_control_project(context, roots, output);
    effect_checked_roots_free(roots);
    return status;
}
static void control_summary_detached(void) {
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
    EffectCheckedRoots *interfaces = NULL, *roots = NULL;
    EffectControlSummary *summary = NULL;
    CHECK(effect_checked_roots_collect(&effect_context, &module, NULL, NULL, NULL, &interfaces, false) == XR_XIR_OK);
    memset(rows, 0xCE, sizeof(rows));
    for (uint32_t f = 0; f < 3; ++f) {
        functions[f].instructions = NULL;
        functions[f].instruction_count = UINT32_MAX;
    }
    CHECK(effect_checked_roots_link_take(&effect_context, &module, NULL, &interfaces, &roots, true, NULL) == XR_XIR_OK);
    CHECK(effect_checked_control_project(&effect_context, roots, &summary) == XR_XIR_OK);
    effect_checked_roots_free(roots);
    CHECK(summary->facts[0].suspend == XR_XIR_EFFECT_MAY && summary->causes[0].callee == 1);
    CHECK(summary->causes[0].distance == 2 && summary->causes[1].callee == 2);
    CHECK(summary->causes[2].cause == XR_XIR_EFFECT_CAUSE_SUSPEND);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);
    puts("numeric control projections close without bodies and own detached values PASS");
}
static void module_interface_lifetime(void) {
    uint32_t imported = 1;
    XrXirSourceModule modules[2] = {
        {.name = "entry", .name_length = 5, .dependencies = &imported, .dependency_count = 1, .initializer = 2},
        {.name = "dependency", .name_length = 10, .initializer = 3}
    };
    XrXirFunctionIdentity identities[4] = {{.module = 0}, {.module = 1}, {.module = 0}, {.module = 1}};
    XrXirSlot slot = {.type = XR_XIR_I64, .module = 1, .mutable = true};
    XrXirDeclarations declarations = {.modules = modules, .module_count = 2,
        .functions = identities, .slots = &slot, .slot_count = 1};
    XrXirType scalar_argument = XR_XIR_I64;
    XrXirGeneric generics[4] = {{.arguments = &scalar_argument, .argument_count = 1}, {.parameter_count = 1}};
    const XrXirInstruction original[4][2] = {
        {{.op = XR_XIR_CALL, .immediate = 1, .type_arguments = {0, 1}}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_SLOT_INIT}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_RETURN}}, {{.op = XR_XIR_RETURN}}
    };
    XrXirInstruction rows[4][2];
    XrXirFunction functions[4] = {0};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .linkage_kind = XR_XIR_PROGRAM,
        .functions = functions, .function_count = 4, .declarations = &declarations, .generics = generics};
    EffectMark baseline = effect_mark();
    size_t sites = 0;
    for (size_t point = 0; point <= sites; ++point) {
        memcpy(rows, original, sizeof(rows));
        for (uint32_t f = 0; f < 4; ++f)
            functions[f] = (XrXirFunction){.name = "f", .name_length = 1, .instructions = rows[f],
                .instruction_count = f < 2 ? 2 : 1, .error_type = XR_XIR_NEVER};
        EffectCheckedRoots *interfaces = NULL, *roots = NULL;
        XrXirCompileContext producer = effect_context;
        CHECK(effect_checked_roots_collect(&producer, &module, NULL, NULL, NULL, &interfaces, false) == XR_XIR_OK);
        CHECK(interfaces->terms && effect_terms_scratch_retired(interfaces->terms));
        memset(&producer, 0xCE, sizeof(producer));
        CHECK(interfaces->modules == 2 && interfaces->module_offsets[1] == 2 &&
            !effect_check_bits(interfaces, interfaces->function[0].root)[0]);
        EffectCheckedRoots *occupied = interfaces;
        size_t prior = attempts;
        CHECK(effect_checked_roots_link_take(&effect_context, &module, NULL, &interfaces, &occupied, true, NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(interfaces == occupied && attempts == prior);
        memset(rows, 0xCE, sizeof(rows));
        for (uint32_t f = 0; f < 4; ++f) {
            functions[f].instructions = NULL;
            functions[f].instruction_count = UINT32_MAX;
        }
        attempts = 0;
        if (point) { injected = false; fail_at = point - 1; }
        XrXirStatus status = effect_checked_roots_link_take(&effect_context, &module, NULL, &interfaces, &roots, true, NULL);
        CHECK(!interfaces);
        if (point) {
            CHECK(status == XR_XIR_OUT_OF_MEMORY && injected && !roots);
            fail_at = SIZE_MAX;
        } else {
            CHECK(status == XR_XIR_OK && roots);
            sites = attempts;
            memset(functions, 0xCE, sizeof(functions));
            uint32_t mask = 0;
            CHECK(effect_checked_root_mask(roots, roots->function[0].root, NULL, 0, &mask) == XR_XIR_OK &&
                mask == XR_XIR_CALLABLE_ROOT_REQUIRED);
            CHECK(roots->node[roots->function[0].root].access_denied &&
                roots->node[roots->function[1].root].access_denied);
            CHECK(!roots->node[roots->function[2].root].access_denied &&
                !roots->node[roots->function[3].root].access_denied);
            effect_checked_roots_free(roots);
        }
        effect_mark_check(baseline);
    }
    printf("module interfaces: body-free cross-module closure, %zu consumed-owner failures and detached reads PASS\n", sites);
}

static XrXirStatus execution_value_oracle(void *context, const XirExecutionSite *site) {
    uint32_t *count = context;
    CHECK(site->protocol == 1 && site->declaration == 1);
    CHECK(site->environment == XIR_EXECUTION_CURRENT_ENVIRONMENT);
    ++*count;
    return XR_XIR_OK;
}

typedef struct ExecutionFormalProbe {
    EffectCheckedRoots *roots;
    uint32_t count;
} ExecutionFormalProbe;
static XrXirStatus execution_formal_oracle(void *context, const XirExecutionSite *site) {
    ExecutionFormalProbe *probe = context;
    CHECK(site->kind == XIR_EXECUTION_FORMAL && site->declaration == 0 && site->receiver_value == 0);
    XirExecutionArgument argument = {0};
    CHECK(xir_execution_argument(probe->roots, site, 0, &argument) == XR_XIR_OK);
    CHECK(argument.function == 0 && argument.value == 0 && !argument.escaping && !argument.cell);
    CHECK(argument.type == XR_XIR_CONSTRUCTED_TYPE_BASE && argument.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT);
    ++probe->count;
    return XR_XIR_OK;
}

static void suspension_parameter_equations(void) {
    EffectMark baseline = effect_mark();
    XrXirType fn = XR_XIR_CONSTRUCTED_TYPE_BASE, cell = XR_XIR_CONSTRUCTED_TYPE_BASE + 1;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_UNIT, .element = XR_XIR_NEVER,
            .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_CONSTRUCTED_TYPE_BASE}
    };
    XrXirTypes types = {.nodes = nodes, .count = 2};
    XrXirType parameter[] = {fn};
    uint32_t argument[] = {0};
    uint32_t escaping = 0;
    XrXirInstruction rows[17][4] = {0};
    XrXirFunction functions[17] = {0};
    for (uint32_t f = 0; f < 17; ++f) {
        rows[f][0] = (XrXirInstruction){.op = XR_XIR_RETURN};
        functions[f] = (XrXirFunction){.name = "f", .name_length = 1, .error_type = XR_XIR_NEVER,
            .instructions = rows[f], .instruction_count = 1, .operands = argument, .operand_count = 1};
    }
    const uint32_t parameters[] = {0, 6, 10, 13, 14};
    for (uint32_t p = 0; p < sizeof(parameters) / sizeof(*parameters); ++p) {
        functions[parameters[p]].parameters = parameter;
        functions[parameters[p]].parameter_count = 1;
    }
    rows[2][0] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    rows[2][1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    functions[2].instruction_count = 2;
    const uint32_t callers[] = {3, 4, 7, 11, 12, 15, 16};
    const uint32_t targets[] = {0, 0, 6, 10, 10, 13, 13};
    for (uint32_t c = 0; c < sizeof(callers) / sizeof(*callers); ++c) {
        uint32_t f = callers[c];
        rows[f][0] = (XrXirInstruction){.op = XR_XIR_FUNCTION_REF, .type = fn,
            .immediate = f == 4 || f == 12 || f == 16 ? 2 : 1};
        rows[f][1] = (XrXirInstruction){.op = XR_XIR_CALL, .args = {0, 1}, .immediate = targets[c]};
        rows[f][2] = (XrXirInstruction){.op = XR_XIR_RETURN};
        functions[f].instruction_count = 3;
    }
    rows[5][0] = (XrXirInstruction){.op = XR_XIR_FUNCTION_REF, .type = fn, .immediate = 2};
    rows[5][1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    functions[5].instruction_count = 2;
    functions[6].escaping_parameter_count = 1;
    functions[6].escaping_parameters = &escaping;
    rows[6][0] = (XrXirInstruction){.op = XR_XIR_CALL_INDIRECT};
    rows[6][1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    functions[6].instruction_count = 2;
    functions[8].result = fn;
    rows[8][0] = (XrXirInstruction){.op = XR_XIR_FUNCTION_REF, .type = fn, .immediate = 1};
    rows[8][1] = (XrXirInstruction){.op = XR_XIR_FUNCTION_WEAKEN, .type = fn};
    rows[8][2] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {1}};
    functions[8].instruction_count = 3;
    rows[9][0] = (XrXirInstruction){.op = XR_XIR_CALL_DEFAULT, .type = fn, .targets = {0, 0}};
    rows[9][1] = (XrXirInstruction){.op = XR_XIR_CALL, .args = {0, 1}, .immediate = 0};
    rows[9][2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    functions[9].instruction_count = 3;
    rows[10][0] = (XrXirInstruction){.op = XR_XIR_CELL_NEW, .type = cell};
    rows[10][1] = (XrXirInstruction){.op = XR_XIR_CELL_READ, .type = fn, .args = {1}};
    rows[10][2] = (XrXirInstruction){.op = XR_XIR_CALL_INDIRECT, .immediate = 2};
    rows[10][3] = (XrXirInstruction){.op = XR_XIR_RETURN};
    functions[10].instruction_count = 4;
    for (uint32_t f = 13; f < 15; ++f) {
        rows[f][0] = (XrXirInstruction){.op = XR_XIR_CALL, .args = {0, 1}, .immediate = f == 13 ? 14 : 13};
        rows[f][1] = (XrXirInstruction){.op = XR_XIR_RETURN};
        functions[f].instruction_count = 2;
    }
    XrXirDefaultBinding binding = {.owner_kind = XR_XIR_DEFAULT_PARAMETER, .owner = 0, .ordinal = 0, .function = 8};
    XrXirDefaultTable defaults = {.records = &binding, .count = 1};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .types = &types, .functions = functions,
        .function_count = 17, .defaults = &defaults};
    EffectCheckedRoots *roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, true) == XR_XIR_OK);
    CHECK(effect_check_suspend_bits(roots, roots->function[0].root)[0] == 4);
    CHECK(effect_checked_suspend_mask(roots, roots->function[0].root) == 1);
    const uint8_t expected[] = {1, 0, 2, 0, 2, 0, 1, 1, 0, 0, 1, 0, 2, 1, 1, 0, 2};
    for (uint32_t f = 0; f < 17; ++f)
        CHECK(effect_checked_suspend_mask(roots, roots->function[f].root) == expected[f]);
    CHECK(effect_check_suspend_bits(roots, roots->function[13].root)[0] == 4);
    CHECK(effect_check_suspend_bits(roots, roots->function[14].root)[0] == 4);
    uint32_t captures = 0;
    bool complete = false;
    CHECK(xir_execution_visit_value_captures(roots, 3, 0,
        execution_value_oracle, &captures, &complete) == XR_XIR_OK && complete && captures == 1);
    captures = 0; complete = true;
    XirExecutionValue formal = {0, 0, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    CHECK(xir_execution_visit_value_captures_in(roots, &formal,
        execution_value_oracle, &captures, &complete, NULL) == XR_XIR_OK && !complete && !captures);
    ExecutionFormalProbe probe = {roots, 0};
    uint32_t before_calls = roots->calls, remainder = UINT32_MAX;
    CHECK(xir_execution_visit_value_environment_in(roots, &formal, execution_formal_oracle,
        &probe, &complete, &remainder) == XR_XIR_OK && complete && !remainder && probe.count == 1);
    CHECK(roots->calls == before_calls);
    for (uint32_t p = 0; p < roots->pending_count; ++p) {
        if (roots->pending[p].function != 6) continue;
        XirExecutionSite site = {0};
        CHECK(xir_execution_site(roots, XIR_EXECUTION_PENDING, p, &site) == XR_XIR_OK);
        uint32_t remainder = 0;
        CHECK(xir_execution_resolve_site(roots, &site, XIR_EXECUTION_CURRENT_ENVIRONMENT,
            execution_value_oracle, &captures, &complete, &remainder) == XR_XIR_OK);
        CHECK(!complete && !captures && (remainder & XIR_EXECUTION_REMAINDER_PARAMETER));
    }
    memset(rows, 0xCE, sizeof(rows));
    memset(functions, 0xCE, sizeof(functions));
    CHECK(effect_checked_suspend_mask(roots, roots->function[4].root) == 2);
    CHECK(!effect_check_evaluate(roots, roots->function[4].root));
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
    puts("suspension formulas preserve callback substitution, aliases, latent values, defaults and recursive bounds PASS");
}

static void suspension_reason_order(void) {
    EffectMark baseline = effect_mark();
    XrXirInstruction rows[5][3] = {
        {{.op = XR_XIR_CALL, .immediate = 1}, {.op = XR_XIR_CALL, .immediate = 2}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_CALL, .immediate = 0}, {.op = XR_XIR_CALL, .immediate = 3}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_CALL, .immediate = 4}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_SUSPEND}, {.op = XR_XIR_RETURN}},
        {{.op = XR_XIR_SUSPEND}, {.op = XR_XIR_RETURN}}
    };
    XrXirFunction functions[5] = {0};
    for (uint32_t f = 0; f < 5; ++f)
        functions[f] = (XrXirFunction){.name = "f", .name_length = 1, .error_type = XR_XIR_NEVER,
            .instructions = rows[f], .instruction_count = f < 2 ? 3 : 2};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .functions = functions, .function_count = 5};
    EffectControlSummary *summary = NULL;
    CHECK(root_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    CHECK(summary->causes[0].distance == 2 && summary->causes[0].callee == 1 && !summary->causes[0].instruction);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);
    puts("equal-length suspension causes retain source order across recursive component scheduling PASS");
}

typedef struct ExecutionCleanupOracle {
    uint32_t normal, panic, captures;
} ExecutionCleanupOracle;
static XrXirStatus execution_cleanup_oracle(void *context, const XirExecutionSite *site) {
    ExecutionCleanupOracle *oracle = context;
    CHECK(site->kind == XIR_EXECUTION_CLEANUP_USE && site->protocol == 4 && site->function == 0);
    CHECK(site->registration != UINT32_MAX);
    if (site->port == XIR_EXECUTION_PANIC) {
        CHECK(site->instruction == UINT32_MAX && (site->block == 1 || site->block == 2));
        CHECK(site->declaration == (site->block == 1 ? 1u : 2u - site->order));
        ++oracle->panic;
    } else {
        CHECK(site->instruction == 2 && site->block == 2 && site->order == oracle->normal);
        CHECK(site->declaration == 2 - site->order);
        ++oracle->normal;
    }
    return XR_XIR_OK;
}
static XrXirStatus execution_capture_oracle(void *context, const XirExecutionSite *site) {
    ExecutionCleanupOracle *oracle = context;
    CHECK(site->kind == XIR_EXECUTION_CALL && site->protocol == 4);
    CHECK(site->instruction < 2 && site->declaration == site->instruction + 1);
    ++oracle->captures;
    return XR_XIR_OK;
}
static void execution_cleanup_order(void) {
    EffectMark baseline = effect_mark();
    XrXirInstruction parent[] = {
        {.op = XR_XIR_CLEANUP_REGISTER, .immediate = 1, .targets = {1}},
        {.op = XR_XIR_CLEANUP_REGISTER, .immediate = 2, .targets = {2}},
        {.op = XR_XIR_CLEANUP_LEAVE, .targets = {3}},
        {.op = XR_XIR_RETURN}
    };
    XrXirInstruction leaf = {.op = XR_XIR_RETURN};
    XrXirBlock blocks[] = {{.first = 0, .count = 1}, {.first = 1, .count = 1, .frontier = 1},
        {.first = 2, .count = 1, .frontier = 2}, {.first = 3, .count = 1}};
    XrXirBlock leaf_block = {.count = 1};
    XrXirFunction functions[] = {
        {.instructions = parent, .instruction_count = 4, .blocks = blocks, .block_count = 4, .error_type = XR_XIR_NEVER},
        {.instructions = &leaf, .instruction_count = 1, .blocks = &leaf_block, .block_count = 1, .error_type = XR_XIR_NEVER},
        {.instructions = &leaf, .instruction_count = 1, .blocks = &leaf_block, .block_count = 1, .error_type = XR_XIR_NEVER}
    };
    XrXirModule module = {.stage = XR_XIR_CHECKED, .functions = functions, .function_count = 3};
    EffectCheckedRoots *roots = NULL;
    attempts = 0;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, true) == XR_XIR_OK);
    size_t sites = attempts;
    ExecutionCleanupOracle oracle = {0};
    CHECK(xir_execution_visit_calls(roots, execution_cleanup_oracle, &oracle) == XR_XIR_OK);
    CHECK(xir_execution_visit_captures(roots, execution_capture_oracle, &oracle) == XR_XIR_OK);
    CHECK(oracle.normal == 2 && oracle.panic == 3 && oracle.captures == 2);
    XirExecutionSite site = {0};
    CHECK(xir_execution_site(roots, XIR_EXECUTION_CLEANUP_USE, 0, &site) == XR_XIR_OK);
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
    for (size_t point = 0; point < sites; ++point) {
        roots = NULL; attempts = 0; injected = false; fail_at = point;
        CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, true) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !roots);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, false) == XR_XIR_OK);
    CHECK(roots->storage);
    for (uint32_t n = 0; n < roots->nodes; ++n) CHECK(roots->node[n].component == UINT32_MAX);
    oracle = (ExecutionCleanupOracle){0};
    CHECK(xir_execution_visit_calls(roots, execution_cleanup_oracle, &oracle) == XR_XIR_OK);
    CHECK(oracle.normal == 2 && oracle.panic == 3);
    XirStorageFlowView storage = {0};
    CHECK(xir_storage_flow_seal(roots->storage, &storage) == XR_XIR_OK && storage.function_count == 3);
    EffectCheckBuild close = {.memory = &effect_context, .roots = roots};
    CHECK(effect_check_cell_roles(&close) == XR_XIR_OK && effect_check_close(&close) == XR_XIR_OK);
    for (uint32_t n = 0; n < roots->nodes; ++n) CHECK(roots->node[n].component != UINT32_MAX);
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
    printf("ordered normal/panic cleanup uses retain registration identities and %zu failure boundaries PASS\n", sites);
}

static void execution_origin_namespaces(void) {
    EffectMark baseline = effect_mark();
    XrXirType fn = XR_XIR_CONSTRUCTED_TYPE_BASE, array = XR_XIR_CONSTRUCTED_TYPE_BASE + 1;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_UNIT, .element = XR_XIR_NEVER,
            .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_CONSTRUCTED_TYPE_BASE}
    };
    XrXirTypes types = {.nodes = nodes, .count = 2};
    uint32_t operands[] = {0};
    XrXirInstruction instructions[] = {
        {.op = XR_XIR_FUNCTION_REF, .type = fn, .immediate = 1},
        {.op = XR_XIR_FUNCTION_REF, .type = fn, .immediate = 2},
        {.op = XR_XIR_ARRAY_NEW, .type = array, .args = {0, 1}},
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64},
        {.op = XR_XIR_ARRAY_GET, .type = fn, .args = {2, 3}},
        {.op = XR_XIR_CALL_INDIRECT, .immediate = 4},
        {.op = XR_XIR_RETURN}
    };
    XrXirInstruction leaf = {.op = XR_XIR_RETURN};
    XrXirFunction functions[] = {
        {.instructions = instructions, .instruction_count = 7, .operands = operands,
            .operand_count = 1, .error_type = XR_XIR_NEVER},
        {.instructions = &leaf, .instruction_count = 1, .error_type = XR_XIR_NEVER},
        {.instructions = &leaf, .instruction_count = 1, .error_type = XR_XIR_NEVER}
    };
    XrXirModule module = {.stage = XR_XIR_CHECKED, .types = &types,
        .functions = functions, .function_count = 3};
    EffectCheckedRoots *roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, true) == XR_XIR_OK);
    XrXirValueFlow unrelated = {1, 9};
    XirStorageFlowFunction rows[] = {
        {.physical_values = 7, .value_count = 10, .flows = &unrelated, .flow_count = 1},
        {.physical_values = 1, .value_count = 1},
        {.physical_values = 1, .value_count = 1}
    };
    XirStorageFlowView storage = {.functions = rows, .function_count = 3};
    CHECK(execution_origins_receive(&effect_context, roots, &storage) == XR_XIR_OK);
    uint32_t captures = 0;
    bool complete = true;
    CHECK(xir_execution_visit_value_captures(roots, 0, 4,
        execution_value_oracle, &captures, &complete) == XR_XIR_OK);
    CHECK(!complete && captures == 1);
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
}

static void restriction_owner_relocation(void) {
    XrXirType parameter = XR_XIR_I64;
    XrXirInstruction code[] = {{.op = XR_XIR_COPY, .type = XR_XIR_I64}, {.op = XR_XIR_RETURN, .args = {1}}};
    XrXirBlock block = {.count = 2};
    XrXirFunction body = {.parameters = &parameter, .parameter_count = 1, .instructions = code,
        .instruction_count = 2, .blocks = &block, .block_count = 1, .result = XR_XIR_I64, .error_type = XR_XIR_NEVER};
    XrXirModule module = {.functions = &body, .function_count = 1};
    uint32_t intrinsic[] = {0, 1, 2, 3}, inputs[] = {3};
    XrXirValueFlow flow = {1, 3};
    XirRestrictionFormulaRow formulas[] = {{0}, {0}, {.constant = true}, {.count = 1}};
    XirRestrictionFunction row = {3, 4, intrinsic, &flow, 1};
    XirRestrictionPoint point = {0, 1, 1, 3, 3};
    XirRestrictionView view = {&row, 1, formulas, 4, inputs, 1, &point, 1};
    EffectMark baseline = effect_mark();
    EffectCheckedRoots *roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, false) == XR_XIR_OK);
    EffectMark held = effect_mark();
    EffectCheckRestrictions copy = {0};
    attempts = 0;
    CHECK(effect_check_restrictions_copy(&effect_context, roots, &view, &copy) == XR_XIR_OK);
    size_t sites = attempts;
    effect_check_restrictions_clear(&copy);
    effect_mark_check(held);
    for (size_t i = 0; i < sites; ++i) {
        attempts = 0; injected = false; fail_at = i;
        CHECK(effect_check_restrictions_copy(&effect_context, roots, &view, &copy) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected);
        fail_at = SIZE_MAX;
        effect_check_restrictions_clear(&copy);
        effect_mark_check(held);
    }
    CHECK(effect_check_restrictions_receive(roots, &view) == XR_XIR_OK);
    CHECK(roots->restrictions.view.inputs != inputs && roots->restrictions.view.points != &point);
    CHECK(effect_check_restrictions_receive(roots, &view) == XR_XIR_BAD_STRUCTURE);
    XirStorageFlowView storage = {0};
    CHECK(xir_storage_flow_seal(roots->storage, &storage) == XR_XIR_OK);
    XrXirInstruction replacement[] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_I64}, code[0], code[1]};
    XrXirBlock replacement_block = {.count = 3};
    XrXirFunction relocated = body;
    relocated.instructions = replacement; relocated.instruction_count = 3; relocated.blocks = &replacement_block;
    uint32_t values[] = {0, 2, 3};
    XirViewNormalizedFunction map = {.instructions = replacement, .values = values, .old_instructions = 2};
    XrXirModule next = module; next.functions = &relocated;
    XirViewNormalization normalization = {.module = next, .bodies = &relocated, .functions = &map};
    EffectFinalOutcomes outcomes = {0};
    XrXirEffects effects = {.resources = effect_context.resources, .count = 1,
        .outcomes_owner = &outcomes, .checked_roots = roots};
    CHECK(xir_effects_relocate_prepared(&effect_context, &effects, &next, &normalization, NULL) == XR_XIR_OK);
    const XirRestrictionView *owned = &roots->restrictions.view;
    CHECK(owned->functions[0].physical_values == 4 && owned->functions[0].value_count == 5);
    CHECK(owned->functions[0].intrinsic[2] == 1 && owned->functions[0].intrinsic[4] == 3);
    CHECK(!owned->formulas[owned->functions[0].intrinsic[1]].constant);
    CHECK(owned->inputs[0] == 4 && owned->functions[0].flows[0].source == 2 &&
        owned->functions[0].flows[0].target == 4);
    CHECK(owned->points[0].instruction == 2 && owned->points[0].value == 2 && owned->points[0].carrier == 4);
    memset(intrinsic, 0xCE, sizeof(intrinsic)); memset(inputs, 0xCE, sizeof(inputs));
    memset(formulas, 0xCE, sizeof(formulas)); memset(&point, 0xCE, sizeof(point));
    CHECK(owned->inputs[0] == 4 && owned->formulas[2].constant);
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
    printf("restriction receiver: detached formulas, %zu allocation failures and numeric relocation PASS\n", sites);
}

static void c_callable_parameter_equations(void) {
    EffectMark baseline = effect_mark();
    XrXirType callable = XR_XIR_CONSTRUCTED_TYPE_BASE, cell = callable + 1;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_UNIT, .element = XR_XIR_NEVER,
            .flags = XR_XIR_CALLABLE_C_ABI | XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_CONSTRUCTED_TYPE_BASE}
    };
    XrXirTypes types = {.nodes = nodes, .count = 2};
    XrXirInstruction ret = {.op = XR_XIR_RETURN};
    XrXirFunction functions[] = {
        {.parameters = &callable, .parameter_count = 1, .instructions = &ret, .instruction_count = 1,
            .error_type = XR_XIR_NEVER},
        {.parameters = &cell, .parameter_count = 1, .instructions = &ret, .instruction_count = 1,
            .error_type = XR_XIR_NEVER}
    };
    XrXirModule module = {.types = &types, .functions = functions, .function_count = 2};
    EffectCheckedRoots *roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, true) == XR_XIR_OK);
    for (uint32_t f = 0; f < 2; ++f) {
        CHECK(!effect_check_suspend_bits(roots, roots->function[f].root)[0]);
        CHECK(!effect_check_suspend_bits(roots, roots->function[f].first)[0]);
        CHECK(!effect_check_suspend_bits(roots, roots->function[f].contents)[0]);
    }
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
}

static XrXirStatus failing_point_request(void *context, const XirExecutionOwner *execution,
    XirStorageFlow *storage, bool final, bool *ready) {
    (void)context; (void)execution; (void)storage; (void)final;
    *ready = false;
    return XR_XIR_OUT_OF_MEMORY;
}

static void point_request_failure(void) {
    EffectMark baseline = effect_mark();
    XrXirInstruction ret = {.op = XR_XIR_RETURN};
    XrXirBlock block = {.count = 1};
    XrXirFunction function = {.instructions = &ret, .instruction_count = 1,
        .blocks = &block, .block_count = 1, .error_type = XR_XIR_NEVER};
    XrXirModule module = {.functions = &function, .function_count = 1};
    XrXirRootRefiner refiner = {.points = failing_point_request};
    EffectCheckedRoots *roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, &refiner, &roots, false) ==
        XR_XIR_OUT_OF_MEMORY && !roots);
    effect_mark_check(baseline);
}

static void execution_instance_parameters(void) {
    EffectMark baseline = effect_mark();
    XrXirType type_parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirCallableParameter callback_parameter = {.type = XR_XIR_CONSTRUCTED_TYPE_BASE};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_ARRAY, .element = type_parameter, .parameter_span = 1},
        {.kind = XR_XIR_TYPE_CALLABLE, .element = XR_XIR_NEVER, .result = XR_XIR_UNIT,
            .parameters = &callback_parameter, .parameter_count = 1, .parameter_span = 1,
            .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_CELL, .element = type_parameter, .parameter_span = 1},
        {.kind = XR_XIR_TYPE_CALLABLE, .element = XR_XIR_NEVER, .result = XR_XIR_UNIT,
            .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED}
    };
    XrXirTypes types = {.nodes = nodes, .count = 4};
    CHECK(xr_xir_compile_types_structure_verify(&effect_context, &types) == XR_XIR_OK);
    XrXirType parameters[] = {type_parameter, XR_XIR_CONSTRUCTED_TYPE_BASE + 1, XR_XIR_CONSTRUCTED_TYPE_BASE + 2};
    XrXirInstruction ret = {.op = XR_XIR_RETURN};
    XrXirBlock block = {.count = 1};
    XrXirFunction function = {.parameters = parameters, .parameter_count = 3, .instructions = &ret,
        .instruction_count = 1, .blocks = &block, .block_count = 1, .error_type = XR_XIR_NEVER};
    XrXirConstraint constraint = {0};
    XrXirGeneric generic = {.constraints = &constraint, .parameter_count = 1};
    XrXirModule module = {.types = &types, .functions = &function, .function_count = 1, .generics = &generic};
    EffectCheckedRoots *roots = NULL;
    CHECK(effect_checked_roots_collect(&effect_context, &module, NULL, NULL, NULL, &roots, false) == XR_XIR_OK);
    EffectCheckBuild build = {.memory = &effect_context, .module = &module, .roots = roots,
        .scratch = {.resources = effect_context.resources}};
    CHECK(effect_check_type_argument(&build, XR_XIR_CONSTRUCTED_TYPE_BASE + 3));
    uint32_t instance = effect_check_instance(&build, 0, 0, 1);
    CHECK(instance == 1 && build.status == XR_XIR_OK);
    for (uint32_t p = 0; p < 3; ++p) CHECK(effect_check_argument(&build, roots->function[0].first + p));
    CHECK(effect_check_call_record(&build, roots->function[0].root,
        (EffectCheckCall){.target = roots->function[instance].root, .count = 3, .protocol = 2}));
    XirExecutionSite site = {0};
    CHECK(xir_execution_site(roots, XIR_EXECUTION_CALL, 0, &site) == XR_XIR_OK);
    XirExecutionParameter formal = {0};
    CHECK(xir_execution_parameter(roots, &site, 0, &formal) == XR_XIR_OK &&
        formal.type == XR_XIR_CONSTRUCTED_TYPE_BASE + 3 && formal.escaping && !formal.cell && formal.generic_storage);
    CHECK(xir_execution_parameter(roots, &site, 1, &formal) == XR_XIR_OK &&
        (uint32_t)formal.type >= (uint32_t)XR_XIR_CONSTRUCTED_TYPE_BASE + types.count && !formal.escaping && !formal.generic_storage);
    const XrXirTypes *closed = xir_execution_types(roots);
    const XrXirTypeNode *signature = xr_xir_callable_signature(closed, formal.type);
    CHECK(signature && signature->parameter_count == 1);
    const XrXirTypeNode *array = xr_xir_type_node(closed, signature->parameters[0].type);
    CHECK(array && array->kind == XR_XIR_TYPE_ARRAY && array->element == XR_XIR_CONSTRUCTED_TYPE_BASE + 3);
    CHECK(xir_execution_parameter(roots, &site, 2, &formal) == XR_XIR_OK && formal.cell &&
        xr_xir_cell_element(closed, formal.type) == XR_XIR_CONSTRUCTED_TYPE_BASE + 3);
    CHECK(effect_check_suspend_bits(roots, roots->function[instance].root)[0] == 8);
    CHECK(effect_check_suspend_bits(roots, roots->function[instance].contents + 2)[0] == 1);
    xr_compile_resources_free(build.instance_index);
    xir_read_scratch_free(&build.scratch);
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
}

typedef struct ExecutionKindProbe {
    XirExecutionSiteKind kind;
    uint32_t count;
} ExecutionKindProbe;
static XrXirStatus execution_kind_oracle(void *context, const XirExecutionSite *site) {
    ExecutionKindProbe *probe = context;
    CHECK(site->kind == probe->kind);
    if (site->kind == XIR_EXECUTION_STORABLE) CHECK(!site->argument_count && site->target == UINT32_MAX);
    ++probe->count;
    return XR_XIR_OK;
}

static void execution_storable_boundaries(void) {
    EffectMark baseline = effect_mark();
    XrXirType fn = XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_CALLABLE, .element = XR_XIR_NEVER,
        .result = XR_XIR_UNIT, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types = {.nodes = &node, .count = 1};
    uint32_t operand = 0, escaping = 0;
    XrXirInstruction caller[] = {
        {.op = XR_XIR_CALL, .type = fn, .args = {0, 1}, .immediate = 1},
        {.op = XR_XIR_FUNCTION_WEAKEN, .type = fn, .args = {0}}, {.op = XR_XIR_RETURN, .args = {1}}
    };
    XrXirInstruction returned = {.op = XR_XIR_RETURN};
    XrXirBlock blocks[] = {{.count = 3}, {.count = 1}};
    XrXirFunction functions[] = {
        {.parameters = &fn, .parameter_count = 1, .result = fn, .blocks = blocks, .block_count = 1,
            .instructions = caller, .instruction_count = 3, .operands = &operand, .operand_count = 1,
            .error_type = XR_XIR_NEVER, .escaping_parameters = &escaping, .escaping_parameter_count = 1},
        {.parameters = &fn, .parameter_count = 1, .result = fn, .blocks = blocks + 1, .block_count = 1,
            .instructions = &returned, .instruction_count = 1, .error_type = XR_XIR_NEVER,
            .escaping_parameters = &escaping, .escaping_parameter_count = 1}
    };
    XrXirModule module = {.types = &types, .functions = functions, .function_count = 2};
    EffectCheckedRoots *roots = NULL;
    CHECK(effect_checked_roots_build(&effect_context, &module, NULL, NULL, NULL, &roots, true) == XR_XIR_OK);
    XirExecutionValue value = {0, 1, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    ExecutionKindProbe probe = {XIR_EXECUTION_STORABLE, 0};
    bool complete = false;
    uint32_t remainder = UINT32_MAX;
    CHECK(xir_execution_visit_value_environment_in(roots, &value, execution_kind_oracle,
        &probe, &complete, &remainder) == XR_XIR_OK && complete && !remainder && probe.count == 1);
    probe.count = 0;
    CHECK(xir_execution_visit_value_captures_in(roots, &value, execution_kind_oracle,
        &probe, &complete, &remainder) == XR_XIR_OK && !complete &&
        (remainder & XIR_EXECUTION_REMAINDER_OPAQUE) && !probe.count);
    value.value = 2; probe.kind = XIR_EXECUTION_FORMAL;
    CHECK(xir_execution_visit_value_environment_in(roots, &value, execution_kind_oracle,
        &probe, &complete, &remainder) == XR_XIR_OK && complete && !remainder && probe.count == 1);
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
}

static void control_summary_kernel(void) {
    control_summary_detached();
    suspension_parameter_equations();
    suspension_reason_order();
    execution_cleanup_order();
    execution_origin_namespaces();
    restriction_owner_relocation();
    c_callable_parameter_equations();
    point_request_failure();
    execution_instance_parameters();
    execution_storable_boundaries();
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
    CHECK(root_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    for (uint32_t f = 0; f < count; ++f) {
        CHECK(summary->facts[f].suspend == XR_XIR_EFFECT_MAY);
        CHECK(summary->facts[f].throws == XR_XIR_EFFECT_NONE);
        CHECK(summary->causes[f].distance == count - f - 1);
    }
    EffectControlSummary *occupied = summary;
    size_t before = attempts;
    CHECK(root_control_summary_build(&effect_context, &module, NULL, &occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == summary && attempts == before);
    xir_effect_control_free(summary);
    effect_mark_check(baseline);

    module.function_count = 4;
    rows[2][0].immediate = 0;
    rows[3][0].immediate = 3;
    summary = NULL;
    CHECK(root_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
    for (uint32_t f = 0; f < 4; ++f) CHECK(summary->facts[f].suspend == XR_XIR_EFFECT_NONE);
    xir_effect_control_free(summary);

    XrXirInstruction terminal[3] = {
        {.op = XR_XIR_CALL}, {.op = XR_XIR_SUSPEND}, {.op = XR_XIR_RETURN}
    };
    functions[2].instructions = terminal;
    functions[2].instruction_count = blocks[2].count = 3;
    summary = NULL;
    attempts = 0;
    CHECK(root_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
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
        CHECK(root_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !summary);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirCompileContext finite = effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE, 128, true});
    uint64_t held = effect_stats(&finite).held_bytes;
    summary = NULL;
    CHECK(root_control_summary_build(&finite, &module, NULL, &summary) == XR_XIR_MEMORY_LIMIT && !summary);
    effect_owner_free(&finite, held);
    effect_mark_check(baseline);

    summary = NULL;
    CHECK(root_control_summary_build(&effect_context, &module, NULL, &summary) == XR_XIR_OK);
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
    module_interface_lifetime();
    direct_root_equations();
    generic_resource_failures();
    control_summary_kernel();
    effect_case_end();
    return 0;
}
