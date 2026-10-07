/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_pipeline.h - One bounded owner for complete capacity programs
 *
 * KEY CONCEPT:
 *   Source, wire replay, specialization, emission and sealing share one ledger.
 */
#ifndef XIR_ARRAY_CAPACITY_PIPELINE_H
#define XIR_ARRAY_CAPACITY_PIPELINE_H
#include "base/xsha256.h"
#ifndef XR_CAPACITY_COMPONENT
#define XR_CAPACITY_COMPONENT "transaction"
#endif
typedef struct CapacityCompile {
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
} CapacityCompile;
#if defined(XR_CAPACITY_NATIVE)
typedef struct CapacityMixed { XrXirProgram *vm; XrXirCallEntry *entries; } CapacityMixed;
static void capacity_mixed_free(void *pointer) {
    CapacityMixed *owner = pointer;
    xr_xir_compile_program_drop(owner->vm);
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner);
}
static XrXirStatus capacity_allocate(const XrXirCompileContext *context, size_t count, size_t bytes, void **out) {
    XrCompileResourceStatus status = xr_compile_resources_calloc(context->resources, count, bytes, out);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
}
#endif
static XrXirStatus capacity_seal(CapacityCompile *run, unsigned mode) {
    if (!mode) return xr_xir_compile_vm_program_take(&run->lowered, &run->program);
#if defined(XR_CAPACITY_NATIVE)
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(proof.length == array_capacity_program.proof.length && !memcmp(proof.identity, array_capacity_program.proof.identity, 32));
    CHECK(!memcmp(proof.bytes, array_capacity_program.proof.bytes, proof.length));
    if (mode == 1) return xr_xir_compile_program_seal(&run->context, &array_capacity_program, &run->program);
    CapacityMixed *owner = NULL;
    XrXirStatus status = capacity_allocate(&run->context, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_XIR_OK) return status;
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    CHECK(module->function_count == array_capacity_program.entry_count);
    status = capacity_allocate(&run->context, module->function_count, sizeof(*owner->entries), (void **)&owner->entries);
    const XrXirArtifact *artifact = run->lowered;
    if (status == XR_XIR_OK) status = xr_xir_compile_vm_program_take(&run->lowered, &owner->vm);
    if (status == XR_XIR_OK) {
        unsigned native = 0, vm = 0;
        CHECK(!run->lowered && owner->vm->context.resources == run->context.resources);
        for (uint32_t f = 0; f < module->function_count; ++f) {
            bool root = module->declarations->functions[f].module == module->declarations->root_module;
            if (root == (mode == 2)) { owner->entries[f] = array_capacity_program.entries[f]; ++native; }
            else { owner->entries[f] = owner->vm->entries[f]; ++vm; }
        }
        CHECK(native && vm);
        XrXirProgramSpec spec = array_capacity_program;
        spec.entries = owner->entries; spec.types = module->types; spec.declarations = module->declarations;
        spec.proof = xr_xir_compile_program_proof(artifact); spec.code = (XrXirCodeLease){owner, capacity_mixed_free};
        status = xr_xir_compile_program_seal(&run->context, &spec, &run->program);
    }
    if (status != XR_XIR_OK) { CHECK(!run->program); capacity_mixed_free(owner); }
    return status;
#else
    (void) mode;
    return XR_XIR_BAD_STRUCTURE;
#endif
}
static XrCompileResourceStats capacity_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == instance_compile_bytes);
    return stats;
}
static bool capacity_compiler_digest;
static void capacity_digest(const void *bytes, size_t length, char text[65]) {
    uint8_t digest[32]; xr_sha256(bytes, length, digest);
    for (unsigned i = 0; i < 32; ++i) CHECK(snprintf(text + 2 * i, 3, "%02x", digest[i]) == 2);
}
static bool capacity_profile;
static clock_t capacity_profile_clock;
static void capacity_profile_stage(CapacityCompile *run, const char *stage) {
    if (!capacity_profile) return;
    clock_t now = clock();
    XrCompileResourceStats stats = capacity_stats(&run->context);
    printf("compiler profile %s seconds%.6f cumulative-sites%zu allocated%llu live%llu peak%llu work%llu\n",
        stage, (double)(now - capacity_profile_clock) / CLOCKS_PER_SEC, instance_compile_attempts,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.live_bytes,
        (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    fflush(stdout); capacity_profile_clock = now;
}
static CapacityCompile capacity_build(size_t failure, XrCompileResourceLimits limits, unsigned mode, const char *emit) {
    instance_compile_zero(); instance_compile_attempts = 0; instance_compile_fail_at = failure; instance_compile_injected = false;
    CapacityCompile run = {0}; run.status = XR_XIR_OUT_OF_MEMORY;
    run.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &run.context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK) { CHECK(!run.context.resources); run.sites = instance_compile_attempts; return run; }
    run.baseline = capacity_stats(&run.context);
    capacity_profile_stage(&run, "resource-new");
    XrCompilerSession *session = NULL; XrXirSourceResult result = {0};
    XrXirArtifact *read = NULL, *specialized = NULL;
    XrXirSourceDiagnostic diagnostic = {0}; XrXirCheckedPacket bytes = {0}; XrXirCSource source = {0}; char *path = NULL;
    XrCompilerSessionStatus session_status = xr_compile_session_new(run.context.resources, &session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        run.status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_CAPACITY_FIXTURES};
    XrXirSourceRequest request = {session, XR_CAPACITY_FIXTURES "/" XR_CAPACITY_COMPONENT "/root.xr", &authority,
        &run.context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    capacity_profile_stage(&run, "session-new");
    run.stage = 1; run.status = xr_xir_compile_source_check(&request, &result, &diagnostic, &path);
    if (run.status != XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);
        if (run.status != XR_XIR_OUT_OF_MEMORY && run.status != XR_XIR_BUDGET)
            fprintf(stderr, "Source status%u line%d:%d %s\n", run.status, diagnostic.line, diagnostic.column, diagnostic.message);
        goto done;
    }
    CHECK(xr_xir_compile_artifact_context(result.checked)->resources == run.context.resources);
    capacity_profile_stage(&run, "source-check");
    run.stage = 2; run.status = xr_xir_compile_checked_write(result.checked, &bytes, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!bytes.bytes && !bytes.length); goto done; }
    capacity_profile_stage(&run, "checked-write");
    run.stage = 3; run.status = xr_xir_compile_checked_read(&run.context, bytes.bytes, bytes.length, &read, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!read); goto done; }
    capacity_profile_stage(&run, "checked-read");
    xr_xir_compile_checked_packet_free(&bytes); xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); session = NULL;
    capacity_profile_stage(&run, "source-session-drop");
    run.stage = 4; run.status = xr_xir_compile_specialize(read, &specialized, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!specialized); goto done; }
    capacity_profile_stage(&run, "specialize");
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run.stage = 5; run.status = xr_xir_compile_lower(specialized, &target, &run.lowered, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!run.lowered); goto done; }
    CHECK(xr_xir_compile_artifact_context(run.lowered)->resources == run.context.resources);
    capacity_profile_stage(&run, "lower");
    run.stage = 6; run.status = xr_xir_compile_emit_c(run.lowered, "array_capacity", 1048576, &source);
    if (run.status != XR_XIR_OK) { CHECK(!source.text && !source.length); goto done; }
    run.prefix_sites = instance_compile_attempts; run.prefix_stats = capacity_stats(&run.context);
    run.prefix_references = run.context.resources->references;
    run.prefix_blocks = instance_compile_live; run.prefix_bytes = instance_compile_bytes;
    if (capacity_compiler_digest) {
        XrXirProgramProof proof = xr_xir_compile_program_proof(run.lowered);
        capacity_digest(source.text, source.length, run.c_sha); capacity_digest(proof.bytes, proof.length, run.proof_sha);
    }
    run.c_bytes = source.length;
    capacity_profile_stage(&run, "emit");
    if (emit) {
        FILE *file = fopen(emit, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length && fclose(file) == 0);
        printf("Array capacity C bytes=%zu cap1048576\n", source.length);
    } else { run.module = xr_xir_compile_artifact_module(run.lowered); run.stage = 7; run.status = capacity_seal(&run, mode); }
    capacity_profile_stage(&run, "seal-or-file");
done:
    if (run.status != XR_XIR_OK) {
        CHECK(!run.program);
        xr_xir_compile_artifact_free(run.lowered); run.lowered = NULL;
    }
    xr_xir_compile_c_source_free(&source); xr_xir_compile_checked_packet_free(&bytes);
    xr_xir_compile_artifact_free(specialized); xr_xir_compile_artifact_free(read);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); xr_compile_resources_free(path);
    capacity_profile_stage(&run, "temporary-drop");
    run.sites = instance_compile_attempts; instance_compile_fail_at = SIZE_MAX;
    run.stats = capacity_stats(&run.context);
    return run;
}
static void capacity_release(CapacityCompile *run) {
    xr_xir_compile_artifact_free(run->lowered); xr_xir_compile_program_drop(run->program);
    if (run->context.resources) {
        CHECK(capacity_stats(&run->context).live_bytes == run->baseline.live_bytes);
        xr_compile_resources_release(run->context.resources);
    }
    *run = (CapacityCompile){0}; instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
}
static XrCompileResourceLimits capacity_limits(void) {
    return (XrCompileResourceLimits){67108864, 8388608, 128000000};
}
#endif // XIR_ARRAY_CAPACITY_PIPELINE_H
