/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_remove_pipeline.h - One bounded owner for complete removal programs
 *
 * KEY CONCEPT:
 *   Source, wire replay, specialization, emission and sealing share one ledger.
 */
#ifndef XIR_ARRAY_REMOVE_PIPELINE_H
#define XIR_ARRAY_REMOVE_PIPELINE_H
#ifndef XR_REMOVE_COMPONENT
#define XR_REMOVE_COMPONENT "transaction"
#endif
typedef struct RemoveCompile {
    XrXirCompileContext context;
    XrCompileResourceStats baseline, stats;
    XrXirArtifact *lowered;
    const XrXirModule *module;
    XrXirProgram *program;
    XrXirStatus status;
    unsigned stage;
    size_t sites, c_bytes;
} RemoveCompile;
#if defined(XR_REMOVE_NATIVE)
typedef struct RemoveMixed { XrXirProgram *vm; XrXirCallEntry *entries; } RemoveMixed;
static void remove_mixed_free(void *pointer) {
    RemoveMixed *owner = pointer;
    xr_xir_compile_program_drop(owner->vm);
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner);
}
static XrXirStatus remove_allocate(const XrXirCompileContext *context, size_t count, size_t bytes, void **out) {
    XrCompileResourceStatus status = xr_compile_resources_calloc(context->resources, count, bytes, out);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
}
#endif
static XrXirStatus remove_seal(RemoveCompile *run, unsigned mode) {
    if (!mode) return xr_xir_compile_vm_program_take(&run->lowered, &run->program);
#if defined(XR_REMOVE_NATIVE)
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(proof.length == array_remove_program.proof.length && !memcmp(proof.identity, array_remove_program.proof.identity, 32));
    CHECK(!memcmp(proof.bytes, array_remove_program.proof.bytes, proof.length));
    if (mode == 1) return xr_xir_compile_program_seal(&run->context, &array_remove_program, &run->program);
    RemoveMixed *owner = NULL;
    XrXirStatus status = remove_allocate(&run->context, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_XIR_OK) return status;
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    CHECK(module->function_count == array_remove_program.entry_count);
    status = remove_allocate(&run->context, module->function_count, sizeof(*owner->entries), (void **)&owner->entries);
    const XrXirArtifact *artifact = run->lowered;
    if (status == XR_XIR_OK) status = xr_xir_compile_vm_program_take(&run->lowered, &owner->vm);
    if (status == XR_XIR_OK) {
        unsigned native = 0, vm = 0;
        CHECK(!run->lowered && owner->vm->context.resources == run->context.resources);
        for (uint32_t f = 0; f < module->function_count; ++f) {
            bool root = module->declarations->functions[f].module == module->declarations->root_module;
            if (root == (mode == 2)) { owner->entries[f] = array_remove_program.entries[f]; ++native; }
            else { owner->entries[f] = owner->vm->entries[f]; ++vm; }
        }
        CHECK(native && vm);
        XrXirProgramSpec spec = array_remove_program;
        spec.entries = owner->entries; spec.types = module->types; spec.declarations = module->declarations;
        spec.proof = xr_xir_compile_program_proof(artifact); spec.code = (XrXirCodeLease){owner, remove_mixed_free};
        status = xr_xir_compile_program_seal(&run->context, &spec, &run->program);
    }
    if (status != XR_XIR_OK) { CHECK(!run->program); remove_mixed_free(owner); }
    return status;
#else
    (void) mode;
    return XR_XIR_BAD_STRUCTURE;
#endif
}
static XrCompileResourceStats remove_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == instance_compile_bytes);
    return stats;
}
#if defined(XR_REMOVE_MATRIX) && XR_REMOVE_MATRIX == 7
static void remove_shadow_query(const XrXirSourceResult *result, const XrXirSourceView *view) {
    CHECK(view && view->complete && view->diagnostic.status == XR_XIR_OK);
    const XrXirSourceDeclaration *owner = NULL, *constructor = NULL, *pop = NULL, *shift = NULL;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *decl = &view->declarations[d];
        if (decl->kind == XR_XIR_SOURCE_TYPE && !strcmp(decl->name, "Array") && !decl->native_identity) {
            CHECK(!owner); owner = decl;
        }
    }
    CHECK(owner && owner->type.known && owner->generic_parameter_count == 1);
    const XrXirTypeNode *type = xr_xir_type_node(view->types, owner->type.type);
    CHECK(type && type->kind == XR_XIR_TYPE_NOMINAL && type->nominal.argument_count == 1);
    CHECK(view->types->nominals->declarations[type->nominal.declaration].kind == XR_XIR_NOMINAL_CLASS);
    CHECK(!xr_xir_type_is_array(view->types, owner->type.type));
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *decl = &view->declarations[d];
        if (decl->parent != owner->id || decl->kind != XR_XIR_SOURCE_FUNCTION) continue;
        CHECK(!decl->native_identity);
        if (!strcmp(decl->name, "constructor")) { CHECK(!constructor); constructor = decl; }
        if (!strcmp(decl->name, "pop")) { CHECK(!pop); pop = decl; }
        if (!strcmp(decl->name, "shift")) { CHECK(!shift); shift = decl; }
    }
    CHECK(constructor && constructor->parameter_count == 1 && pop && shift);
    CHECK(!pop->mutable && !shift->mutable && pop->parameter_count == 1 && shift->parameter_count == 1);
    unsigned constructors = 0, pops = 0, shifts = 0;
    for (uint32_t r = 0; r < view->reference_count; ++r) {
        const XrXirSourceReference *ref = &view->references[r];
        if (ref->access != XR_XIR_SOURCE_CALL) continue;
        if (ref->target == constructor->id) { CHECK(ref->declaration == owner->id); ++constructors; }
        pops += ref->target == pop->id; shifts += ref->target == shift->id;
    }
    CHECK(constructors == 2 && pops == 2 && shifts == 2);
    const XrXirModule *module = xr_xir_compile_artifact_module(result->checked);
    unsigned bodies = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        if (identity->nominal_owner != type->nominal.declaration + 1 || identity->method_kind != XR_XIR_CONSTRUCTOR) continue;
        CHECK(identity->promises & XR_XIR_FUNCTION_NO_SUSPEND);
        unsigned created = 0;
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i)
            created += module->functions[f].instructions[i].op == XR_XIR_CLASS_NEW;
        CHECK(created == 1); ++bodies;
    }
    CHECK(bodies == 1);
    printf("shadow class query ordinary Array<T> constructors2 pop2 shift2 class-new1\n");
}
#endif
static bool remove_profile;
static clock_t remove_profile_clock;
static void remove_profile_stage(RemoveCompile *run, const char *stage) {
    if (!remove_profile) return;
    clock_t now = clock();
    XrCompileResourceStats stats = remove_stats(&run->context);
    printf("compiler profile %s seconds%.6f cumulative-sites%zu allocated%llu live%llu peak%llu work%llu\n",
        stage, (double)(now - remove_profile_clock) / CLOCKS_PER_SEC, instance_compile_attempts,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.live_bytes,
        (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    fflush(stdout); remove_profile_clock = now;
}
static RemoveCompile remove_build(size_t failure, XrCompileResourceLimits limits, unsigned mode, const char *emit) {
    instance_compile_zero(); instance_compile_attempts = 0; instance_compile_fail_at = failure; instance_compile_injected = false;
    RemoveCompile run = {0}; run.status = XR_XIR_OUT_OF_MEMORY;
    run.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &run.context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK) { CHECK(!run.context.resources); run.sites = instance_compile_attempts; return run; }
    run.baseline = remove_stats(&run.context);
    remove_profile_stage(&run, "resource-new");
    XrCompilerSession *session = NULL; XrXirSourceResult result = {0};
    XrXirArtifact *read = NULL, *specialized = NULL;
    XrXirSourceDiagnostic diagnostic = {0}; XrXirCheckedPacket bytes = {0}; XrXirCSource source = {0}; char *path = NULL;
    XrCompilerSessionStatus session_status = xr_compile_session_new(run.context.resources, &session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        run.status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_REMOVE_FIXTURES};
    XrXirSourceRequest request = {session, XR_REMOVE_FIXTURES "/" XR_REMOVE_COMPONENT "/root.xr", &authority,
        &run.context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    remove_profile_stage(&run, "session-new");
    run.stage = 1; run.status = xr_xir_compile_source_check(&request, &result, &diagnostic, &path);
    if (run.status != XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);
        if (run.status != XR_XIR_OUT_OF_MEMORY && run.status != XR_XIR_BUDGET)
            fprintf(stderr, "Source status%u line%d:%d %s\n", run.status, diagnostic.line, diagnostic.column, diagnostic.message);
        goto done;
    }
    CHECK(xr_xir_compile_artifact_context(result.checked)->resources == run.context.resources);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    unsigned removal_members = 0;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *declaration = &view->declarations[d];
        if (declaration->native_identity == 8 || declaration->native_identity == 9) {
            CHECK(declaration->mutable && declaration->parameter_count == 0 && declaration->exported);
            CHECK(!declaration->type.known && !strcmp(declaration->signature, "() -> T?")); ++removal_members;
        }
    }
    CHECK(removal_members == 2);
#if defined(XR_REMOVE_MATRIX) && XR_REMOVE_MATRIX == 7
    remove_shadow_query(&result, view);
#endif

    remove_profile_stage(&run, "source-check");
    run.stage = 2; run.status = xr_xir_compile_checked_write(result.checked, &bytes, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!bytes.bytes && !bytes.length); goto done; }
    remove_profile_stage(&run, "checked-write");
    run.stage = 3; run.status = xr_xir_compile_checked_read(&run.context, bytes.bytes, bytes.length, &read, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!read); goto done; }
    remove_profile_stage(&run, "checked-read");
    xr_xir_compile_checked_packet_free(&bytes); xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); session = NULL;
    remove_profile_stage(&run, "source-session-drop");
    run.stage = 4; run.status = xr_xir_compile_specialize(read, &specialized, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!specialized); goto done; }
    remove_profile_stage(&run, "specialize");
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run.stage = 5; run.status = xr_xir_compile_lower(specialized, &target, &run.lowered, NULL);
    if (run.status != XR_XIR_OK) { CHECK(!run.lowered); goto done; }
    CHECK(xr_xir_compile_artifact_context(run.lowered)->resources == run.context.resources);
    remove_profile_stage(&run, "lower");
    run.stage = 6; run.status = xr_xir_compile_emit_c(run.lowered, "array_remove", 1048576, &source);
    if (run.status != XR_XIR_OK) { CHECK(!source.text && !source.length); goto done; }
    run.c_bytes = source.length;
    remove_profile_stage(&run, "emit");
    if (emit) {
        FILE *file = fopen(emit, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length && fclose(file) == 0);
        printf("Array removal C bytes=%zu cap1048576\n", source.length);
    } else { run.module = xr_xir_compile_artifact_module(run.lowered); run.stage = 7; run.status = remove_seal(&run, mode); }
    remove_profile_stage(&run, "seal-or-file");
done:
    if (run.status != XR_XIR_OK) {
        CHECK(!run.program);
        xr_xir_compile_artifact_free(run.lowered); run.lowered = NULL;
    }
    xr_xir_compile_c_source_free(&source); xr_xir_compile_checked_packet_free(&bytes);
    xr_xir_compile_artifact_free(specialized); xr_xir_compile_artifact_free(read);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); xr_compile_resources_free(path);
    remove_profile_stage(&run, "temporary-drop");
    run.sites = instance_compile_attempts; instance_compile_fail_at = SIZE_MAX;
    run.stats = remove_stats(&run.context);
    return run;
}
static void remove_release(RemoveCompile *run) {
    xr_xir_compile_artifact_free(run->lowered); xr_xir_compile_program_drop(run->program);
    if (run->context.resources) {
        CHECK(remove_stats(&run->context).live_bytes == run->baseline.live_bytes);
        xr_compile_resources_release(run->context.resources);
    }
    *run = (RemoveCompile){0}; instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
}
static XrCompileResourceLimits remove_limits(void) {
    return (XrCompileResourceLimits){67108864, 8388608, 128000000};
}
#endif // XIR_ARRAY_REMOVE_PIPELINE_H
