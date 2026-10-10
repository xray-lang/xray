/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_static_methods_current_native_mixed.c - Actual native and mixed execution
 *
 * KEY CONCEPT:
 *   A separate writer exports authentic packet and native C inputs. Two Instances
 *   retain the Lowered lease after the caller drops its Program, and every
 *   exported scalar call keeps the original independent result.
 */
#if !defined(XR_SOURCE_STATIC_NATIVE_RUNNER)
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
_Static_assert(XR_XIR_CHECKED_SCHEMA == 27u && XR_XIR_CHECKED_CONTRACT == 72u, "Exact formal Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Exact formal public runtime identity");

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

#if !defined(XR_SOURCE_STATIC_NATIVE_RUNNER)
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

#if defined(XR_SOURCE_STATIC_NATIVE_RUNNER)
/* Only the existing scalar normal public start/poll/take/drop helper. */
static void execute_fixed_i64(XrXirInstance *instance, uint32_t entry, int64_t expected) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallStatus status;
    size_t polls = 0;
    do {
        CHECK(++polls < 4096);
        XrXirInstanceResult borrowed = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(xr_xir_call_result_valid(&borrowed.outcome));
        status = borrowed.outcome.status;
        CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED);
        /* Poll results borrow; only take_result transfers an owned value. */
    } while (status == XR_XIR_CALL_READY);
    CHECK(status == XR_XIR_CALL_RETURNED && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_valid(&value) && value.type == XR_XIR_I64 && !value.reserved && (int64_t)value.payload == expected);
    xr_xir_value_drop(&value);
    CHECK(value.type == XR_XIR_UNIT && !value.reserved && !value.payload);
}

#endif
#include "native_mixed_support.h"

#if !defined(XR_SOURCE_STATIC_NATIVE_RUNNER)
static int write_source_native(char **argv) {
    const char *root_input = argv[1], *file_input = argv[2], *stdlib_input = argv[3];
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
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
        emit_native_material(lowered, argv[4], argv[5], argv[6], argv[7]);
        xr_xir_compile_artifact_free(lowered); lowered = NULL;
    } else fprintf(stderr, "static-methods-current-native-mixed Source required=0 actual=%u stage=%u message=%s\n",
        status, diagnostic.stage, diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("static-methods-current-native-mixed required=0 actual=%u compiler-physical=0/0 runtime-physical=0/0 result=%s\n",
        status, status == XR_XIR_OK ? "PASS" : "FAIL");
    return status == XR_XIR_OK ? 0 : 1;
}
#else
static void run_native_instances(XrXirProgram **program, StaticEntries entries, unsigned mode) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = UINT64_C(1048576);
    XrXirInstance *instances[2] = {0};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(*program, &config, &instances[i]) == XR_XIR_CALL_READY && instances[i]);
    CHECK(instances[0] != instances[1]);
    xr_xir_compile_program_drop(*program); *program = NULL;
    CHECK(observed_native_owner && observed_code_lease_releases == 0);
    /* Both live Instances retain the real binding/delegate/Lowered code lease. */
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], entries.private_answer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
        execute_fixed_i64(instances[i], entries.entry, 0);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            MethodResumeCounts before_call = method_resume_counts(entries);
            execute_fixed_i64(instances[i], entries.answer, 42);
            MethodResumeCounts after_call = method_resume_counts(entries);
            check_actual_method_execution(mode, before_call, after_call);
            printf("actual-provider instance=%u repeat=%u mode=%u fixed-i64=42 First-native=%llu First-VM=%llu Second-native=%llu Second-VM=%llu\n",
                i, repeat, mode, (unsigned long long)(after_call.native[0] - before_call.native[0]),
                (unsigned long long)(after_call.vm[0] - before_call.vm[0]),
                (unsigned long long)(after_call.native[1] - before_call.native[1]),
                (unsigned long long)(after_call.vm[1] - before_call.vm[1]));
        }
        /* READY is required only for this fully successful normal path. */
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY); instances[i] = NULL;
        if (i == 0) CHECK(observed_native_owner && observed_code_lease_releases == 0);
    }
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    CHECK(!observed_native_owner && observed_code_lease_releases == 1);
}

static int run_source_free_native(char **argv, unsigned mode) {
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    NativePacketInput input = {0}; native_packet_load(context, argv[2], argv[3], &input);
    uint8_t identity[32]; memcpy(identity, input.identity, sizeof(identity));
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(context, input.bytes, input.length, &closed, NULL) == XR_XIR_OK);
    XrXirCheckedPacket expected_closed = {0};
    CHECK(xr_xir_compile_checked_write(closed, &expected_closed, NULL) == XR_XIR_OK);
    CHECK(expected_closed.bytes && expected_closed.bytes != input.bytes && expected_closed.length == input.length &&
        !memcmp(expected_closed.bytes, input.bytes, input.length));
    native_packet_die(&input);
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
    native_input_poison(identity, sizeof(identity)); proof = (XrXirProgramProof){0};
    XrXirProgram *program = seal_observed_native(context, &lowered, mode, entries, argv[4]);
    CHECK(!lowered && program && observed_native_owner);
    run_native_instances(&program, entries, mode);
    CHECK(!program && !closed && !lowered && !observed_native_owner && observed_code_lease_releases == 1);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("source-free-native-mixed normal-two-instances=2 independent-fixed42=4 actual-provider-deltas=4 input-poisoned-freed=1 caller-Program-dead=1 actual-code-lease-released=1");
    return 0;
}
#endif

int main(int argc, char **argv) {
#if defined(XR_SOURCE_STATIC_NATIVE_RUNNER)
    if (argc != 5) return 2;
    unsigned mode = !strcmp(argv[1], "1") ? 1u : !strcmp(argv[1], "2") ? 2u : !strcmp(argv[1], "3") ? 3u : 0u;
    if (!mode) return 2;
    return run_source_free_native(argv, mode);
#else
    if (argc != 8) return 2;
    return write_source_native(argv);
#endif
}
