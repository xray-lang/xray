/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * intrinsic_evaluation.h - Actual compiler staging and purpose-specific transport
 *
 * KEY CONCEPT:
 *   The existing staging producer owns the body and observed call coordinates.
 *   Full construction checking precedes pending specialization and receiving.
 */
#ifndef INTRINSIC_EVALUATION_H
#define INTRINSIC_EVALUATION_H
#include "xir/xxir_source_comptime_instances_internal.h"
#include "xir/xxir_construction_internal.h"
#include "xir/xxir_comptime_pending_internal.h"
#include "xir_comptime_construction_fixture.h"

static void evaluation_transport(Fixture *fixture) {
    XrXirCompileContext *context = &fixture->context;
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "predicate-evaluation", NULL};
    XrXirSourceRequest source_request = {.session = session, .authority = &authority,
        .context = context, .linkage_kind = XR_XIR_PROGRAM};
    const char text[] =
        "fn zero<T>()->bool{return \"xab\".startsWith(\"x\") && \"xab\".endsWith(\"b\") && \"xab\".contains(\"a\")}\n"
        "fn make<T>()->bool{return comptime zero<T>()}\n"
        "export fn run()->bool{return make<i64>()}\n";
    XrXirSourceText input = {NULL, text, sizeof(text) - 1};
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirCtfeLimits limits = {XR_XIR_CTFE_MAX_STEPS, XR_XIR_CTFE_MAX_FRAME_BYTES, XR_XIR_CTFE_MAX_OUTPUT_BYTES};
    XirSourceComptimeRequest request = {&source_request, &input, NULL, 0, target, limits, XR_XIR_CTFE_MAX_CALL_DEPTH};
    XirSourceComptimeInstances stage = {0};
    CHECK(xr_xir_compile_source_comptime_instances(&request, &stage, NULL) == XR_XIR_OK && stage.owner && stage.count);
    XirCtfeInstanceView *views = xr_calloc(stage.count, sizeof(*views));
    XirSourceComptimeBinding *bindings = xr_calloc(stage.count, sizeof(*bindings));
    CHECK(views && bindings);
    CHECK(xr_xir_compile_source_comptime_instances_view(&stage, views, bindings, stage.count) == XR_XIR_CTFE_OK);
    const XrXirArtifact *original = views[0].definition;
    XrXirComptimeSite site = {bindings[0].source_function, bindings[0].source_instruction, bindings[0].helper};
    CHECK(original && site.caller < original->module.function_count);
    const XrXirFunction *caller = &original->module.functions[site.caller];
    CHECK(site.instruction < caller->instruction_count && caller->instructions[site.instruction].op == XR_XIR_CALL &&
        caller->instructions[site.instruction].immediate == site.helper);
    // This compiler-only input records the observed call as an unresolved site.
    // It does not claim a public Source snapshot or invent a function body.
    XrXirComptimeTable table = {&site, 1, NULL, 0};
    XrXirConstruction *construction = NULL;
    CHECK(xir_fixture_comptime_construction(context, original, &table, &construction) == XR_XIR_OK);
    XrXirModule built = original->module;
    built.stage = XR_XIR_BUILT;
    XrXirArtifact *unbound = NULL;
    CHECK(xr_xir_compile_check_v2(context, &built, construction, &unbound, NULL) == XR_XIR_OK);
    xr_xir_compile_construction_free(construction);
    xr_xir_compile_source_comptime_instances_free(&stage);
    xr_compile_session_free(session);
    xr_free(views); xr_free(bindings);
    XirComptimePending *pending = NULL;
    CHECK(xr_xir_compile_comptime_pending(unbound, &pending, NULL) == XR_XIR_OK && pending);
    xr_xir_compile_artifact_free(unbound);
    XrXirArtifact *evaluated = NULL;
    CHECK(xr_xir_compile_comptime_pending_specialize(pending, &evaluated, NULL) == XR_XIR_OK);
    xr_xir_compile_comptime_pending_free(pending);
    XrXirCheckedPacket packet = {0};
    CHECK(evaluated->evaluation_only && xir_evaluation_write(evaluated, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(evaluated);
    XrXirArtifact *received = NULL;
    CHECK(xir_evaluation_read(context, packet.bytes, packet.length, &received, NULL) == XR_XIR_OK);
    CHECK(received->evaluation_only && get32(packet.bytes + 12) == get32(fixture->packet.bytes + 12));
    XrXirCtfeCallRequest evaluation = {.evaluation = {.function = received->module.provenance->evaluation.callee,
        .target = target, .limits = limits}, .call_depth = XR_XIR_CTFE_MAX_CALL_DEPTH};
    xr_sha256(packet.bytes, packet.length, evaluation.evaluation.checked_identity);
    XrXirArtifact *code = NULL;
    CHECK(xr_xir_compile_lower(received, &target, &code, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(received);
    xr_xir_compile_checked_packet_free(&packet);
    evaluation.evaluation.artifact = code;
    XrXirValue value = {0};
    CHECK(xr_xir_compile_ctfe_calls(&evaluation, &value) == XR_XIR_CTFE_OK);
    CHECK(value.type == XR_XIR_BOOL && !value.reserved && value.payload == 1);
    xr_xir_value_drop(&value);
    xr_xir_compile_artifact_free(code);
    puts("compiler-internal observed-site Evaluation transport and CTFE passed; public snapshot is separate");
}
#endif // INTRINSIC_EVALUATION_H
