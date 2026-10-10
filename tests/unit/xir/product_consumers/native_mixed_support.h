/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_mixed_support.h - Authentic native material and transparent dispatch
 *
 * KEY CONCEPT:
 *   Current artifact identities select roles; one code lease pins actual native
 *   and VM delegates, and observers return through the product callback stack.
 */
#ifndef SOURCE_STATIC_CURRENT_NATIVE_MIXED_SUPPORT_H
#define SOURCE_STATIC_CURRENT_NATIVE_MIXED_SUPPORT_H
static bool native_name(XrXirLiteral name, const char *text) {
    size_t length = strlen(text);
    return name.bytes && name.length == length && !memcmp(name.bytes, text, length);
}
static void native_nominal_roles(const XrXirModule *module, uint32_t owners[2]) {
    const XrXirNominalTable *table = module->types->nominals;
    CHECK(table && table->count == 2 && (table->declarations || table->identities));
    owners[0] = owners[1] = UINT32_MAX;
    for (uint32_t n = 0; n < table->count; ++n) {
        XrXirLiteral name, scope; uint32_t kind, arity, fields, exported;
        if (table->declarations) {
            const XrXirNominalDeclaration *d = &table->declarations[n];
            name = d->name; scope = d->module; kind = d->kind;
            arity = d->parameter_count; fields = d->field_count; exported = d->exported;
        } else {
            const XrXirNominalIdentity *d = &table->identities[n];
            name = d->name; scope = d->module; kind = d->kind;
            arity = d->arity; fields = d->field_count; exported = d->exported;
        }
        const XrXirSourceModule *root = &module->declarations->modules[module->declarations->root_module];
        CHECK(kind == XR_XIR_NOMINAL_CLASS && !arity && !fields && !exported);
        CHECK(scope.bytes && scope.length == root->name_length && !memcmp(scope.bytes, root->name, scope.length));
        unsigned slot = native_name(name, "First") ? 0u : 1u;
        CHECK((slot == 0 || native_name(name, "Second")) && owners[slot] == UINT32_MAX);
        owners[slot] = n;
    }
    CHECK(owners[0] != UINT32_MAX && owners[1] != UINT32_MAX && owners[0] != owners[1]);
}
static void native_method_body(const XrXirFunction *function, int64_t expected) {
    CHECK(!function->parameter_count && function->result == XR_XIR_I64);
    unsigned constants = 0, returns = 0; uint32_t value = UINT32_MAX;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op == XR_XIR_CONST_INT) {
            ++constants; CHECK(op->type == XR_XIR_I64 && op->immediate == expected); value = i;
        }
        if (op->op == XR_XIR_RETURN) {
            ++returns; CHECK(op->type == XR_XIR_UNIT && value != UINT32_MAX && op->args[0] == value);
        }
    }
    CHECK(constants == 1 && returns == 1);
}
static void native_answer_body(const XrXirFunction *function, const ReceiverRoles *roles) {
    CHECK(!function->parameter_count && function->result == XR_XIR_I64);
    unsigned calls = 0, adds = 0, returns = 0; uint32_t values[2] = {UINT32_MAX, UINT32_MAX}, sum = UINT32_MAX;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op == XR_XIR_CALL) {
            CHECK(calls < 2 && op->type == XR_XIR_I64 && !op->args[1] && op->immediate == roles->value_methods[calls]);
            values[calls++] = i;
        }
        if (op->op == XR_XIR_ADD_INT) {
            ++adds; CHECK(calls == 2 && op->type == XR_XIR_I64 && op->args[0] == values[0] && op->args[1] == values[1]); sum = i;
        }
        if (op->op == XR_XIR_RETURN) { ++returns; CHECK(op->type == XR_XIR_UNIT && sum != UINT32_MAX && op->args[0] == sum); }
    }
    CHECK(calls == 2 && adds == 1 && returns == 1);
}
/* Facts are observations. The public artifact verifier and seal admit them. */
static ReceiverRoles native_current_roles(const XrXirModule *module) {
    CHECK(module && module->linkage_kind == XR_XIR_PROGRAM && module->types && module->declarations &&
        module->declarations->functions && module->declarations->modules && module->functions);
    const XrXirDeclarations *d = module->declarations;
    CHECK(d->root_module < d->module_count);
    uint32_t owners[2]; native_nominal_roles(module, owners);
    ReceiverRoles roles = {d->entry_function, UINT32_MAX, UINT32_MAX, {UINT32_MAX, UINT32_MAX}};
    CHECK(roles.entry < module->function_count && !module->functions[roles.entry].parameter_count &&
        module->functions[roles.entry].result == XR_XIR_I64);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f]; const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->module != d->root_module) continue;
        if (identity->method_kind == XR_XIR_STATIC_METHOD) {
            unsigned slot = identity->nominal_owner == owners[0] + 1 ? 0u : 1u;
            CHECK(identity->nominal_owner == owners[slot] + 1 && roles.value_methods[slot] == UINT32_MAX);
            CHECK(fn->name_length == 5 && !memcmp(fn->name, "value", 5) && identity->member_access == XR_XIR_MEMBER_PUBLIC);
            native_method_body(fn, slot ? 23 : 19); roles.value_methods[slot] = f;
        }
        if (fn->name_length == 14 && !memcmp(fn->name, "consumerAnswer", 14)) {
            CHECK(roles.answer == UINT32_MAX && identity->exported && !fn->parameter_count && fn->result == XR_XIR_I64);
            roles.answer = f;
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) {
            CHECK(roles.private_answer == UINT32_MAX && !identity->exported); roles.private_answer = f;
        }
    }
    CHECK(roles.answer != UINT32_MAX && roles.private_answer != UINT32_MAX && roles.answer != roles.private_answer);
    CHECK(roles.value_methods[0] != UINT32_MAX && roles.value_methods[1] != UINT32_MAX &&
        roles.value_methods[0] != roles.value_methods[1]);
    native_answer_body(&module->functions[roles.private_answer], &roles);
    return roles;
}
#if defined(XR_SOURCE_STATIC_NATIVE_PACKET_WRITER)
static void emit_native_material(ReceiverRun *run, char **argv) {
    CHECK(completed(run, xr_xir_compile_emit_c(run->lowered, "source_static_current_native",
        16777216, &run->emitted), "writer-real-Lowered-C11-emission"));
    CHECK(run->emitted.text && run->emitted.length);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(proof.bytes && proof.identity && proof.layouts && proof.length == run->expected_closed.length);
    CHECK(!memcmp(proof.bytes, run->expected_closed.bytes, proof.length));
    uint8_t digest[32]; char hex[65], binding[256]; xr_sha256((const uint8_t *)run->emitted.text, run->emitted.length, digest);
    for (unsigned i = 0; i < 32; ++i) CHECK(snprintf(hex + i * 2, 3, "%02x", digest[i]) == 2);
    int length = snprintf(binding, sizeof(binding), "const char source_static_current_native_c_sha256[65] = \"%s\";\n", hex);
    CHECK(length > 0 && (size_t)length < sizeof(binding));
    for (unsigned i = 4; i < 8; ++i) {
        CHECK(strlen(argv[i]) < sizeof(run->material_paths[i - 4]));
        for (unsigned j = 4; j < i; ++j) CHECK(strcmp(argv[i], argv[j]));
        strcpy(run->material_paths[i - 4], argv[i]);
    }
    packet_file_write(run, argv[4], run->emitted.text, run->emitted.length);
    packet_file_write(run, argv[5], run->expected_closed.bytes, run->expected_closed.length);
    packet_file_write(run, argv[6], proof.identity, 32); packet_file_write(run, argv[7], binding, (size_t)length);
    printf("native-material actual-C-bytes=%zu compiled-sidecar-sha256=%s authentic-Closed-bytes=%zu runtime=NOT_RUN\n",
        run->emitted.length, hex, proof.length);
    xr_xir_compile_c_source_free(&run->emitted); poison(digest, sizeof(digest)); poison(hex, sizeof(hex)); poison(binding, sizeof(binding));
}
#else
extern const XrXirProgramSpec source_static_current_native_program;
extern const char source_static_current_native_c_sha256[65];
typedef struct ReceiverNativeOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *vm_entries, *entries;
    uint64_t *native_resumes, *vm_resumes;
    uint32_t count;
    unsigned mode;
} ReceiverNativeOwner;
static ReceiverNativeOwner *receiver_native_owner;
static unsigned receiver_native_releases;
static bool callback_check(bool condition, int line, const char *expression) {
    if (condition) return true;
    receiver_observer_failed = true;
    if (!receiver_failure_line) receiver_failure_line = line;
    record_failure("CALLBACK_OBSERVATION_ASSERTION", "real-native-or-VM-callback", false, 0, 0, expression);
    fprintf(stderr, "callback-failure line=%d condition=%s; natural-product-return\n", line, expression);
    return false;
}
#define RECEIVER_CALLBACK(c) callback_check(!!(c), __LINE__, #c)
static XrXirAction observed_native_resume(XrXirCallView *view) {
    ReceiverNativeOwner *owner = receiver_native_owner;
    if (!RECEIVER_CALLBACK(owner && view && xr_xir_call_admission(view))) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    uint32_t f = xr_xir_call_current_entry(view->activation);
    if (!RECEIVER_CALLBACK(f < owner->count && owner->entries[f].resume == observed_native_resume))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    const XrXirCallEntry *delegate = &source_static_current_native_program.entries[f];
    if (!RECEIVER_CALLBACK(delegate->resume && view->environment == delegate->environment && owner->native_resumes[f] < UINT64_MAX))
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARGUMENT);
    ++owner->native_resumes[f]; return delegate->resume(view);
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
    ++owner->vm_resumes[f]; return delegate->resume(view);
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
    receiver_operation = run->operation = "receiver-actual-host-C-versus-compiled-sidecar";
    CHECK(path && !run->packet_file && !run->transient_bytes);
    run->packet_file = fopen(path, "rb"); CHECK(run->packet_file && !fseek(run->packet_file, 0, SEEK_END));
    long length = ftell(run->packet_file); CHECK(length > 0 && length <= 16777216 && !fseek(run->packet_file, 0, SEEK_SET));
    run->transient_size = (size_t)length; run->transient_bytes = owned_allocate(run->context, run->transient_size, 1);
    CHECK(fread(run->transient_bytes, 1, run->transient_size, run->packet_file) == run->transient_size);
    CHECK(fgetc(run->packet_file) == EOF && !ferror(run->packet_file)); packet_file_close(run);
    uint8_t digest[32]; char hex[65]; xr_sha256(run->transient_bytes, run->transient_size, digest);
    for (unsigned i = 0; i < 32; ++i) CHECK(snprintf(hex + i * 2, 3, "%02x", digest[i]) == 2);
    CHECK(!strcmp(hex, source_static_current_native_c_sha256));
    poison(run->transient_bytes, run->transient_size); xr_compile_resources_free(run->transient_bytes);
    run->transient_bytes = NULL; run->transient_size = 0; poison(digest, sizeof(digest)); poison(hex, sizeof(hex));
}
static void native_select_entries(ReceiverRun *run, ReceiverNativeOwner *owner) {
    const XrXirProgramSpec *compiled = &source_static_current_native_program;
    unsigned natives = 0, vms = 0;
    for (uint32_t f = 0; f < owner->count; ++f) {
        bool native = owner->mode == 1 || (f % 2 == 0) == (owner->mode == 2);
        if (owner->mode != 1 && f == run->roles.value_methods[0]) native = owner->mode == 2;
        if (owner->mode != 1 && f == run->roles.value_methods[1]) native = owner->mode == 3;
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
    const XrXirProgramSpec *compiled = &source_static_current_native_program;
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    CHECK(module && module->stage == XR_XIR_LOWERED && compiled->entry_count == module->function_count);
    CHECK(compiled->entries && proof.bytes && proof.identity && proof.layouts && compiled->proof.bytes && compiled->proof.identity);
    CHECK(proof.length == compiled->proof.length && !memcmp(proof.bytes, compiled->proof.bytes, proof.length));
    CHECK(!memcmp(proof.identity, compiled->proof.identity, 32)); native_material_file_digest(run, c_path);
    ReceiverNativeOwner *owner = owned_allocate(run->context, 1, sizeof(*owner)); run->pending_native_owner = owner;
    owner->count = module->function_count; owner->mode = run->mode;
    owner->entries = owned_allocate(run->context, owner->count, sizeof(*owner->entries));
    owner->native_resumes = owned_allocate(run->context, owner->count, sizeof(*owner->native_resumes));
    owner->vm_resumes = owned_allocate(run->context, owner->count, sizeof(*owner->vm_resumes));
    if (owner->mode != 1) CHECK(completed(run, xr_xir_compile_vm_bind_table(run->lowered,
        &owner->bindings, &owner->vm_entries), "public-complete-VM-bind-table"));
    native_select_entries(run, owner);
    XrXirProgramSpec spec = *compiled;
    spec.target = *xr_xir_compile_artifact_target(run->lowered); spec.entries = owner->entries; spec.entry_count = owner->count;
    spec.declarations = module->declarations; spec.types = module->types; spec.proof = proof;
    spec.code = (XrXirCodeLease){owner, receiver_native_release};
    CHECK(completed(run, xr_xir_compile_program_seal(run->context, &spec, &run->program), "public-native-mixed-fullproof-seal"));
    CHECK(run->program); owner->lowered = run->lowered; run->lowered = NULL;
    receiver_native_owner = owner; run->pending_native_owner = NULL;
}
typedef struct MethodResumeCounts { uint64_t native[2], vm[2]; } MethodResumeCounts;
static MethodResumeCounts method_resume_counts(ReceiverRoles roles) {
    CHECK(receiver_native_owner); MethodResumeCounts counts;
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(roles.value_methods[n] < receiver_native_owner->count);
        counts.native[n] = receiver_native_owner->native_resumes[roles.value_methods[n]];
        counts.vm[n] = receiver_native_owner->vm_resumes[roles.value_methods[n]];
    }
    return counts;
}
static void check_actual_method_execution(unsigned mode, MethodResumeCounts before, MethodResumeCounts after) {
    for (unsigned n = 0; n < 2; ++n) {
        bool native = mode == 1 || (mode == 2 ? n == 0 : n == 1);
        CHECK(native ? after.native[n] > before.native[n] : after.vm[n] > before.vm[n]);
        CHECK(native ? after.vm[n] == before.vm[n] : after.native[n] == before.native[n]);
    }
}
#endif
#endif // SOURCE_STATIC_CURRENT_NATIVE_MIXED_SUPPORT_H
