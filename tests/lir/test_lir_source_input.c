/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_source_input.c - Real Checked inputs and specialization source mapping
 */
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_comptime_pending_internal.h"
#include "xir/xxir_source_comptime_instances_internal.h"
#include "../unit/xir/xir_comptime_construction_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%d: %s\n", __LINE__, #c);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define XIR_OK(call) CHECK((call) == XR_XIR_OK)

static XrXirSourceResult compile_text(const char *text) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "lir-source-input", NULL};
    XrXirSourceRequest request = {.session = session,
                                  .authority = &authority,
                                  .context = &context,
                                  .linkage_kind = XR_XIR_PROGRAM};
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirSourceText input = {NULL, text, strlen(text)};
    XrXirStatus status =
        xr_xir_compile_source_check_text(&request, &input, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK)
        fprintf(stderr, "source %u:%d:%d: %s\n", (unsigned) status, diagnostic.line,
                diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
    return result;
}

static unsigned count_op(const XrXirFunction *function, XrXirOp op) {
    unsigned count = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i)
        if (function->instructions[i].op == op)
            ++count;
    return count;
}

static void scalar_input_facts(void) {
    const char *text = "fn fib(n:i64)->i64 {\nif(n<2){return n}\nreturn fib(n-1)+fib(n-2)\n}\n"
                       "fn sum(n:i64)->i64 {\nvar i=0\nvar "
                       "total=0\nwhile(i<n){total=total+i;i=i+1}\nreturn total\n}\n"
                       "print(fib(10))\nprint(sum(20))\n";
    XrXirSourceResult result = compile_text(text);
    XrXirArtifact *closed = NULL;
    XrXirDiagnostic diagnostic = {0};
    XIR_OK(xr_xir_compile_specialize(result.checked, &closed, &diagnostic));
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    const XrXirDeclarations *declarations = module->declarations;
    CHECK(declarations && declarations->root_module < declarations->module_count);
    uint32_t init = declarations->modules[declarations->root_module].initializer;
    CHECK(init < module->function_count && declarations->entry_function < module->function_count &&
          init != declarations->entry_function);
    CHECK(count_op(&module->functions[init], XR_XIR_PRINT) == 2);
    CHECK(count_op(&module->functions[declarations->entry_function], XR_XIR_PRINT) == 0);
    bool sum = false;
    unsigned cells = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != 3 || memcmp(function->name, "sum", 3))
            continue;
        CHECK(!sum);
        sum = true;
        cells = count_op(function, XR_XIR_CELL_NEW);
        CHECK(cells == 2 && count_op(function, XR_XIR_CELL_READ) >= 3 &&
              count_op(function, XR_XIR_CELL_WRITE) == 2);
    }
    CHECK(sum);
    printf("scalar Checked input: %u modules, %u functions, %u local Cells, initializer %u, entry "
           "%u\n",
           declarations->module_count, module->function_count, cells, init,
           declarations->entry_function);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&result);
}

static void specialization_origins(void) {
    XrXirSourceResult result =
        compile_text("fn identity<T>(value:T)->T{return value}\n"
                     "fn scalar()->i64{return identity(42)}\nprint(scalar())\n");
    const XrXirSourceDebugView *debug = xr_xir_compile_source_snapshot_debug(result.snapshot);
    CHECK(debug && debug->function_count && debug->functions);
    XrXirArtifact *closed = NULL;
    XIR_OK(xr_xir_compile_specialize(result.checked, &closed, NULL));
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    uint32_t *origins = xr_malloc((size_t) module->function_count * sizeof(*origins));
    CHECK(origins != NULL);
    bool reordered = false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XIR_OK(xr_xir_compile_specialization_origin(closed, f, &origins[f]));
        CHECK(origins[f] < debug->function_count);
        const XrXirSourceDebugFunction *source = &debug->functions[origins[f]];
        CHECK(source->parameter_count == module->functions[f].parameter_count &&
              source->instruction_count == module->functions[f].instruction_count);
        if (origins[f] != f)
            reordered = true;
    }
    CHECK(reordered);
    uint32_t untouched = UINT32_MAX;
    CHECK(xr_xir_compile_specialization_origin(NULL, 0, &untouched) == XR_XIR_BAD_STRUCTURE &&
          untouched == UINT32_MAX);
    CHECK(xr_xir_compile_specialization_origin(closed, 0, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_specialization_origin(closed, module->function_count, &untouched) ==
              XR_XIR_BAD_VALUE &&
          untouched == UINT32_MAX);
    xr_xir_compile_artifact_free(result.checked);
    result.checked = NULL;
    xr_xir_compile_source_snapshot_free(result.snapshot);
    result.snapshot = NULL;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        uint32_t origin = UINT32_MAX;
        XIR_OK(xr_xir_compile_specialization_origin(closed, f, &origin));
        CHECK(origin == origins[f]);
    }
    xr_free(origins);
    xr_xir_compile_artifact_free(closed);
    puts("specialized function origins survive producer and source snapshot destruction");
}

static void rejected_origin_stage(const XrXirArtifact *artifact) {
    uint32_t untouched = UINT32_MAX;
    CHECK(xr_xir_compile_specialization_origin(artifact, 0, &untouched) == XR_XIR_BAD_STAGE &&
          untouched == UINT32_MAX);
    CHECK(xr_xir_compile_specialization_origin(artifact, UINT32_MAX, &untouched) ==
              XR_XIR_BAD_STAGE &&
          untouched == UINT32_MAX);
}

static void lowered_origin_rejected(void) {
    XrXirSourceResult result = compile_text("fn scalar()->i64{return 42}\nprint(scalar())\n");
    XrXirArtifact *closed = NULL, *lowered = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XIR_OK(xr_xir_compile_specialize(result.checked, &closed, NULL));
    XIR_OK(xr_xir_compile_lower(closed, &target, &lowered, NULL));
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&result);
    XIR_OK(xr_xir_compile_artifact_verify(lowered, NULL));
    CHECK(xr_xir_compile_artifact_module(lowered)->stage == XR_XIR_LOWERED);
    rejected_origin_stage(lowered);
    xr_xir_compile_artifact_free(lowered);
    puts("source origins reject a genuine Lowered artifact without changing output");
}

static void evaluation_origin_rejected(void) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "lir-evaluation-input", NULL};
    XrXirSourceRequest source_request = {.session = session,
                                         .authority = &authority,
                                         .context = &context,
                                         .linkage_kind = XR_XIR_PROGRAM};
    const char *text = "fn make<T>()->i64{return comptime {const v:Array<T>=[];len(v)+42}}\n"
                       "export fn run()->i64{return make<i8>()+make<string>()-42}\n";
    XrXirSourceText input = {NULL, text, strlen(text)};
    XirSourceComptimeRequest request = {
        &source_request,
        &input,
        NULL,
        0,
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        {XR_XIR_CTFE_MAX_STEPS, XR_XIR_CTFE_MAX_FRAME_BYTES, XR_XIR_CTFE_MAX_OUTPUT_BYTES},
        XR_XIR_CTFE_MAX_CALL_DEPTH};
    XirSourceComptimeInstances instances = {0};
    XIR_OK(xr_xir_compile_source_comptime_instances(&request, &instances, NULL));
    CHECK(instances.count == 2);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
    XirCtfeInstanceView views[2] = {0};
    XirSourceComptimeBinding bindings[2] = {0};
    CHECK(xr_xir_compile_source_comptime_instances_view(&instances, views, bindings, 2) ==
          XR_XIR_CTFE_OK);
    CHECK(views[0].definition == views[1].definition);
    const XrXirArtifact *checked = views[0].definition;
    XrXirComptimeSite site = {bindings[0].source_function, bindings[0].source_instruction,
                              bindings[0].helper};
    CHECK(bindings[1].source_function == site.caller &&
          bindings[1].source_instruction == site.instruction && bindings[1].helper == site.helper);
    XrXirComptimeTable table = {&site, 1, NULL, 0};
    /* Recheck complete Source with a missing key so the ordinary compiler
     * producer, rather than a forged purpose bit, creates evaluation input. */
    XrXirConstruction *construction = NULL;
    XIR_OK(xir_fixture_comptime_construction(&checked->context, checked, &table, &construction));
    XrXirModule built = checked->module;
    built.stage = XR_XIR_BUILT;
    XrXirArtifact *source = NULL, *evaluation = NULL;
    XIR_OK(xr_xir_compile_check_v2(&checked->context, &built, construction, &source, NULL));
    xr_xir_compile_construction_free(construction);
    XirComptimePending *pending = NULL;
    XIR_OK(xr_xir_compile_comptime_pending(source, &pending, NULL));
    CHECK(pending != NULL);
    xr_xir_compile_artifact_free(source);
    xr_xir_compile_source_comptime_instances_free(&instances);
    XIR_OK(xr_xir_compile_comptime_pending_specialize(pending, &evaluation, NULL));
    xr_xir_compile_comptime_pending_free(pending);
    XIR_OK(xr_xir_compile_artifact_verify(evaluation, NULL));
    CHECK(evaluation->module.stage == XR_XIR_CHECKED && evaluation->evaluation_only == 1 &&
          evaluation->module.provenance->kind == XR_XIR_EVIDENCE_EVALUATION);
    rejected_origin_stage(evaluation);
    xr_xir_compile_artifact_free(evaluation);
    puts("source origins reject genuine compiler-only evaluation input without changing output");
}

int main(void) {
    scalar_input_facts();
    specialization_origins();
    lowered_origin_rejected();
    evaluation_origin_rejected();
    return 0;
}
