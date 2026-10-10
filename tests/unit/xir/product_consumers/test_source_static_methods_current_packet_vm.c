/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_static_methods_current_packet_vm.c - Source-free owned packet execution
 *
 * KEY CONCEPT:
 *   A separate normal writer exports a real CLOSED packet and its full identity.
 *   The VM process owns decoded facts after all borrowed packet inputs die.
 */
#if defined(XR_SOURCE_STATIC_PACKET_WRITER)
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#endif
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "static_methods_source_shape.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 28u && XR_XIR_CHECKED_CONTRACT == 73u, "Exact formal Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 23u && XR_XIR_CALL_ABI_VERSION == 29u &&
    XR_XIR_PROGRAM_ABI_VERSION == 30u, "Exact formal public runtime identity");

/* Observe an actual owner. Complete admission belongs to the public verifier. */
static void construction_owner(const XrXirArtifact *artifact, const XrXirCompileContext *caller) {
    const XrXirCompileContext *context = xr_xir_compile_artifact_context(artifact);
    const XrXirConstruction *construction = xr_xir_compile_artifact_construction(artifact);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    CHECK(artifact && context && context->resources == caller->resources && module && construction);
    const XrXirNominalTable *nominals = module->types ? module->types->nominals : NULL;
    uint32_t count = nominals && nominals->declarations ? nominals->count : 0;
    CHECK(xr_xir_compile_construction_count(construction) == count);
    for (uint32_t n = 0; n < count; ++n) {
        const XrXirConstructionRow *row = xr_xir_compile_construction_row(construction, n);
        CHECK(row && row->field_count == nominals->declarations[n].field_count);
        CHECK(!!row->field_initializers == !!row->field_count);
    }
    CHECK(!xr_xir_compile_construction_row(construction, count));
    CHECK(xr_xir_compile_verify_v2(context, module, construction, NULL) == XR_XIR_OK);
}

#if defined(XR_SOURCE_STATIC_PACKET_WRITER)
/* Retain the complete authentic producer packet while its borrowed view lives. */
static void packet_copy(const XrXirCompileContext *context, const XrXirSourceProductPacketView *view,
    XrXirCheckedPacket *owned) {
    CHECK(view->bytes && view->length && view->length <= UINT64_C(16777216));
    CHECK(!owned->bytes && !owned->length);
    void *bytes = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, view->length, &bytes) == XR_COMPILE_RESOURCE_OK);
    owned->bytes = bytes; owned->length = view->length;
    memcpy(owned->bytes, view->bytes, view->length);
}

static void packet_exact(const XrXirArtifact *artifact, const XrXirCheckedPacket *expected) {
    XrXirCheckedPacket actual = {0};
    CHECK(xr_xir_compile_checked_write(artifact, &actual, NULL) == XR_XIR_OK);
    CHECK(actual.length == expected->length && !memcmp(actual.bytes, expected->bytes, actual.length));
    xr_xir_compile_checked_packet_free(&actual);
}
#endif

typedef struct StaticEntries { uint32_t entry, answer, private_answer, value_methods[2]; } StaticEntries;
static StaticEntries source_entries(const XrXirModule *module) {
    CHECK(module && module->declarations);
    const XrXirDeclarations *declarations = module->declarations;
    StaticEntries found = {declarations->entry_function, UINT32_MAX, UINT32_MAX, {UINT32_MAX, UINT32_MAX}};
    CHECK(found.entry < module->function_count);
    CHECK(!module->functions[found.entry].parameter_count && module->functions[found.entry].result == XR_XIR_I64);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &declarations->functions[f];
        if (identity->method_kind == XR_XIR_STATIC_METHOD) {
            CHECK(identity->nominal_owner == 1 || identity->nominal_owner == 2);
            uint32_t nominal = identity->nominal_owner - 1;
            CHECK(found.value_methods[nominal] == UINT32_MAX);
            CHECK(function->name_length == 5 && !memcmp(function->name, "value", 5));
            found.value_methods[nominal] = f;
        }
        if (function->name_length == 14 && !memcmp(function->name, "consumerAnswer", 14)) {
            CHECK(found.answer == UINT32_MAX && identity->exported && identity->module == declarations->root_module);
            CHECK(!function->parameter_count && function->result == XR_XIR_I64);
            found.answer = f;
        }
        if (function->name_length == 6 && !memcmp(function->name, "answer", 6)) {
            CHECK(found.private_answer == UINT32_MAX && !identity->exported);
            CHECK(!function->parameter_count && function->result == XR_XIR_I64);
            found.private_answer = f;
        }
    }
    CHECK(found.answer != UINT32_MAX && found.private_answer != UINT32_MAX && found.answer != found.private_answer);
    CHECK(found.value_methods[0] != UINT32_MAX && found.value_methods[1] != UINT32_MAX && found.value_methods[0] != found.value_methods[1]);
    return found;
}

#include "base/xsha256.h"

/* Volatile stores make borrowed-input poisoning observable before release. */
static void packet_poison(void *pointer, size_t length) {
    volatile uint8_t *bytes = pointer;
    for (size_t i = 0; i < length; ++i) bytes[i] = 0xa5;
}

#if defined(XR_SOURCE_STATIC_PACKET_WRITER)
static void packet_file_write(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(bytes, 1, length, file) == length && !fclose(file));
}
static int write_source_packet(char **argv) {
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    const char *root_input = argv[1], *file_input = argv[2], *stdlib_input = argv[3];
    char root[2048], file[2048];
    CHECK(strlen(root_input) < sizeof(root) && strlen(file_input) < sizeof(file));
    strcpy(root, root_input); strcpy(file, file_input);
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{session, file, &authority, context, stdlib_input, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    XrXirArtifact *source_checked = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket original_source_packet = {0}, original_closed_packet = {0};
    uint32_t product_entry = UINT32_MAX;
    if (status == XR_XIR_OK) {
        CHECK(product && xr_xir_compile_source_product_context(product)->resources == context->resources);
        CHECK(xr_xir_compile_source_product_verify(product, 16777216, NULL) == XR_XIR_OK);
        XrXirSourceDiagnostic admission = {0};
        CHECK(xr_xir_compile_source_product_public_admit(product, &admission) == XR_XIR_OK);
        const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
        CHECK(facts); product_entry = facts->entry;
        XrXirSourceProductPacketView packet = {0};
        CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_SOURCE, &packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &source_checked, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_verify(source_checked, NULL) == XR_XIR_OK);
        construction_owner(source_checked, context);
        static_methods_source_shape(xr_xir_compile_artifact_module(source_checked));
        packet_copy(context, &packet, &original_source_packet);
        CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
        packet_copy(context, &packet, &original_closed_packet);
    } else CHECK(!product && diagnostic.status == status && diagnostic.source.message[0]);
    XrXirSourceProductDiagnostic saved = diagnostic;
    char owned_path[2048] = {0};
    if (diagnostic.source_path) {
        CHECK(strlen(diagnostic.source_path) < sizeof(owned_path));
        strcpy(owned_path, diagnostic.source_path);
    }
    xr_compile_session_free(session); session = NULL;
    xr_xir_compile_source_product_free(product); product = NULL;
    memset(root, 0xa5, sizeof(root)); memset(file, 0xa5, sizeof(file));
    memset(&request, 0xa5, sizeof(request)); memset(&authority, 0xa5, sizeof(authority));
    CHECK(!memcmp(&saved, &diagnostic, sizeof(saved)));
    if (owned_path[0]) CHECK(!strcmp(owned_path, diagnostic.source_path));
    if (source_checked) {
        CHECK(xr_xir_compile_artifact_verify(source_checked, NULL) == XR_XIR_OK);
        construction_owner(source_checked, context);
        static_methods_source_shape(xr_xir_compile_artifact_module(source_checked));
        packet_exact(source_checked, &original_source_packet);
        xr_xir_compile_checked_packet_free(&original_source_packet);
        CHECK(xr_xir_compile_specialize(source_checked, &closed, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(source_checked); source_checked = NULL;
        CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
        construction_owner(closed, context);
        static_methods_source_shape(xr_xir_compile_artifact_module(closed));
        packet_exact(closed, &original_closed_packet);
        XrXirCheckedPacket exported_closed = {0};
        CHECK(xr_xir_compile_checked_write(closed, &exported_closed, NULL) == XR_XIR_OK);
        CHECK(exported_closed.length == original_closed_packet.length &&
            !memcmp(exported_closed.bytes, original_closed_packet.bytes, exported_closed.length));
        xr_xir_compile_checked_packet_free(&original_closed_packet);
        StaticEntries entries = source_entries(xr_xir_compile_artifact_module(closed));
        CHECK(entries.entry == product_entry);
        XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(closed); closed = NULL;
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
        construction_owner(lowered, context);
        StaticEntries actual = source_entries(xr_xir_compile_artifact_module(lowered));
        CHECK(actual.entry == entries.entry && actual.answer == entries.answer && actual.private_answer == entries.private_answer);
        CHECK(actual.value_methods[0] == entries.value_methods[0] && actual.value_methods[1] == entries.value_methods[1]);
        XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
        CHECK(proof.bytes && proof.length && proof.identity && proof.layouts);
        CHECK(proof.length == exported_closed.length &&
            !memcmp(proof.bytes, exported_closed.bytes, proof.length));
        uint8_t identity[32]; xr_sha256(exported_closed.bytes, exported_closed.length, identity);
        CHECK(!memcmp(identity, proof.identity, sizeof(identity)));
        packet_file_write(argv[4], exported_closed.bytes, exported_closed.length);
        packet_file_write(argv[5], proof.identity, 32);
        printf("authentic-CLOSED bytes=%zu same-Lowered-proof=1 runtime=NOT_RUN full-FI=NOT_RUN\n", exported_closed.length);
        xr_xir_compile_checked_packet_free(&exported_closed);
        xr_xir_compile_artifact_free(lowered); lowered = NULL;
        packet_poison(identity, sizeof(identity));
    } else fprintf(stderr, "packet-writer Source required=0 actual=%u stage=%u message=%s\n",
        status, diagnostic.stage, diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    return status == XR_XIR_OK ? 0 : 1;
}

#else
typedef struct BorrowedPacketInput {
    uint8_t *bytes;
    size_t length;
    uint8_t identity[32];
    char packet_path[2048], identity_path[2048];
} BorrowedPacketInput;

/* File bytes belong to the receiving ledger only until checked_read returns. */
static void packet_file_load(const XrXirCompileContext *context, char **argv, BorrowedPacketInput *input) {
    CHECK(!input->bytes && !input->length);
    CHECK(strlen(argv[1]) < sizeof(input->packet_path) && strlen(argv[2]) < sizeof(input->identity_path));
    strcpy(input->packet_path, argv[1]); strcpy(input->identity_path, argv[2]);
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
    packet_poison(digest, sizeof(digest));
}
static void packet_input_die(BorrowedPacketInput *input) {
    CHECK(input->bytes && input->length);
    packet_poison(input->bytes, input->length);
    CHECK(input->bytes[0] == 0xa5 && input->bytes[input->length - 1] == 0xa5);
    xr_compile_resources_free(input->bytes); input->bytes = NULL; input->length = 0;
    packet_poison(input->identity, sizeof(input->identity));
    packet_poison(input->packet_path, sizeof(input->packet_path));
    packet_poison(input->identity_path, sizeof(input->identity_path));
}

typedef struct PacketVmOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *delegates, *entries;
    uint64_t *resumes;
    uint32_t count;
} PacketVmOwner;
static PacketVmOwner *packet_vm_owner;
static unsigned packet_vm_releases;

/* The real public VM delegate sees the exact original admitted active view. */
static XrXirAction observed_packet_vm_resume(XrXirCallView *view) {
    PacketVmOwner *owner = packet_vm_owner;
    CHECK(owner && xr_xir_call_admission(view));
    uint32_t function = xr_xir_call_current_entry(view->activation);
    CHECK(function < owner->count && owner->entries[function].resume == observed_packet_vm_resume);
    const XrXirCallEntry *delegate = &owner->delegates[function];
    CHECK(delegate->resume && view->environment == delegate->environment);
    CHECK(owner->resumes[function] < UINT64_MAX); ++owner->resumes[function];
    return delegate->resume(view);
}
static void packet_vm_release(void *pointer) {
    PacketVmOwner *owner = pointer;
    CHECK(owner && owner == packet_vm_owner && !packet_vm_releases);
    packet_vm_owner = NULL; ++packet_vm_releases;
    xr_xir_compile_artifact_free(owner->lowered);
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->delegates);
    xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner->resumes);
    xr_compile_resources_free(owner);
}
static void *packet_vm_allocate(const XrXirCompileContext *context, size_t count, size_t stride) {
    void *pointer = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, count, stride, &pointer) == XR_COMPILE_RESOURCE_OK);
    return pointer;
}
static XrXirProgram *packet_vm_seal(const XrXirCompileContext *context, XrXirArtifact **lowered) {
    CHECK(lowered && *lowered && !packet_vm_owner && !packet_vm_releases);
    const XrXirModule *module = xr_xir_compile_artifact_module(*lowered);
    CHECK(module && module->stage == XR_XIR_LOWERED && module->declarations);
    PacketVmOwner *owner = packet_vm_allocate(context, 1, sizeof(*owner));
    owner->count = module->function_count;
    CHECK(xr_xir_compile_vm_bind_table(*lowered, &owner->bindings, &owner->delegates) == XR_XIR_OK);
    owner->entries = packet_vm_allocate(context, owner->count, sizeof(*owner->entries));
    owner->resumes = packet_vm_allocate(context, owner->count, sizeof(*owner->resumes));
    for (uint32_t f = 0; f < owner->count; ++f) {
        owner->entries[f] = owner->delegates[f];
        CHECK(owner->entries[f].resume && owner->entries[f].release);
        owner->entries[f].resume = observed_packet_vm_resume;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, *xr_xir_compile_artifact_target(*lowered),
        owner->entries, owner->count, module->declarations, {owner, packet_vm_release}, module->types,
        xr_xir_compile_program_proof(*lowered)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK && program);
    owner->lowered = *lowered; *lowered = NULL; packet_vm_owner = owner;
    return program;
}
static void execute_packet_i64(XrXirInstance *instance, uint32_t entry, int64_t expected) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallStatus status;
    size_t polls = 0;
    do {
        CHECK(++polls < 4096);
        XrXirInstanceResult borrowed = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(xr_xir_call_result_valid(&borrowed.outcome));
        status = borrowed.outcome.status;
        CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED);
    } while (status == XR_XIR_CALL_READY);
    CHECK(status == XR_XIR_CALL_RETURNED && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_valid(&value) && value.type == XR_XIR_I64 && !value.reserved && (int64_t)value.payload == expected);
    xr_xir_value_drop(&value);
    CHECK(value.type == XR_XIR_UNIT && !value.reserved && !value.payload);
}
static void run_packet_instances(XrXirProgram **program, StaticEntries entries) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = UINT64_C(1048576);
    XrXirInstance *instances[2] = {0};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(*program, &config, &instances[i]) == XR_XIR_CALL_READY && instances[i]);
    CHECK(instances[0] != instances[1]);
    xr_xir_compile_program_drop(*program); *program = NULL;
    CHECK(packet_vm_owner && !packet_vm_releases);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], entries.private_answer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
        execute_packet_i64(instances[i], entries.entry, 0);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            uint64_t before[2];
            for (unsigned n = 0; n < 2; ++n) before[n] = packet_vm_owner->resumes[entries.value_methods[n]];
            execute_packet_i64(instances[i], entries.answer, 42);
            for (unsigned n = 0; n < 2; ++n)
                CHECK(packet_vm_owner->resumes[entries.value_methods[n]] > before[n]);
            printf("source-free instance=%u repeat=%u full-owned-i64=42 actual-First-VM=%llu actual-Second-VM=%llu\n",
                i, repeat, (unsigned long long)(packet_vm_owner->resumes[entries.value_methods[0]] - before[0]),
                (unsigned long long)(packet_vm_owner->resumes[entries.value_methods[1]] - before[1]));
        }
        /* These READY expectations cover only fully successful normal calls. */
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY); instances[i] = NULL;
        if (i == 0) CHECK(packet_vm_owner && !packet_vm_releases);
    }
    CHECK(!packet_vm_owner && packet_vm_releases == 1);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}
static int run_source_free_packet(char **argv) {
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    BorrowedPacketInput input = {0}; packet_file_load(context, argv, &input);
    uint8_t identity[32]; memcpy(identity, input.identity, sizeof(identity));
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(context, input.bytes, input.length, &closed, NULL) == XR_XIR_OK);
    XrXirCheckedPacket expected_closed = {0};
    CHECK(xr_xir_compile_checked_write(closed, &expected_closed, NULL) == XR_XIR_OK);
    CHECK(expected_closed.bytes && expected_closed.bytes != input.bytes && expected_closed.length == input.length &&
        !memcmp(expected_closed.bytes, input.bytes, input.length));
    packet_input_die(&input);
    CHECK(!input.bytes && !input.length);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    construction_owner(closed, context);
    static_methods_source_shape(xr_xir_compile_artifact_module(closed));
    StaticEntries entries = source_entries(xr_xir_compile_artifact_module(closed));
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); closed = NULL;
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    construction_owner(lowered, context);
    StaticEntries actual = source_entries(xr_xir_compile_artifact_module(lowered));
    CHECK(actual.entry == entries.entry && actual.answer == entries.answer && actual.private_answer == entries.private_answer);
    CHECK(actual.value_methods[0] == entries.value_methods[0] && actual.value_methods[1] == entries.value_methods[1]);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(proof.bytes && proof.length && proof.identity && proof.layouts && !memcmp(identity, proof.identity, sizeof(identity)));
    CHECK(proof.length == expected_closed.length && !memcmp(proof.bytes, expected_closed.bytes, proof.length));
    xr_xir_compile_checked_packet_free(&expected_closed);
    packet_poison(identity, sizeof(identity)); proof = (XrXirProgramProof){0};
    XrXirProgram *program = packet_vm_seal(context, &lowered);
    CHECK(!lowered && program && packet_vm_owner);
    run_packet_instances(&program, entries);
    CHECK(!program && !closed && !lowered && !packet_vm_owner && packet_vm_releases == 1);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("source-free-packet normal-two-instances=2 independent-fixed42=4 input-poisoned-freed=1 caller-Program-dead=1 actual-Lowered-lease-released=1");
    return 0;
}
#endif

int main(int argc, char **argv) {
#if defined(XR_SOURCE_STATIC_PACKET_WRITER)
    if (argc != 6) return 2;
    return write_source_packet(argv);
#else
    if (argc != 3) return 2;
    return run_source_free_packet(argv);
#endif
}
