/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_task_source_prepare.h - Failure-safe compound Task Source preparation
 */
#ifndef XIR_MIXED_TASK_SOURCE_PREPARE_H
#define XIR_MIXED_TASK_SOURCE_PREPARE_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#ifndef XR_MIXED_TASK_RESULT_MODULES
#define XR_MIXED_TASK_RESULT_MODULES 2
#endif

typedef struct TaskSourcePrepare {
    const XrXirCompileContext *context;
    XrCompilerSession *session;
    XrXirSourceResult source;
    XrXirCheckedPacket packet;
    XrXirArtifact *checked, *closed, *lowered;
    XrXirCSource generated;
    uint32_t entries[3];
    XrXirStatus status;
    unsigned stage;
} TaskSourcePrepare;

static bool task_source_stage(TaskSourcePrepare *state, unsigned stage, XrXirStatus status) {
    state->stage = stage; state->status = status; return status == XR_XIR_OK;
}
static void task_source_entries(const XrXirModule *module, uint32_t entries[3]) {
    static const char *const names[] = {"run", "observed", "words"};
    if (module->declarations->module_count != 2 || module->declarations->slot_count != 1) {
        const XrXirDeclarations *declarations = module->declarations;
        fprintf(stderr, "Task Source shape modules=%u slots=%u functions=%u root=%u\n",
            declarations->module_count, declarations->slot_count, module->function_count, declarations->root_module);
        for (uint32_t i = 0; i < declarations->module_count; ++i)
            fprintf(stderr, "Task Source module=%u name=%.*s\n", i, (int)declarations->modules[i].name_length,
                declarations->modules[i].name);
        for (uint32_t i = 0; i < declarations->slot_count; ++i)
            fprintf(stderr, "Task Source slot=%u module=%u type=%u mutable=%u\n", i,
                declarations->slots[i].module, declarations->slots[i].type, declarations->slots[i].mutable);
    }
    CHECK(module->declarations->module_count == XR_MIXED_TASK_RESULT_MODULES && module->declarations->slot_count == 1);
    CHECK(module->declarations->slots[0].module == module->declarations->root_module);
    for (unsigned n = 0; n < 3; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            if (function->name_length != strlen(names[n]) || memcmp(function->name, names[n], function->name_length)) continue;
            CHECK(entries[n] == UINT32_MAX && !function->parameter_count);
            CHECK((module->declarations->functions[f].module == module->declarations->root_module) == (n != 2));
            entries[n] = f;
        }
        CHECK(entries[n] != UINT32_MAX);
    }
}
static bool task_source_prepare(TaskSourcePrepare *state) {
    XrCompilerSessionStatus created = xr_compile_session_new(state->context->resources, &state->session);
    CHECK(created == XR_COMPILER_SESSION_OK || created == XR_COMPILER_SESSION_BUDGET || created == XR_COMPILER_SESSION_OUT_OF_MEMORY);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (!task_source_stage(state, 2, status)) { CHECK(!state->session); return false; }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_MIXED_TASK_RESULT_FIXTURES};
    XrXirSourceRequest request = {state->session, XR_MIXED_TASK_RESULT_FIXTURES "/root.xr", &authority,
        state->context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceDiagnostic diagnostic = {0};
    status = xr_xir_compile_source_check(&request, &state->source, &diagnostic, NULL);
    if (!task_source_stage(state, 3, status)) { CHECK(!state->source.checked && !state->source.snapshot); return false; }
    CHECK(state->source.checked && state->source.snapshot);
    status = xr_xir_compile_checked_write(state->source.checked, &state->packet, NULL);
    if (!task_source_stage(state, 4, status)) { CHECK(!state->packet.bytes && !state->packet.length); return false; }
    xr_xir_compile_source_result_free(&state->source); xr_compile_session_free(state->session); state->session = NULL;
    status = xr_xir_compile_checked_read(state->context, state->packet.bytes, state->packet.length, &state->checked, NULL);
    memset(state->packet.bytes, 0xA5, state->packet.length); xr_xir_compile_checked_packet_free(&state->packet);
    if (!task_source_stage(state, 5, status)) { CHECK(!state->checked); return false; }
    status = xr_xir_compile_specialize(state->checked, &state->closed, NULL);
    if (!task_source_stage(state, 6, status)) { CHECK(!state->closed); return false; }
    xr_xir_compile_artifact_free(state->checked); state->checked = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(state->closed, &target, NULL, &state->lowered, NULL);
    if (!task_source_stage(state, 7, status)) { CHECK(!state->lowered); return false; }
    xr_xir_compile_artifact_free(state->closed); state->closed = NULL;
    task_source_entries(xr_xir_compile_artifact_module(state->lowered), state->entries);
    status = xr_xir_compile_emit_c(state->lowered, "p20_task_result_fixture", 4194304, &state->generated);
    if (!task_source_stage(state, 8, status)) { CHECK(!state->generated.text && !state->generated.length); return false; }
    CHECK(state->generated.text && !strstr(state->generated.text, "({") && !strstr(state->generated.text, "xr_xir_vm"));
    return true;
}
static void task_source_cleanup(TaskSourcePrepare *state) {
    xr_xir_compile_source_result_free(&state->source); xr_compile_session_free(state->session); state->session = NULL;
    xr_xir_compile_checked_packet_free(&state->packet); xr_xir_compile_artifact_free(state->checked); state->checked = NULL;
    xr_xir_compile_artifact_free(state->closed); state->closed = NULL;
    xr_xir_compile_artifact_free(state->lowered); state->lowered = NULL; xr_xir_compile_c_source_free(&state->generated);
}
#endif // XIR_MIXED_TASK_SOURCE_PREPARE_H
