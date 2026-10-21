/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_concat_pipeline.h - One owner for complete concat programs
 *
 * KEY CONCEPT:
 *   Source, wire replay, specialization, emission and sealing share one ledger.
 */
#ifndef XIR_ARRAY_CONCAT_PIPELINE_H
#define XIR_ARRAY_CONCAT_PIPELINE_H
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_types.h"
#include "base/xsha256.h"
#include "base/xresource_limits.h"
#ifndef XR_CONCAT_COMPONENT
#define XR_CONCAT_COMPONENT "transaction"
#endif
typedef struct ConcatCompile {
    XrXirCompileContext context;
    XrCompileResourceStats baseline, stats;
    XrXirArtifact *lowered;
    const XrXirModule *module;
    XrXirProgram *program;
    XrXirStatus status;
    unsigned stage;
    size_t sites, c_bytes, prefix_sites;
    XrCompileResourceStats prefix_stats;
    uint64_t prefix_references;
    size_t prefix_blocks, prefix_bytes;
    char c_sha[65], proof_sha[65];
} ConcatCompile;
#if defined(XR_CONCAT_NATIVE)
typedef struct ConcatMixed { XrXirProgram *vm; XrXirCallEntry *entries; } ConcatMixed;
static void concat_mixed_free(void *pointer) {
    ConcatMixed *owner = pointer;
    xr_xir_compile_program_drop(owner->vm);
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner);
}
static XrXirStatus concat_allocate(const XrXirCompileContext *context, size_t count, size_t bytes, void **out) {
    XrCompileResourceStatus status = xr_compile_resources_calloc(context->resources, count, bytes, out);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
}
#endif
static XrXirStatus concat_seal(ConcatCompile *run, unsigned mode) {
    if (!mode) return xr_xir_compile_vm_program_take(&run->lowered, &run->program);
#if defined(XR_CONCAT_NATIVE)
    XrXirProgramPacket proof = xir_program_packet_borrow(run->lowered);
    CHECK(proof.length == array_concat_program.packet.length && !memcmp(proof.identity, array_concat_program.packet.identity, 32));
    CHECK(!memcmp(proof.bytes, array_concat_program.packet.bytes, proof.length));
    if (mode == 1) return xr_xir_compile_program_seal(&run->context, &array_concat_program, &run->program);
    ConcatMixed *owner = NULL;
    XrXirStatus status = concat_allocate(&run->context, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_XIR_OK) return status;
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    CHECK(module->function_count == array_concat_program.entry_count);
    status = concat_allocate(&run->context, module->function_count, sizeof(*owner->entries), (void **)&owner->entries);
    const XrXirArtifact *artifact = run->lowered;
    if (status == XR_XIR_OK) status = xr_xir_compile_vm_program_take(&run->lowered, &owner->vm);
    if (status == XR_XIR_OK) {
        unsigned native = 0, vm = 0;
        CHECK(!run->lowered && owner->vm->context.resources == run->context.resources);
        for (uint32_t f = 0; f < module->function_count; ++f) {
            bool root = module->declarations->functions[f].module == module->declarations->root_module;
            if (root == (mode == 2)) { owner->entries[f] = array_concat_program.entries[f]; ++native; }
            else { owner->entries[f] = owner->vm->entries[f]; ++vm; }
        }
        CHECK(native && vm);
        XrXirProgramSpec spec = array_concat_program;
        spec.entries = owner->entries; spec.types = module->types; spec.declarations = module->declarations;
        spec.packet = xir_program_packet_borrow(artifact); spec.code = (XrXirCodeLease){owner, concat_mixed_free};
        status = xr_xir_compile_program_seal(&run->context, &spec, &run->program);
    }
    if (status != XR_XIR_OK) { CHECK(!run->program); concat_mixed_free(owner); }
    return status;
#else
    (void) mode;
    return XR_XIR_BAD_STRUCTURE;
#endif
}
static XrCompileResourceStats concat_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == instance_compile_bytes);
    return stats;
}
static bool concat_compiler_digest;
static void concat_digest(const void *bytes, size_t length, char text[65]) {
    uint8_t digest[32]; xr_sha256(bytes, length, digest);
    for (unsigned i = 0; i < 32; ++i) CHECK(snprintf(text + 2 * i, 3, "%02x", digest[i]) == 2);
}
static bool concat_profile;
static clock_t concat_profile_clock;
static void concat_profile_stage(ConcatCompile *run, const char *stage) {
    if (!concat_profile) return;
    clock_t now = clock();
    XrCompileResourceStats stats = concat_stats(&run->context);
    printf("compiler profile %s seconds%.6f cumulative-sites%zu allocated%llu live%llu peak%llu work%llu\n",
        stage, (double)(now - concat_profile_clock) / CLOCKS_PER_SEC, instance_compile_attempts,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.live_bytes,
        (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    fflush(stdout); concat_profile_clock = now;
}
static ConcatCompile concat_build(size_t failure, XrCompileResourceLimits limits, unsigned mode, const char *emit) {
    instance_compile_zero(); instance_compile_attempts = 0; instance_compile_fail_at = failure; instance_compile_injected = false;
    ConcatCompile run = {0}; run.status = XR_XIR_OUT_OF_MEMORY;
    run.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &run.context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK) { CHECK(!run.context.resources); run.sites = instance_compile_attempts; return run; }
    run.baseline = concat_stats(&run.context);
    concat_profile_stage(&run, "resource-new");
    XrCompilerSession *session = NULL; XrXirSourceResult result = {0};
    XrXirArtifact *read = NULL, *specialized = NULL;
    XrXirSourceDiagnostic diagnostic = {0}; XrXirCheckedPacket bytes = {0}; XrXirCSource source = {0}; XrXirSourceFailure path={0};
    XrCompilerSessionStatus session_status = xr_compile_session_new(run.context.resources, &session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        run.status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_CONCAT_FIXTURES};
    XrXirSourceRequest request = {session, XR_CONCAT_FIXTURES "/" XR_CONCAT_COMPONENT "/root.xr", &authority,
        &run.context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    concat_profile_stage(&run, "session-new");
    run.stage = 1; run.status = xr_xir_compile_source_check(&request, &result, &diagnostic, &path);
    if (run.status != XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);
        if (run.status != XR_XIR_OUT_OF_MEMORY && run.status != XR_XIR_BUDGET)
            fprintf(stderr, "Source status%u line%d:%d %s\n", run.status, diagnostic.line, diagnostic.column, diagnostic.message);
        goto done;
    }
    CHECK(xr_xir_compile_artifact_context(result.checked)->resources == run.context.resources);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    unsigned concat_members = 0;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *declaration = &view->declarations[d];
        if (declaration->native_identity == 14) {
            CHECK(!declaration->mutable && declaration->parameter_count == 1 && declaration->exported);
            CHECK(!strcmp(declaration->name, "concat") &&
                !strcmp(declaration->signature, "(...arrays: Array<T>) -> Array<T>"));
            CHECK(declaration->kind == XR_XIR_SOURCE_MEMBER && declaration->parent &&
                declaration->parent <= view->declaration_count);
            const XrXirSourceDeclaration *array = &view->declarations[declaration->parent - 1];
            CHECK(array->kind == XR_XIR_SOURCE_TYPE && !strcmp(array->name, "Array") &&
                array->generic_parameter_count == 1);
            CHECK(declaration->type.known && declaration->type.generic_owner == array->id);
            const XrXirTypeNode *schema = xr_xir_type_node(view->types, declaration->type.type);
            CHECK(schema && schema->kind == XR_XIR_TYPE_ARRAY &&
                schema->element == (XrXirType) XR_XIR_TYPE_PARAMETER_BASE);
            CHECK(declaration->parameters && declaration->parameters[0].known &&
                declaration->parameters[0].generic_owner == array->id &&
                declaration->parameters[0].type == declaration->type.type);
            ++concat_members;
        }
    }
    CHECK(concat_members == 1);
    concat_profile_stage(&run, "source-check");
    run.stage = 2; run.status = xr_xir_compile_checked_write(result.checked, &bytes, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!bytes.bytes && !bytes.length); goto done; }
    concat_profile_stage(&run, "checked-write");
    run.stage = 3; run.status = xr_xir_compile_checked_read(&run.context, bytes.bytes, bytes.length, &read, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!read); goto done; }
    concat_profile_stage(&run, "checked-read");
    xr_xir_compile_checked_packet_free(&bytes); xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); session = NULL;
    concat_profile_stage(&run, "source-session-drop");
    run.stage = 4; run.status = xr_xir_compile_specialize(read, &specialized, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!specialized); goto done; }
    concat_profile_stage(&run, "specialize");
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run.stage = 5; run.status = xr_xir_compile_lower(specialized, &target, &run.lowered, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!run.lowered); goto done; }
    CHECK(xr_xir_compile_artifact_context(run.lowered)->resources == run.context.resources);
    concat_profile_stage(&run, "lower");
    run.stage = 6; run.status = xr_xir_compile_emit_c(run.lowered, "array_concat", 1048576, &source);
    if (run.status != XR_XIR_OK) { CHECK(!source.text && !source.length); goto done; }
    run.prefix_sites = instance_compile_attempts; run.prefix_stats = concat_stats(&run.context);
    run.prefix_references = run.context.resources->references;
    run.prefix_blocks = instance_compile_live; run.prefix_bytes = instance_compile_bytes;
    if (concat_compiler_digest) {
        XrXirProgramPacket proof = xir_program_packet_borrow(run.lowered);
        concat_digest(source.text, source.length, run.c_sha); concat_digest(proof.bytes, proof.length, run.proof_sha);
    }
    run.c_bytes = source.length;
    concat_profile_stage(&run, "emit");
    if (emit) {
        FILE *file = fopen(emit, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length && fclose(file) == 0);
        printf("Array concat C bytes=%zu cap1048676\n", source.length);
    } else { run.module = xr_xir_compile_artifact_module(run.lowered); run.stage = 7; run.status = concat_seal(&run, mode); }
    concat_profile_stage(&run, "seal-or-file");
done:
    if (run.status != XR_XIR_OK) {
        CHECK(!run.program);
        xr_xir_compile_artifact_free(run.lowered); run.lowered = NULL;
    }
    xr_xir_compile_c_source_free(&source); xr_xir_compile_checked_packet_free(&bytes);
    xr_xir_compile_artifact_free(specialized); xr_xir_compile_artifact_free(read);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); xr_xir_compile_source_failure_free(&path);
    concat_profile_stage(&run, "temporary-drop");
    run.sites = instance_compile_attempts; instance_compile_fail_at = SIZE_MAX;
    run.stats = concat_stats(&run.context);
    return run;
}
static void concat_release(ConcatCompile *run) {
    xr_xir_compile_artifact_free(run->lowered); xr_xir_compile_program_drop(run->program);
    if (run->context.resources) {
        CHECK(concat_stats(&run->context).live_bytes == run->baseline.live_bytes);
        xr_compile_resources_release(run->context.resources);
    }
    *run = (ConcatCompile){0}; instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
}
static XrCompileResourceLimits concat_limits(void) {
    return (XrCompileResourceLimits)XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
}
#endif // XIR_ARRAY_CONCAT_PIPELINE_H
