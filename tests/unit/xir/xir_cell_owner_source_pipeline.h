/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_owner_source_pipeline.h - Real Source stages and exported entry lookup
 */
#ifndef XIR_CELL_OWNER_SOURCE_PIPELINE_H
#define XIR_CELL_OWNER_SOURCE_PIPELINE_H
typedef struct CellSourceEntries { uint32_t run, counter, factory; } CellSourceEntries;

static XrXirDiagnostic cell_source_diagnostic(void) {
    return (XrXirDiagnostic){XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
}
static void cell_source_stage(const char *name, const char *stage, XrXirStatus status,
    const XrXirDiagnostic *diagnostic) {
    if (status == XR_XIR_OK) return;
    if (diagnostic)
        fprintf(stderr, "%s stage=%s status=%u diagnostic_status=%u function=%u block=%u instruction=%u reason=%u\n",
            name, stage, (unsigned)status, (unsigned)diagnostic->status, diagnostic->function,
            diagnostic->block, diagnostic->instruction, (unsigned)diagnostic->reason);
    else fprintf(stderr, "%s stage=%s status=%u diagnostic=unavailable\n", name, stage, (unsigned)status);
}
static void cell_source_unit_query(const XrXirSourceView *view) {
    unsigned found = 0;
    CHECK(view && view->complete);
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *decl = &view->declarations[d];
        if (decl->kind != XR_XIR_SOURCE_BINDING || strcmp(decl->name, "stamp")) continue;
        CHECK(decl->mutable && decl->type.known && decl->type.type == XR_XIR_UNIT);
        ++found;
    }
    CHECK(found == 1);
}
static void cell_source_indirect_query(const XrXirSourceView *view, const char *name) {
    if (strncmp(name,"indirect_ref_",13)) return;
    unsigned values = 0, callables = 0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *declaration = &view->declarations[d];
        if (declaration->kind != XR_XIR_SOURCE_PARAMETER || !declaration->type.known) continue;
        if (!strcmp(declaration->name,"value")) {
            CHECK(!xr_xir_type_is_cell(view->types,declaration->type.type));
            ++values;
        }
        const XrXirTypeNode *signature = xr_xir_callable_signature(view->types,declaration->type.type);
        if (!signature) continue;
        for (uint32_t p=0;p<signature->parameter_count;++p) if (signature->parameters[p].mode == XR_PARAM_REF) {
            CHECK(xr_xir_type_is_cell(view->types,signature->parameters[p].type));
            ++callables;
        }
    }
    CHECK(values);
    if (!strcmp(name,"indirect_ref_forward.xr") || !strcmp(name,"indirect_ref_local.xr") ||
        !strcmp(name,"indirect_ref_unit.xr") || !strcmp(name,"indirect_ref_generic.xr")) CHECK(callables);
}
static XrXirStatus cell_source_lower(const XrXirCompileContext *context, const char *name,
    XrXirArtifact **output, bool unit) {
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s", XR_CELL_OWNER_FIXTURES, name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_CELL_OWNER_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *decoded = NULL, *closed = NULL;
    XrXirDiagnostic stage_diagnostic = cell_source_diagnostic();
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status == XR_XIR_OK) {
        CHECK(result.checked && result.snapshot);
        cell_source_indirect_query(xr_xir_compile_source_snapshot_view(result.snapshot),name);
        if (unit) cell_source_unit_query(xr_xir_compile_source_snapshot_view(result.snapshot));
        status = xr_xir_compile_checked_write(result.checked, &packet, &stage_diagnostic);
        cell_source_stage(name, "checked_write", status, &stage_diagnostic);
    } else {
        CHECK(!result.checked && !result.snapshot);
        fprintf(stderr, "%s Source rejected: %u at %u:%d:%d %s\n", name, status,
            diagnostic.module, diagnostic.line, diagnostic.column, diagnostic.message);
    }
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    if (status == XR_XIR_OK) {
        stage_diagnostic = cell_source_diagnostic();
        status = xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &stage_diagnostic);
        cell_source_stage(name, "checked_read", status, &stage_diagnostic);
    }
    if (packet.bytes) memset(packet.bytes, 0xCC, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    if (status == XR_XIR_OK) {
        stage_diagnostic = cell_source_diagnostic();
        status = xr_xir_compile_specialize(decoded, &closed, &stage_diagnostic);
        cell_source_stage(name, "specialize", status, &stage_diagnostic);
    }
    xr_xir_compile_artifact_free(decoded);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    if (status == XR_XIR_OK) {
        stage_diagnostic = cell_source_diagnostic();
        status = xr_xir_compile_lower(closed, &target, output, &stage_diagnostic);
        cell_source_stage(name, "lower", status, &stage_diagnostic);
    }
    xr_xir_compile_artifact_free(closed);
    if (status != XR_XIR_OK) CHECK(!*output);
    return status;
}
static uint32_t cell_source_entry(const XrXirModule *module, const char *name, bool required) {
    uint32_t entry = UINT32_MAX;
    size_t length = strlen(name);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != length || memcmp(function->name, name, length)) continue;
        CHECK(entry == UINT32_MAX && module->declarations->functions[f].exported);
        entry = f;
    }
    CHECK(!required || entry != UINT32_MAX);
    return entry;
}
static void cell_source_unit_storage(const XrXirModule *module) {
    unsigned found = 0;
    for (uint32_t s = 0; s < module->declarations->slot_count; ++s) {
        const XrXirSlot *slot = &module->declarations->slots[s];
        if (!slot->mutable || !xr_xir_type_is_cell(module->types, slot->type)) continue;
        if (xr_xir_cell_element(module->types, slot->type) == XR_XIR_UNIT) ++found;
    }
    CHECK(found == 1);
}
static XrXirStatus cell_source_program(const XrXirCompileContext *context, const char *name,
    XrXirProgram **output, CellSourceEntries *entries, bool unit) {
    XrXirArtifact *lowered = NULL;
    XrXirStatus status = cell_source_lower(context, name, &lowered, unit);
    if (status == XR_XIR_OK) {
        const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
        CHECK(module && module->declarations && module->declarations->functions);
        entries->run = cell_source_entry(module, "run", true);
        entries->counter = cell_source_entry(module, "cellCounter", false);
        entries->factory = cell_source_entry(module, "cellFactory", false);
        if (unit) cell_source_unit_storage(module);
        status = xr_xir_compile_vm_program_take(&lowered, output);
        cell_source_stage(name, "vm_program_take", status, NULL);
        if (status == XR_XIR_OK) CHECK(!lowered && *output);
    }
    xr_xir_compile_artifact_free(lowered);
    if (status != XR_XIR_OK) CHECK(!*output);
    return status;
}
#endif // XIR_CELL_OWNER_SOURCE_PIPELINE_H
