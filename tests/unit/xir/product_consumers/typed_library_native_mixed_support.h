/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_native_mixed_support.h - Authentic material and transparent Library dispatch
 *
 * KEY CONCEPT:
 *   One real generated C image and full Closed proof select actual native and VM
 *   delegates. Observers preserve each admitted view and return through the product.
 */
#ifndef TYPED_LIBRARY_CURRENT_NATIVE_MIXED_SUPPORT_H
#define TYPED_LIBRARY_CURRENT_NATIVE_MIXED_SUPPORT_H
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER)
static void emit_native_material(ReceiverRun *run, char **argv) {
    CHECK(completed(run, xr_xir_compile_emit_c(run->lowered, "source_typed_library_current_native",
        16777216, &run->emitted), "writer-real-Lowered-C11-emission"));
    CHECK(run->emitted.text && run->emitted.length);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(proof.bytes && proof.identity && proof.layouts && proof.length == run->expected_closed.length);
    CHECK(!memcmp(proof.bytes, run->expected_closed.bytes, proof.length));
    uint8_t digest[32]; char hex[65], binding[256];
    xr_sha256((const uint8_t *)run->emitted.text, run->emitted.length, digest);
    for (unsigned i = 0; i < 32; ++i) CHECK(snprintf(hex + i * 2, 3, "%02x", digest[i]) == 2);
    int length = snprintf(binding, sizeof(binding), "const char source_typed_library_current_native_c_sha256[65] = \"%s\";\n", hex);
    CHECK(length > 0 && (size_t)length < sizeof(binding));
    for (unsigned i = 4; i < 8; ++i) {
        CHECK(strlen(argv[i]) < sizeof(run->material_paths[i - 4]));
        for (unsigned j = 4; j < i; ++j) CHECK(strcmp(argv[i], argv[j]));
        strcpy(run->material_paths[i - 4], argv[i]);
    }
    packet_file_write(run, argv[4], run->emitted.text, run->emitted.length);
    packet_file_write(run, argv[5], run->expected_closed.bytes, run->expected_closed.length);
    packet_file_write(run, argv[6], proof.identity, 32); packet_file_write(run, argv[7], binding, (size_t)length);
    printf("typed-library-native-material actual-C-bytes=%zu compiled-sidecar-sha256=%s authentic-Closed-bytes=%zu runtime=NOT_RUN\n",
        run->emitted.length, hex, proof.length);
    xr_xir_compile_c_source_free(&run->emitted); poison(digest, sizeof(digest)); poison(hex, sizeof(hex)); poison(binding, sizeof(binding));
}
#else
extern const XrXirProgramSpec source_typed_library_current_native_program;
extern const char source_typed_library_current_native_c_sha256[65];
typedef struct ReceiverNativeOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *vm_entries, *entries;
    uint64_t *native_resumes, *vm_resumes;
    uint64_t child_native_suspends, child_vm_suspends;
    uint32_t count, child;
    unsigned mode;
} ReceiverNativeOwner;
static ReceiverNativeOwner *receiver_native_owner;
static unsigned receiver_native_releases;
static bool callback_check(bool condition, int line, const char *expression) {
    if (condition) return true;
    receiver_observer_failed = true;
    if (!receiver_failure_line) receiver_failure_line = line;
    record_failure("CALLBACK_OBSERVATION_ASSERTION", "real-typed-Library-native-or-VM-callback", false, 0, 0, expression);
    fprintf(stderr, "callback-failure line=%d condition=%s; natural-product-return\n", line, expression);
    return false;
}
#define RECEIVER_CALLBACK(c) callback_check(!!(c), __LINE__, #c)
/* Tokens do not exist yet here. The host separately consumes actual WAIT_YIELD. */
static XrXirAction observe_child_action(ReceiverNativeOwner *owner, uint32_t function, bool native, XrXirAction action) {
    if (function == owner->child && action.kind == XR_XIR_ACTION_SUSPEND) {
        uint64_t *counter = native ? &owner->child_native_suspends : &owner->child_vm_suspends;
        if (!RECEIVER_CALLBACK(*counter < UINT64_MAX)) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
        ++*counter;
    }
    return action;
}
/* Resume is the only replaced field; the exact admitted view goes to real code. */
static XrXirAction observed_native_resume(XrXirCallView *view) {
    ReceiverNativeOwner *owner = receiver_native_owner;
    if (!RECEIVER_CALLBACK(owner && view && xr_xir_call_admission(view))) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    uint32_t f = xr_xir_call_current_entry(view->activation);
    if (!RECEIVER_CALLBACK(f < owner->count && owner->entries[f].resume == observed_native_resume))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    const XrXirCallEntry *delegate = &source_typed_library_current_native_program.entries[f];
    if (!RECEIVER_CALLBACK(delegate->resume && view->environment == delegate->environment && owner->native_resumes[f] < UINT64_MAX))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    ++owner->native_resumes[f];
    return observe_child_action(owner, f, true, delegate->resume(view));
}
static XrXirAction observed_vm_resume(XrXirCallView *view) {
    ReceiverNativeOwner *owner = receiver_native_owner;
    if (!RECEIVER_CALLBACK(owner && view && xr_xir_call_admission(view))) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    uint32_t f = xr_xir_call_current_entry(view->activation);
    if (!RECEIVER_CALLBACK(f < owner->count && owner->entries[f].resume == observed_vm_resume && owner->vm_entries))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    const XrXirCallEntry *delegate = &owner->vm_entries[f];
    if (!RECEIVER_CALLBACK(delegate->resume && view->environment == delegate->environment && owner->vm_resumes[f] < UINT64_MAX))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    ++owner->vm_resumes[f];
    return observe_child_action(owner, f, false, delegate->resume(view));
}
static void dispose_receiver_native_owner(ReceiverNativeOwner *owner) {
    xr_xir_compile_artifact_free(owner->lowered); xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->vm_entries); xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner->native_resumes); xr_compile_resources_free(owner->vm_resumes);
    xr_compile_resources_free(owner);
}
static void receiver_native_release(void *pointer) {
    ReceiverNativeOwner *owner = receiver_native_owner;
    (void)RECEIVER_CALLBACK(owner && pointer == owner && !receiver_native_releases);
    if (!owner) return;
    receiver_native_owner = NULL; ++receiver_native_releases; dispose_receiver_native_owner(owner);
}
static void native_material_file_digest(ReceiverRun *run, const char *path) {
    receiver_operation = run->operation = "receiver-actual-host-C-versus-compiled-writer-sidecar";
    CHECK(path && !run->packet_file && !run->transient_bytes);
    run->packet_file = fopen(path, "rb"); CHECK(run->packet_file && !fseek(run->packet_file, 0, SEEK_END));
    long length = ftell(run->packet_file); CHECK(length > 0 && length <= 16777216 && !fseek(run->packet_file, 0, SEEK_SET));
    run->transient_size = (size_t)length; run->transient_bytes = owned_allocate(run->context, run->transient_size, 1);
    CHECK(fread(run->transient_bytes, 1, run->transient_size, run->packet_file) == run->transient_size);
    CHECK(fgetc(run->packet_file) == EOF && !ferror(run->packet_file)); packet_file_close(run);
    uint8_t digest[32]; char hex[65]; xr_sha256(run->transient_bytes, run->transient_size, digest);
    for (unsigned i = 0; i < 32; ++i) CHECK(snprintf(hex + i * 2, 3, "%02x", digest[i]) == 2);
    CHECK(!strcmp(hex, source_typed_library_current_native_c_sha256));
    poison(run->transient_bytes, run->transient_size); xr_compile_resources_free(run->transient_bytes);
    run->transient_bytes = NULL; run->transient_size = 0; poison(digest, sizeof(digest)); poison(hex, sizeof(hex));
}
static void native_select_entries(ReceiverRun *run, ReceiverNativeOwner *owner) {
    const XrXirProgramSpec *compiled = &source_typed_library_current_native_program;
    unsigned natives = 0, vms = 0;
    for (uint32_t f = 0; f < owner->count; ++f) {
        bool native = owner->mode == 1 || (f % 2 == 0) == (owner->mode == 2);
        if (owner->mode != 1 && f == run->roles.constructor) native = owner->mode == 2;
        if (owner->mode != 1 && f == run->roles.child) native = owner->mode == 3;
        owner->entries[f] = native ? compiled->entries[f] : owner->vm_entries[f];
        CHECK(owner->entries[f].resume && owner->entries[f].release);
        owner->entries[f].resume = native ? observed_native_resume : observed_vm_resume;
        if (native) ++natives; else ++vms;
    }
    CHECK(natives && (owner->mode == 1 ? !vms : vms != 0));
}
static void seal_receiver_native(ReceiverRun *run, const char *c_path) {
    CHECK(run->lowered && !run->program && !receiver_native_owner && !receiver_native_releases && run->mode >= 1 && run->mode <= 3);
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    const XrXirProgramSpec *compiled = &source_typed_library_current_native_program;
    const XrXirTarget *target = xr_xir_compile_artifact_target(run->lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(module && module->stage == XR_XIR_LOWERED && compiled->entry_count == module->function_count);
    CHECK(target && compiled->abi_version == XR_XIR_PROGRAM_ABI_VERSION && compiled->target.architecture == target->architecture &&
        compiled->target.abi_version == target->abi_version);
    CHECK(compiled->entries && proof.bytes && proof.identity && proof.layouts && compiled->proof.bytes && compiled->proof.identity && compiled->proof.layouts);
    CHECK(proof.length == compiled->proof.length && !memcmp(proof.bytes, compiled->proof.bytes, proof.length));
    CHECK(!memcmp(proof.identity, compiled->proof.identity, 32)); native_material_file_digest(run, c_path);
    ReceiverNativeOwner *owner = owned_allocate(run->context, 1, sizeof(*owner)); run->pending_native_owner = owner;
    owner->count = module->function_count; owner->mode = run->mode; owner->child = run->roles.child;
    CHECK(run->roles.constructor < owner->count && owner->child < owner->count && run->roles.constructor != owner->child);
    owner->entries = owned_allocate(run->context, owner->count, sizeof(*owner->entries));
    owner->native_resumes = owned_allocate(run->context, owner->count, sizeof(*owner->native_resumes));
    owner->vm_resumes = owned_allocate(run->context, owner->count, sizeof(*owner->vm_resumes));
    if (owner->mode != 1) CHECK(completed(run, xr_xir_compile_vm_bind_table(run->lowered,
        &owner->bindings, &owner->vm_entries), "public-complete-VM-bind-table"));
    native_select_entries(run, owner);
    XrXirProgramSpec spec = *compiled;
    spec.entries = owner->entries; spec.entry_count = owner->count;
    spec.declarations = module->declarations; spec.types = module->types;
    /* Public sealing also authenticates the actual compiled layouts against the packet. */
    spec.code = (XrXirCodeLease){owner, receiver_native_release};
    CHECK(completed(run, xr_xir_compile_program_seal(run->context, &spec, &run->program), "public-native-mixed-complete-compiledproof-seal"));
    CHECK(run->program); owner->lowered = run->lowered; run->lowered = NULL;
    receiver_native_owner = owner; run->pending_native_owner = NULL;
}
typedef struct NativeRouteCounts { uint64_t native[2], vm[2], child_suspends[2]; } NativeRouteCounts;
static NativeRouteCounts native_route_counts(ReceiverRoles roles) {
    CHECK(receiver_native_owner); NativeRouteCounts counts;
    const uint32_t functions[2] = {roles.constructor, roles.child};
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(functions[n] < receiver_native_owner->count);
        counts.native[n] = receiver_native_owner->native_resumes[functions[n]];
        counts.vm[n] = receiver_native_owner->vm_resumes[functions[n]];
    }
    counts.child_suspends[0] = receiver_native_owner->child_native_suspends;
    counts.child_suspends[1] = receiver_native_owner->child_vm_suspends;
    return counts;
}
static void check_actual_route(unsigned mode, NativeRouteCounts before, NativeRouteCounts after) {
    for (unsigned n = 0; n < 2; ++n) {
        bool native = mode == 1 || (mode == 2 ? n == 0 : n == 1);
        CHECK(native ? after.native[n] > before.native[n] : after.vm[n] > before.vm[n]);
        CHECK(native ? after.vm[n] == before.vm[n] : after.native[n] == before.native[n]);
    }
    unsigned selected_child = mode == 1 || mode == 3 ? 0u : 1u;
    CHECK(after.child_suspends[selected_child] >= before.child_suspends[selected_child] &&
        after.child_suspends[selected_child] - before.child_suspends[selected_child] == 2);
    CHECK(after.child_suspends[1 - selected_child] == before.child_suspends[1 - selected_child]);
}
#if defined(XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER)
static void check_actual_cancel_route(unsigned mode, unsigned prefix, NativeRouteCounts before, NativeRouteCounts after) {
    CHECK(prefix == 1 || prefix == 2);
    for (unsigned n = 0; n < 2; ++n) {
        bool native = mode == 1 || (mode == 2 ? n == 0 : n == 1);
        CHECK(native ? after.native[n] > before.native[n] : after.vm[n] > before.vm[n]);
        CHECK(native ? after.vm[n] == before.vm[n] : after.native[n] == before.native[n]);
    }
    unsigned selected_child = mode == 1 || mode == 3 ? 0u : 1u;
    CHECK(after.child_suspends[selected_child] >= before.child_suspends[selected_child] &&
        after.child_suspends[selected_child] - before.child_suspends[selected_child] == prefix);
    CHECK(after.child_suspends[1 - selected_child] == before.child_suspends[1 - selected_child]);
}
#endif
#endif
#endif // TYPED_LIBRARY_CURRENT_NATIVE_MIXED_SUPPORT_H
