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
#include "base/xsha256.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_class_cycles.h"
#include "xir/xxir_file_stat_native.h"
#include "xir/xxir_type_description.h"
#include "xir/xxir_types.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_comptime_pending_internal.h"
#include "xir/xxir_source_comptime_instances_internal.h"
#include "xir/xxir_source_builtin.h"
#include "xir/xxir_source_query_internal.h"
#include "shared/xr_core_intrinsic.h"
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
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "lir-source-input", NULL};
    XrXirSourceRequest request = {.session = session,
                                  .authority = &authority,
                                  .context = &context,
                                  .linkage_kind = XR_XIR_PROGRAM, .target = XR_XIR_HOST_TARGET};
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirSourceText input = {NULL, text, strlen(text)};
    XrXirStatus status =
        xr_xir_compile_source_check_text(&request, &input, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK)
        fprintf(stderr, "source %u:%d:%d: %s\n", (unsigned) status, diagnostic.line,
                diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    CHECK(xr_xir_target_is_host(xr_xir_compile_artifact_target(result.checked)));
    const XrXirTarget host = request.target;
    bool match = false;
    CHECK(xr_target_fact_matches(&host, (XrTargetFact){XR_TARGET_QUERY_OPERATING_SYSTEM,
        (uint16_t)host.os}, &match) && match);
    const uint16_t foreign_os = host.os == XR_TARGET_QUERY_VALUE_OS_WINDOWS ?
        XR_TARGET_QUERY_VALUE_OS_LINUX : XR_TARGET_QUERY_VALUE_OS_WINDOWS;
    CHECK(xr_target_fact_matches(&host, (XrTargetFact){XR_TARGET_QUERY_OPERATING_SYSTEM,
        foreign_os}, &match) && !match);
    match = true;
    CHECK(!xr_target_fact_matches(&host, (XrTargetFact){UINT16_MAX, foreign_os}, &match) && match);
    CHECK(result.checked->checked_effects);
#if !XR_XIR_PARANOID
    CHECK(!result.checked->source_facts&&!result.checked->checked_packet.bytes);
#endif
    XrXirDiagnostic verified={0};
    XrXirStatus verification=xr_xir_compile_artifact_verify(result.checked,&verified);
    if(verification!=XR_XIR_OK)
        fprintf(stderr,"source verification status=%u function=%u block=%u instruction=%u\n%s\n",
            (unsigned)verification,verified.function,verified.block,verified.instruction,text);
    XIR_OK(verification);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
    return result;
}

static void source_target_admission(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILE_RESOURCE_OK);
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "lir-source-target", NULL};
    const XrXirSourceText invalid = {NULL, ":", 1};
    const XrXirTarget host = xr_xir_target_host();
    const uint32_t foreign_os = host.os == XR_TARGET_QUERY_VALUE_OS_WINDOWS ?
        XR_TARGET_QUERY_VALUE_OS_LINUX : XR_TARGET_QUERY_VALUE_OS_WINDOWS;
    const uint32_t foreign_machine = host.architecture == XR_TARGET_QUERY_VALUE_ARCH_X86_64 ?
        XR_TARGET_QUERY_VALUE_ARCH_AARCH64 : XR_TARGET_QUERY_VALUE_ARCH_X86_64;
    const XrXirTarget invalid_targets[] = {{0},
        {foreign_os, host.architecture, host.abi_version},
        {host.os, foreign_machine, host.abi_version},
        {host.os, host.architecture, host.abi_version + 1}};
    XrXirSourceRequest request = {.session = session, .authority = &authority,
        .context = &context, .linkage_kind = XR_XIR_PROGRAM,
    .target = XR_XIR_HOST_TARGET};
    for (size_t i = 0; i < sizeof(invalid_targets) / sizeof(invalid_targets[0]); ++i) {
        request.target = invalid_targets[i];
        XrXirSourceResult result = {0};
        XrXirSourceDiagnostic diagnostic = {0};
        CHECK(xr_xir_compile_source_check_text(&request, &invalid, &result,
            &diagnostic, NULL) == XR_XIR_UNSUPPORTED);
        CHECK(!result.checked && !result.snapshot &&
            !strcmp(diagnostic.message, "unsupported source compilation target"));
    }
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
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
    XIR_OK(xr_xir_compile_specialize(result.checked, &closed, &diagnostic, NULL));
    CHECK(xr_xir_target_is_host(xr_xir_compile_artifact_target(closed)));
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
    XIR_OK(xr_xir_compile_specialize(result.checked, &closed, NULL, NULL));
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
    XrXirTarget target = XR_XIR_HOST_TARGET;
    XIR_OK(xr_xir_compile_specialize(result.checked, &closed, NULL, NULL));
    XIR_OK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL));
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&result);
    XIR_OK(xr_xir_compile_artifact_verify(lowered, NULL));
    CHECK(xr_xir_compile_artifact_module(lowered)->stage == XR_XIR_LOWERED);
    rejected_origin_stage(lowered);
    xr_xir_compile_artifact_free(lowered);
    puts("source origins reject a genuine Lowered artifact without changing output");
}

static void evaluation_origin_rejected(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "lir-evaluation-input", NULL};
    XrXirSourceRequest source_request = {.session = session,
                                         .authority = &authority,
                                         .context = &context,
                                         .linkage_kind = XR_XIR_PROGRAM, .target = XR_XIR_HOST_TARGET};
    const char *text = "fn make<T>()->i64{return comptime {const v:Array<T>=[];len(v)+42}}\n"
                       "export fn run()->i64{return make<i8>()+make<string>()-42}\n";
    XrXirSourceText input = {NULL, text, strlen(text)};
    XirSourceComptimeRequest request = {
        &source_request,
        &input,
        NULL,
        0,
        XR_XIR_HOST_TARGET,
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
    for(unsigned i=0;i<2;++i) {
        CHECK(views[i].lowered->program_final);
#if !XR_XIR_PARANOID
        CHECK(!views[i].checked->checked_packet.bytes&&!views[i].checked->checked_packet.length);
        CHECK(!views[i].lowered->checked_packet.bytes&&!views[i].lowered->checked_packet.length);
        CHECK(!views[i].definition->checked_packet.bytes&&!views[i].definition->checked_packet.length);
        CHECK(!views[i].checked->source_facts&&!views[i].lowered->source_facts&&!views[i].definition->source_facts);
#endif
        CHECK(views[i].value->type==XR_XIR_I64&&views[i].value->payload==42);
        XIR_OK(xr_xir_compile_artifact_verify(views[i].lowered,NULL));
    }
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
    XIR_OK(xr_xir_compile_comptime_pending(source, &pending, NULL, NULL));
    CHECK(pending != NULL);
    xr_xir_compile_artifact_free(source);
    xr_xir_compile_source_comptime_instances_free(&instances);
    XIR_OK(xr_xir_compile_comptime_pending_specialize(pending, &evaluation, NULL, NULL));
    xr_xir_compile_comptime_pending_free(pending);
    XIR_OK(xr_xir_compile_artifact_verify(evaluation, NULL));
    CHECK(evaluation->module.stage == XR_XIR_CHECKED && evaluation->evaluation_only == 1 &&
          evaluation->module.provenance->kind == XR_XIR_EVIDENCE_EVALUATION);
    rejected_origin_stage(evaluation);
    XrXirCheckedPacket packet = {0};
    XIR_OK(xir_evaluation_write(evaluation, &packet, NULL, NULL));
    XrXirArtifact *decoded = NULL;
    XIR_OK(xir_evaluation_read(&evaluation->context, packet.bytes, packet.length, &decoded, NULL, NULL));
    CHECK(xr_xir_target_is_host(xr_xir_compile_artifact_target(decoded)));
    CHECK(xr_xir_target_is_host(xr_xir_compile_artifact_target(decoded->module.provenance->source)));
    xr_xir_compile_artifact_free(decoded);
    decoded = NULL;
    memset(packet.bytes + 20, 0, 4);
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, packet.bytes, 32);
    xr_sha256_update(&hash, packet.bytes + 64, packet.length - 64);
    xr_sha256_final(&hash, packet.bytes + 32);
    CHECK(xir_evaluation_read(&evaluation->context, packet.bytes, packet.length, &decoded,
        NULL, NULL) == XR_XIR_BAD_LAYOUT && !decoded);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(evaluation);
    puts("source origins reject genuine compiler-only evaluation input without changing output");
}

static void literal_output_source_facts(void) {
    XrXirSourceResult source = compile_text("print(\"A\\0\\u{e9}\")\nvar i=0\nwhile(i<3){print(i);i=i+1}\nprint(\"\")\n");
    XrXirArtifact *closed = NULL;
    XIR_OK(xr_xir_compile_specialize(source.checked, &closed, NULL, NULL));
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    const XrXirDeclarations *d = module->declarations;
    CHECK(d->slot_count == 1 && d->slots[0].mutable &&
        xr_xir_cell_element(module->types, d->slots[0].type) == XR_XIR_I64);
    const unsigned char expected[] = {'A', 0, 0xc3, 0xa9};
    bool bytes = false, empty = false;
    for (uint32_t i = 0; i < d->literal_count; ++i) {
        bytes |= d->literals[i].length == sizeof(expected) &&
            !memcmp(d->literals[i].bytes, expected, sizeof(expected));
        empty |= d->literals[i].length == 0;
    }
    CHECK(bytes && empty);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
}

static void integer_parse_checked_roundtrip(void) {
    XrXirSourceResult source = compile_text(
        "fn parse(text:string)->i64? { return i64.tryParse(text) }\n"
        "fn magnitude(value:i64)->i64 { return value.abs() }\n"
        "print(magnitude(parse(\"42\")!))\n");
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(source.snapshot);
    unsigned integer_types = 0;
    for (uint32_t i = 0; i < view->declaration_count; ++i) {
        const XrXirSourceDeclaration *declaration = &view->declarations[i];
        integer_types += declaration->kind == XR_XIR_SOURCE_TYPE && declaration->type.type == XR_XIR_I64 &&
            declaration->native_identity != 0 && !strcmp(declaration->name,"i64");
    }
    CHECK(integer_types == 1);
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *loaded = NULL, *closed = NULL;
    XIR_OK(xr_xir_compile_checked_write(source.checked, &packet, NULL));
    XIR_OK(xr_xir_compile_checked_read(&source.checked->context, packet.bytes, packet.length, &loaded, NULL));
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_source_result_free(&source);
    XIR_OK(xr_xir_compile_specialize(loaded, &closed, NULL, NULL));
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    unsigned parses = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            if (op->op != XR_XIR_I64_TRY_PARSE) continue;
            CHECK(xr_xir_operand_type(fn, op->args[0]) == XR_XIR_STRING);
            CHECK(xr_xir_nullable_element(module->types, op->type) == XR_XIR_I64);
            ++parses;
        }
    }
    CHECK(parses == 1);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(loaded);
}

static void builtin_loader_identity(void) {
    XrXirSourceResult source=compile_text("assert(true)\nassertEqual(1,1)\n");
    const XrXirModule *checked=xr_xir_compile_artifact_module(source.checked);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source.snapshot);
    XrXirSourceBuiltinBinding bindings[2]={{0}};
    uint32_t count=0;
    for (uint32_t f=0;f<checked->function_count;++f) {
        XrXirSourceBuiltinBinding binding={0};
        if (!xr_xir_compile_source_builtin_binding(source.snapshot,f,&binding)) continue;
        CHECK(count<2 && binding.function==f && binding.declaration && binding.declaration<=view->declaration_count);
        CHECK(binding.builtin==XR_CORE_BUILTIN_ASSERT || binding.builtin==XR_CORE_BUILTIN_ASSERT_EQUAL);
        CHECK(view->declarations[binding.declaration-1].native_identity==binding.builtin);
        bindings[count++]=binding;
    }
    CHECK(count==2 && bindings[0].builtin!=bindings[1].builtin);
    XrXirSourceSnapshot *observations=NULL;
    XIR_OK(xr_xir_compile_source_snapshot_copy_v2(&source.checked->context,view,
        xr_xir_compile_artifact_construction(source.checked),&observations));
    XrXirSourceBuiltinBinding untouched={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for (uint32_t f=0;f<checked->function_count;++f) {
        CHECK(!xr_xir_compile_source_builtin_binding(observations,f,&untouched));
        CHECK(untouched.builtin==UINT32_MAX && untouched.function==UINT32_MAX && untouched.declaration==UINT32_MAX);
    }
    xr_xir_compile_source_snapshot_free(observations);
    XrXirSourceSnapshot *owned=source.snapshot; source.snapshot=NULL;
    xr_xir_compile_source_result_free(&source);
    for (uint32_t n=0;n<count;++n) {
        XrXirSourceBuiltinBinding actual={0};
        CHECK(xr_xir_compile_source_builtin_binding(owned,bindings[n].function,&actual));
        CHECK(actual.builtin==bindings[n].builtin && actual.function==bindings[n].function &&
            actual.declaration==bindings[n].declaration);
    }
    CHECK(!xr_xir_compile_source_builtin_binding(owned,UINT32_MAX,&untouched) &&
        !xr_xir_compile_source_builtin_binding(NULL,0,&untouched) &&
        !xr_xir_compile_source_builtin_binding(owned,bindings[0].function,NULL));
    CHECK(untouched.builtin==UINT32_MAX && untouched.function==UINT32_MAX && untouched.declaration==UINT32_MAX);
    xr_xir_compile_source_snapshot_free(owned);
}

static XrXirType cycle_test_type(const XrXirArtifact *artifact, const char *name,
    XrXirType first, XrXirType second, bool callable) {
    const XrXirTypes *types = xr_xir_compile_artifact_module(artifact)->types;
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    size_t length = strlen(name);
    uint32_t arguments = second != XR_XIR_UNIT ? 2 : first != XR_XIR_UNIT || callable ? 1 : 0;
    for (uint32_t i = 0; i < types->count; ++i) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + i);
        const XrXirTypeNode *node = &types->nodes[i];
        if (!xr_xir_type_is_class(types, type) || node->parameter_span || node->nominal.argument_count != arguments) continue;
        XrXirTypeDescription description = {0};
        XIR_OK(xr_xir_compile_type_description(context, types, type, &description));
        if (length != description.name.length || memcmp(name, description.name.bytes, length)) continue;
        const XrXirType *actual = node->nominal.arguments;
        if (arguments == 2 && actual[1] != second) continue;
        if (callable) {
            const XrXirTypeNode *signature = xr_xir_type_node(types, actual[0]);
            if (!signature || signature->kind != XR_XIR_TYPE_CALLABLE) continue;
        } else if (arguments && actual[0] != first) continue;
        return type;
    }
    CHECK(false);
    return XR_XIR_UNIT;
}

static void class_cycle_facts(void) {
    const char *text =
        "class Box<T> { value: T; init(v: T) { this.value = v } }\n"
        "class Pair<A, B> { first: A; second: B; init(a: A, b: B) { this.first = a; this.second = b } }\n"
        "class ArrayLoop { backing: Array<ArrayLink>; init() { this.backing = [] } }\n"
        "struct ArrayLink { next: ArrayLoop? }\n"
        "class MapLoop { backing: Map<string, MapLink>; init() { this.backing = {} } }\n"
        "struct MapLink { next: MapLoop? }\n"
        "class Holder { value: ArrayLoop; init(v: ArrayLoop) { this.value = v } }\n"
        "var words = Box(\"text\")\n"
        "var number = Box(42)\n"
        "var pair = Pair(\"value\", 42)\n"
        "var callback = Box(fn(v: i64) -> i64 { return v })\n"
        "var circle = ArrayLoop()\n"
        "var map_circle = MapLoop()\n"
        "var incoming = Holder(circle)\n";
    XrXirSourceResult source = compile_text(text);
    XrXirClassCycles *cycles = NULL;
    CHECK(xr_xir_class_cycles_build(source.checked, &cycles, NULL) == XR_XIR_BAD_STAGE && !cycles);
    XrXirArtifact *closed = NULL;
    XIR_OK(xr_xir_compile_specialize(source.checked, &closed, NULL, NULL));
    XIR_OK(xr_xir_class_cycles_build(closed, &cycles, NULL));
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    const struct { const char *name; XrXirType first, second; bool callable; XrXirClassCycleKind kind; } cases[] = {
        {"Box", XR_XIR_STRING, XR_XIR_UNIT, false, XR_XIR_CLASS_CYCLE_ACYCLIC},
        {"Box", XR_XIR_I64, XR_XIR_UNIT, false, XR_XIR_CLASS_CYCLE_ACYCLIC},
        {"Pair", XR_XIR_STRING, XR_XIR_I64, false, XR_XIR_CLASS_CYCLE_ACYCLIC},
        {"Box", XR_XIR_UNIT, XR_XIR_UNIT, true, XR_XIR_CLASS_CYCLE_UNKNOWN},
        {"ArrayLoop", XR_XIR_UNIT, XR_XIR_UNIT, false, XR_XIR_CLASS_CYCLE_POSSIBLE},
        {"MapLoop", XR_XIR_UNIT, XR_XIR_UNIT, false, XR_XIR_CLASS_CYCLE_POSSIBLE},
        {"Holder", XR_XIR_UNIT, XR_XIR_UNIT, false, XR_XIR_CLASS_CYCLE_ACYCLIC}
    };
    for (size_t n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        XrXirType type = cycle_test_type(closed, cases[n].name, cases[n].first, cases[n].second, cases[n].callable);
        XrXirClassCycleKind kind = XR_XIR_CLASS_CYCLE_UNKNOWN;
        XIR_OK(xr_xir_class_cycles_query(cycles, closed, type, &kind));
        CHECK(kind == cases[n].kind);
    }
    XrXirType number = cycle_test_type(closed, "Box", XR_XIR_I64, XR_XIR_UNIT, false);
    XrXirType callback = cycle_test_type(closed, "Box", XR_XIR_UNIT, XR_XIR_UNIT, true);
    CHECK(number != callback && xr_xir_type_node(module->types, number)->nominal.declaration ==
        xr_xir_type_node(module->types, callback)->nominal.declaration);
    XrXirClassCycleKind preserved = XR_XIR_CLASS_CYCLE_POSSIBLE;
    CHECK(xr_xir_class_cycles_query(cycles, source.checked, number, &preserved) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_class_cycles_query(cycles, closed, XR_XIR_I64, &preserved) == XR_XIR_BAD_TYPE);
    CHECK(preserved == XR_XIR_CLASS_CYCLE_POSSIBLE);
    xr_xir_class_cycles_free(cycles);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&source);
}

static void file_stat_cycle_facts(void) {
#if defined(XR_OS_WINDOWS)
#define FILE_STAT_TEST_ROOT "C:/xray-class-cycles-test"
#else
#define FILE_STAT_TEST_ROOT "/xray-class-cycles-test"
#endif
    static const char path_source[] =
        "export class Path{raw:string\ninit(raw:string){this.raw=raw}}\n";
    static const char io_source[] =
        "import {Path} from path\n"
        "export fn exists(path: Path) -> bool { return __stat(path) != null }\n"
        "print(exists(Path(\"fixture\")))\n";
    const XrModuleIdentityAuthority io = {XR_MODULE_IDENTITY_STDLIB, "io", FILE_STAT_TEST_ROOT};
    const XrModuleOverlayInput overlays[] = {
        {io, "io/io.xr", FILE_STAT_TEST_ROOT "/io/io.xr", io_source, sizeof(io_source) - 1},
        {{XR_MODULE_IDENTITY_STDLIB, "path", FILE_STAT_TEST_ROOT}, "path/path.xr",
            FILE_STAT_TEST_ROOT "/path/path.xr", path_source, sizeof(path_source) - 1}
    };
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILE_RESOURCE_OK);
    const XrXirSourceRequest request = {.session=session, .entry_path=overlays[0].source_path, .authority=&io,
        .context=&context, .stdlib_path=FILE_STAT_TEST_ROOT, .linkage_kind=XR_XIR_PROGRAM, .target=XR_XIR_HOST_TARGET};
#undef FILE_STAT_TEST_ROOT
    XrXirSourceResult source = {0};
    XIR_OK(xr_xir_compile_source_check_overlay(&request, overlays, sizeof(overlays) / sizeof(*overlays),
        &source, NULL, NULL));
    XrXirArtifact *closed = NULL;
    XIR_OK(xr_xir_compile_specialize(source.checked, &closed, NULL, NULL));
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    XrXirType type = XR_XIR_UNIT;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->declarations->functions[f].native_callable != XR_NATIVE_CALLABLE_IO_STAT) continue;
        XrXirType result = module->functions[f].result;
        CHECK(type == XR_XIR_UNIT && xir_file_stat_nullable(module->types, result));
        type = xr_xir_type_node(module->types, result)->element;
    }
    CHECK(type != XR_XIR_UNIT);
    XrXirClassCycles *cycles = NULL;
    XIR_OK(xr_xir_class_cycles_build(closed, &cycles, NULL));
    XrXirClassCycleKind kind = XR_XIR_CLASS_CYCLE_UNKNOWN;
    XIR_OK(xr_xir_class_cycles_query(cycles, closed, type, &kind));
    CHECK(kind == XR_XIR_CLASS_CYCLE_ACYCLIC);
    xr_xir_class_cycles_free(cycles);

    XrXirTypes altered_types = *module->types;
    XrXirNominalTable altered_table = *altered_types.nominals;
    CHECK(altered_table.declarations);
    size_t bytes = altered_table.count * sizeof(*altered_table.declarations);
    XrXirNominalDeclaration *declarations = xr_malloc(bytes);
    CHECK(declarations);
    memcpy(declarations, altered_table.declarations, bytes);
    uint32_t index = xr_xir_type_node(module->types, type)->nominal.declaration;
    declarations[index].native.source_fingerprint[0] ^= 1;
    altered_table.declarations = declarations;
    altered_types.nominals = &altered_table;
    XrXirArtifact altered = *closed;
    altered.module.types = &altered_types;
    cycles = NULL;
    XIR_OK(xr_xir_class_cycles_build(&altered, &cycles, NULL));
    XIR_OK(xr_xir_class_cycles_query(cycles, &altered, type, &kind));
    CHECK(kind == XR_XIR_CLASS_CYCLE_UNKNOWN);
    xr_xir_class_cycles_free(cycles);
    xr_free(declarations);

    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    xr_compile_resources_release(context.resources);
}

int main(void) {
    source_target_admission();
    scalar_input_facts();
    literal_output_source_facts();
    builtin_loader_identity();
    integer_parse_checked_roundtrip();
    specialization_origins();
    lowered_origin_rejected();
    evaluation_origin_rejected();
    class_cycle_facts();
    file_stat_cycle_facts();
    return 0;
}
