/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_execution_native_pipeline.h - Owned Source to native root programs
 *
 * KEY CONCEPT:
 *   The source snapshot, packet and intermediate owners die before C emission.
 */
#ifndef XIR_ROOT_EXECUTION_NATIVE_PIPELINE_H
#define XIR_ROOT_EXECUTION_NATIVE_PIPELINE_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"

static const char *const rn_fixture_names[4] = {"identity", "resume", "closing", "init_failure"};
static const char *const rn_entry_names[4][4] = {
    {"next", "current", "handle", "probe"}, {"run", "current", NULL, NULL},
    {"run", "current", NULL, NULL}, {"run", NULL, NULL, NULL}};
static const unsigned rn_entry_counts[4] = {4, 2, 2, 1};

static uint32_t rn_find(const XrXirModule *module, const char *name) {
    uint32_t found = UINT32_MAX;
    size_t length = strlen(name);
    for (uint32_t i = 0; i < module->function_count; ++i) {
        const XrXirFunction *function = &module->functions[i];
        if (module->declarations->functions[i].module == module->declarations->root_module &&
            function->name_length == length && !memcmp(function->name, name, length)) {
            CHECK(found == UINT32_MAX);
            found = i;
        }
    }
    CHECK(found != UINT32_MAX);
    return found;
}

static void rn_check_result(const XrXirTypes *types, XrXirType result, unsigned fixture, unsigned named) {
    if (!fixture && named == 2) {
        const XrXirTypeNode *signature = xr_xir_callable_signature(types, result);
        CHECK(signature && !signature->parameter_count && signature->result == XR_XIR_I64);
    } else CHECK(result == ((fixture == 1 || fixture == 2) && !named ? XR_XIR_STRING : XR_XIR_I64));
}

static void rn_query_boundary(const XrXirSourceResult *result, unsigned fixture) {
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete);
    for (unsigned n = 0; n < rn_entry_counts[fixture]; ++n) {
        unsigned found = 0;
        for (uint32_t i = 0; i < view->declaration_count; ++i) {
            const XrXirSourceDeclaration *declaration = &view->declarations[i];
            if (declaration->kind != XR_XIR_SOURCE_FUNCTION || !declaration->name ||
                strcmp(declaration->name, rn_entry_names[fixture][n])) continue;
            CHECK(declaration->exported && declaration->type.known && !declaration->parameter_count &&
                !declaration->generic_parameter_count);
            rn_check_result(view->types, declaration->type.type, fixture, n);
            fprintf(stderr, "ROOT_NATIVE_INPUT fixture=%s name=%s parameters=0 resultType=%u known=1\n",
                rn_fixture_names[fixture], declaration->name, (unsigned)declaration->type.type);
            ++found;
        }
        CHECK(found == 1);
    }
}

static void rn_failure_order(const XrXirModule *module) {
    const XrXirDeclarations *declarations = module->declarations;
    CHECK(declarations->module_count == 2 && declarations->modules[declarations->root_module].dependency_count == 1);
    uint32_t core = declarations->modules[declarations->root_module].dependencies[0];
    CHECK(core < declarations->module_count && core != declarations->root_module);
    unsigned assertions = 0;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        const XrXirFunction *function = &module->functions[i];
        if (declarations->functions[i].module != core || function->name_length != 6 ||
            memcmp(function->name, "assert", 6)) continue;
        CHECK(function->parameter_count == 2 && function->parameters[0] == XR_XIR_BOOL &&
            function->parameters[1] == XR_XIR_STRING && function->result == XR_XIR_UNIT);
        ++assertions;
    }
    CHECK(assertions == 1);
    fprintf(stderr, "ROOT_NATIVE_CORE module=%u slots=0 assertParameters=BOOL/STRING result=UNIT\n", core);
    CHECK(declarations->slot_count == 2 && declarations->slots[0].type == XR_XIR_STRING &&
        declarations->slots[1].type == XR_XIR_I64 && declarations->slots[0].mutable &&
        declarations->slots[1].mutable && declarations->slots[0].module == declarations->root_module &&
        declarations->slots[1].module == declarations->root_module);
    const XrXirFunction *initializer = &module->functions[declarations->modules[declarations->root_module].initializer];
    uint32_t published = UINT32_MAX, called = UINT32_MAX;
    uint32_t fail = rn_find(module, "fail");
    for (uint32_t i = 0; i < initializer->instruction_count; ++i) {
        const XrXirInstruction *op = &initializer->instructions[i];
        if (op->op == XR_XIR_SLOT_INIT && op->immediate == 0) {
            CHECK(published == UINT32_MAX);
            published = i;
        }
        if (op->op == XR_XIR_CALL && op->immediate == fail) {
            CHECK(called == UINT32_MAX);
            called = i;
        }
    }
    CHECK(published != UINT32_MAX && called != UINT32_MAX && published < called);
    fprintf(stderr, "ROOT_NATIVE_INITIALIZER publishedSlot=0 op=%u failCall=%u root=%u\n",
        published, called, declarations->root_module);
}

static XrXirArtifact *rn_lower_source(const XrXirCompileContext *context, unsigned fixture) {
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/%s/root.xr", XR_ROOT_NATIVE_FIXTURES, rn_fixture_names[fixture]) > 0);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ROOT_NATIVE_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    char *failure_path = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &diagnostic, &failure_path);
    if (status != XR_XIR_OK) fprintf(stderr, "ROOT_NATIVE_SOURCE fixture=%s status=%u %d:%d %s path=%s\n",
        rn_fixture_names[fixture], (unsigned)status, diagnostic.line, diagnostic.column, diagnostic.message,
        failure_path ? failure_path : "none");
    CHECK(status == XR_XIR_OK);
    rn_query_boundary(&source, fixture);
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *replay = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &replay, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_source_result_free(&source);
    xr_compile_session_free(session);
    xr_compile_resources_free(failure_path);
    XrXirDiagnostic checked = {0};
    status = xr_xir_compile_specialize(replay, &closed, &checked);
    if (status != XR_XIR_OK) fprintf(stderr, "ROOT_NATIVE_SPECIALIZE fixture=%s status=%u function=%u block=%u instruction=%u reason=%u\n",
        rn_fixture_names[fixture], (unsigned)status, checked.function, checked.block, checked.instruction, (unsigned)checked.reason);
    CHECK(status == XR_XIR_OK);
    xr_xir_compile_artifact_free(replay);
    CHECK(xr_xir_compile_artifact_verify(closed, &checked) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, &checked) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered, &checked) == XR_XIR_OK);
    return lowered;
}

static void rn_emit_fixture(const XrXirCompileContext *context, unsigned fixture, const char *path, FILE *header) {
    XrXirArtifact *lowered = rn_lower_source(context, fixture);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const XrXirDeclarations *declarations = module->declarations;
    CHECK(module->stage == XR_XIR_LOWERED && module->functions[declarations->entry_function].parameter_count == 0 &&
        module->functions[declarations->entry_function].result == XR_XIR_I64);
    char prefix[64];
    CHECK(snprintf(prefix, sizeof(prefix), "rn_%s", rn_fixture_names[fixture]) > 0);
    CHECK(fprintf(header, "XR_DATA const XrXirProgramSpec %s_program;\nstatic const uint32_t %s_ids[%u]={",
        prefix, prefix, rn_entry_counts[fixture]) > 0);
    for (unsigned i = 0; i < rn_entry_counts[fixture]; ++i) {
        uint32_t entry = rn_find(module, rn_entry_names[fixture][i]);
        CHECK(!module->functions[entry].parameter_count && declarations->functions[entry].exported);
        rn_check_result(module->types, module->functions[entry].result, fixture, i);
        CHECK(fprintf(header, "%s%uu", i ? "," : "", entry) > 0);
    }
    CHECK(fputs("};\n", header) >= 0);
    if (fixture == 3) rn_failure_order(module);
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, prefix, 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && source.length && !source.text[source.length] && !strstr(source.text, "({"));
    CHECK(strstr(source.text, "xr_xir_instance_slot_read(") || fixture == 3);
    if (fixture == 0) CHECK(strstr(source.text, "xr_xir_instance_function("));
    xr_xir_compile_artifact_free(lowered);
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(source.text, 1, source.length, file) == source.length && !fclose(file));
    printf("ROOT_NATIVE_GENERATED fixture=%s SourceCheckedReplaySpecializeVerifyLowered=1 C11bytes=%zu\n",
        rn_fixture_names[fixture], source.length);
    xr_xir_compile_c_source_free(&source);
}
#endif // XIR_ROOT_EXECUTION_NATIVE_PIPELINE_H
