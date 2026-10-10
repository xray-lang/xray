/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_mixed_support.h - Actual C payload and normal dispatch observations
 *
 * KEY CONCEPT:
 *   One authentic Lowered proof binds the generated C and VM callbacks. The
 *   observer preserves each active view and records actual provider execution.
 */
#ifndef SOURCE_STATIC_CURRENT_NATIVE_MIXED_SUPPORT_H
#define SOURCE_STATIC_CURRENT_NATIVE_MIXED_SUPPORT_H
#include "base/xsha256.h"
#if !defined(XR_SOURCE_STATIC_NATIVE_RUNNER)
#include "xir/xxir_emit_c.h"

static void native_payload_digest(const XrXirCSource *source, char digest[65]) {
    uint8_t bytes[32];
    xr_sha256((const uint8_t *)source->text, source->length, bytes);
    for (unsigned i = 0; i < 32; ++i)
        CHECK(snprintf(digest + i * 2, 3, "%02x", bytes[i]) == 2);
}

static void native_write_file(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(bytes, 1, length, file) == length && !fclose(file));
}
static void emit_native_material(const XrXirArtifact *lowered, const char *c_path,
    const char *proof_path, const char *identity_path, const char *binding_path) {
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "source_static_current_native", 16777216, &source) == XR_XIR_OK);
    CHECK(source.text && source.length);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(proof.bytes && proof.length && proof.identity && proof.layouts);
    char digest[65]; native_payload_digest(&source, digest);
    /* The actual emitted C is unchanged. Metadata lives in a separate C unit. */
    native_write_file(c_path, source.text, source.length);
    native_write_file(proof_path, proof.bytes, proof.length);
    native_write_file(identity_path, proof.identity, 32);
    char binding[256];
    int length = snprintf(binding, sizeof(binding),
        "const char source_static_current_native_c_sha256[65] = \"%s\";\n", digest);
    CHECK(length > 0 && (size_t)length < sizeof(binding));
    native_write_file(binding_path, binding, (size_t)length);
    printf("native-material actual-C-bytes=%zu actual-C-sha256=%s authentic-proof-bytes=%zu runtime=NOT_RUN full-FI=NOT_RUN\n",
        source.length, digest, proof.length);
    xr_xir_compile_c_source_free(&source);
}
#else
extern const XrXirProgramSpec source_static_current_native_program;
extern const char source_static_current_native_c_sha256[65];

typedef struct NativePacketInput {
    uint8_t *bytes;
    size_t length;
    uint8_t identity[32];
    char packet_path[2048], identity_path[2048];
} NativePacketInput;
static void native_input_poison(void *pointer, size_t length) {
    volatile uint8_t *bytes = pointer;
    for (size_t i = 0; i < length; ++i) bytes[i] = 0xa5;
}

/* The receiving ledger owns file bytes only until checked_read detaches them. */
static void native_packet_load(const XrXirCompileContext *context, const char *packet_path,
    const char *identity_path, NativePacketInput *input) {
    CHECK(!input->bytes && !input->length);
    CHECK(strlen(packet_path) < sizeof(input->packet_path) && strlen(identity_path) < sizeof(input->identity_path));
    strcpy(input->packet_path, packet_path); strcpy(input->identity_path, identity_path);
    FILE *file = fopen(input->packet_path, "rb");
    CHECK(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    CHECK(length >= 64 && length <= 16777216 && !fseek(file, 0, SEEK_SET));
    input->length = (size_t)length;
    void *bytes = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, input->length, &bytes) == XR_COMPILE_RESOURCE_OK);
    input->bytes = bytes;
    CHECK(fread(input->bytes, 1, input->length, file) == input->length);
    CHECK(fgetc(file) == EOF && !ferror(file) && !fclose(file));
    file = fopen(input->identity_path, "rb");
    CHECK(file && fread(input->identity, 1, sizeof(input->identity), file) == sizeof(input->identity));
    CHECK(fgetc(file) == EOF && !ferror(file) && !fclose(file));
    uint8_t digest[32]; xr_sha256(input->bytes, input->length, digest);
    CHECK(!memcmp(digest, input->identity, sizeof(digest)));
    native_input_poison(digest, sizeof(digest));
}
static void native_packet_die(NativePacketInput *input) {
    CHECK(input->bytes && input->length);
    native_input_poison(input->bytes, input->length);
    CHECK(input->bytes[0] == 0xa5 && input->bytes[input->length - 1] == 0xa5);
    xr_compile_resources_free(input->bytes); input->bytes = NULL; input->length = 0;
    native_input_poison(input->identity, sizeof(input->identity));
    native_input_poison(input->packet_path, sizeof(input->packet_path));
    native_input_poison(input->identity_path, sizeof(input->identity_path));
}

/* Check the actual C file compiled by the host, without invoking the emitter. */
static void native_material_file_digest(const XrXirCompileContext *context, const char *path) {
    char copied_path[2048];
    CHECK(strlen(path) < sizeof(copied_path)); strcpy(copied_path, path);
    FILE *file = fopen(copied_path, "rb");
    CHECK(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    CHECK(length > 0 && length <= 16777216 && !fseek(file, 0, SEEK_SET));
    void *bytes = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, (size_t)length, &bytes) == XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    CHECK(fgetc(file) == EOF && !ferror(file) && !fclose(file));
    uint8_t digest[32]; xr_sha256(bytes, (size_t)length, digest);
    char text[65];
    for (unsigned i = 0; i < 32; ++i)
        CHECK(snprintf(text + i * 2, 3, "%02x", digest[i]) == 2);
    CHECK(!strcmp(text, source_static_current_native_c_sha256));
    native_input_poison(bytes, (size_t)length); xr_compile_resources_free(bytes);
    native_input_poison(digest, sizeof(digest)); native_input_poison(text, sizeof(text));
    native_input_poison(copied_path, sizeof(copied_path));
}
typedef struct ObservedNativeOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *vm_entries, *entries;
    uint64_t *native_resumes, *vm_resumes;
    uint32_t count;
    unsigned mode;
} ObservedNativeOwner;
static ObservedNativeOwner *observed_native_owner;
static unsigned observed_code_lease_releases;

/* The exact active view pointer, environment and all other facts pass unchanged. */
static XrXirAction observed_native_resume(XrXirCallView *view) {
    ObservedNativeOwner *owner = observed_native_owner;
    CHECK(owner && xr_xir_call_admission(view));
    uint32_t function = xr_xir_call_current_entry(view->activation);
    CHECK(function < owner->count && owner->entries[function].resume == observed_native_resume);
    const XrXirCallEntry *original = &source_static_current_native_program.entries[function];
    CHECK(original->resume && view->environment == original->environment);
    CHECK(owner->native_resumes[function] < UINT64_MAX);
    ++owner->native_resumes[function];
    return original->resume(view);
}
static XrXirAction observed_vm_resume(XrXirCallView *view) {
    ObservedNativeOwner *owner = observed_native_owner;
    CHECK(owner && xr_xir_call_admission(view));
    uint32_t function = xr_xir_call_current_entry(view->activation);
    CHECK(function < owner->count && owner->entries[function].resume == observed_vm_resume);
    const XrXirCallEntry *original = &owner->vm_entries[function];
    CHECK(original->resume && view->environment == original->environment);
    CHECK(owner->vm_resumes[function] < UINT64_MAX);
    ++owner->vm_resumes[function];
    return original->resume(view);
}
static void observed_native_release(void *pointer) {
    ObservedNativeOwner *owner = pointer;
    CHECK(owner && observed_native_owner == owner);
    observed_native_owner = NULL;
    CHECK(observed_code_lease_releases == 0); ++observed_code_lease_releases;
    xr_xir_compile_artifact_free(owner->lowered);
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->vm_entries);
    xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner->native_resumes);
    xr_compile_resources_free(owner->vm_resumes);
    xr_compile_resources_free(owner);
}
static void *native_owner_allocate(const XrXirCompileContext *context, size_t count, size_t stride) {
    void *pointer = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, count, stride, &pointer) == XR_COMPILE_RESOURCE_OK);
    return pointer;
}
static XrXirProgram *seal_observed_native(const XrXirCompileContext *context, XrXirArtifact **lowered,
    unsigned mode, StaticEntries roles, const char *c_path) {
    CHECK(lowered && *lowered && mode >= 1 && mode <= 3 && !observed_native_owner && !observed_code_lease_releases);
    const XrXirModule *module = xr_xir_compile_artifact_module(*lowered);
    const XrXirProgramSpec *compiled = &source_static_current_native_program;
    XrXirProgramProof proof = xr_xir_compile_program_proof(*lowered);
    CHECK(module && module->declarations && compiled->entry_count == module->function_count);
    CHECK(proof.bytes && proof.identity && proof.layouts && compiled->proof.bytes && compiled->proof.identity);
    CHECK(proof.length == compiled->proof.length && !memcmp(proof.bytes, compiled->proof.bytes, proof.length));
    CHECK(!memcmp(proof.identity, compiled->proof.identity, 32));
    native_material_file_digest(context, c_path);
    ObservedNativeOwner *owner = native_owner_allocate(context, 1, sizeof(*owner));
    owner->count = module->function_count; owner->mode = mode;
    owner->entries = native_owner_allocate(context, owner->count, sizeof(*owner->entries));
    owner->native_resumes = native_owner_allocate(context, owner->count, sizeof(*owner->native_resumes));
    owner->vm_resumes = native_owner_allocate(context, owner->count, sizeof(*owner->vm_resumes));
    if (mode != 1)
        CHECK(xr_xir_compile_vm_bind_table(*lowered, &owner->bindings, &owner->vm_entries) == XR_XIR_OK);
    unsigned native_count = 0, vm_count = 0;
    for (uint32_t f = 0; f < owner->count; ++f) {
        bool native = mode == 1 || (f % 2 == 0) == (mode == 2);
        if (mode != 1 && f == roles.value_methods[0]) native = mode == 2;
        if (mode != 1 && f == roles.value_methods[1]) native = mode == 3;
        owner->entries[f] = native ? compiled->entries[f] : owner->vm_entries[f];
        CHECK(owner->entries[f].resume && owner->entries[f].release);
        /* Only resume changes. All authenticated ABI/layout/cleanup facts remain. */
        owner->entries[f].resume = native ? observed_native_resume : observed_vm_resume;
        if (native) ++native_count; else ++vm_count;
    }
    CHECK(native_count && (mode == 1 ? !vm_count : vm_count != 0));
    XrXirProgramSpec spec = *compiled;
    spec.target = *xr_xir_compile_artifact_target(*lowered);
    spec.entries = owner->entries; spec.entry_count = owner->count;
    spec.declarations = module->declarations; spec.types = module->types; spec.proof = proof;
    spec.code = (XrXirCodeLease){owner, observed_native_release};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK && program);
    owner->lowered = *lowered; *lowered = NULL;
    observed_native_owner = owner;
    printf("native-binding mode=%u actual-native=%u actual-VM=%u emitted-C-sha256=%s\n",
        mode, native_count, vm_count, source_static_current_native_c_sha256);
    return program;
}
typedef struct MethodResumeCounts { uint64_t native[2], vm[2]; } MethodResumeCounts;
static MethodResumeCounts method_resume_counts(StaticEntries roles) {
    ObservedNativeOwner *owner = observed_native_owner;
    CHECK(owner);
    MethodResumeCounts counts;
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(roles.value_methods[n] < owner->count);
        counts.native[n] = owner->native_resumes[roles.value_methods[n]];
        counts.vm[n] = owner->vm_resumes[roles.value_methods[n]];
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
#endif
