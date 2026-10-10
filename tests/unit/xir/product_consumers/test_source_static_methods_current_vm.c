/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_static_methods_current_vm.c - Owned static method VM execution
 *
 * KEY CONCEPT:
 *   Source owners die before detached artifacts are consumed. Two Instances
 *   retain the Lowered lease after the caller drops its Program, and every
 *   exported scalar call keeps the original independent result.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
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

typedef struct StaticEntries { uint32_t entry, answer, private_answer; } StaticEntries;
static StaticEntries source_entries(const XrXirModule *module) {
    CHECK(module && module->declarations);
    const XrXirDeclarations *declarations = module->declarations;
    StaticEntries found = {declarations->entry_function, UINT32_MAX, UINT32_MAX};
    CHECK(found.entry < module->function_count);
    CHECK(!module->functions[found.entry].parameter_count && module->functions[found.entry].result == XR_XIR_I64);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &declarations->functions[f];
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
    return found;
}

/* Adapt only the existing scalar helper's normal public start/poll/take/drop. */
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

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    runtime_fail_at = SIZE_MAX;
    char root[2048], file[2048];
    CHECK(strlen(argv[1]) < sizeof(root) && strlen(argv[2]) < sizeof(file));
    strcpy(root, argv[1]); strcpy(file, argv[2]);
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{session, file, &authority, context, argv[3], NULL, XR_XIR_PROGRAM, NULL},
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
        XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
        CHECK(proof.bytes && proof.length && proof.identity && proof.layouts);
        XrXirProgram *program = NULL;
        /* The public take performs complete bind/proof/program_seal on this Lowered. */
        CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
        CHECK(!lowered && program);
        proof = (XrXirProgramProof){0}; /* End the caller's borrowed proof observation. */
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.value_limit = UINT64_C(1048576);
        XrXirInstance *instances[2] = {0};
        for (unsigned i = 0; i < 2; ++i)
            CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY && instances[i]);
        CHECK(instances[0] != instances[1]);
        xr_xir_compile_program_drop(program); program = NULL;
        /* Both live Instances retain the Program's real Lowered code lease. */
        for (unsigned i = 0; i < 2; ++i) {
            size_t attempts = runtime_attempts;
            CHECK(xr_xir_instance_start(instances[i], entries.private_answer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == attempts);
            execute_fixed_i64(instances[i], entries.entry, 0);
            for (unsigned repeat = 0; repeat < 2; ++repeat)
                execute_fixed_i64(instances[i], entries.answer, 42);
            /* READY is required only for this fully successful normal path. */
            CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY); instances[i] = NULL;
        }
        CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
        puts("static-methods-current-vm normal-two-instances=2 independent-fixed42=4 source-dead=1 source-Checked-dead=1 closed-Checked-dead=1 caller-Program-dead=1 actual-Lowered-code-lease-released=1");
    } else fprintf(stderr, "static-methods-current-vm Source required=0 actual=%u stage=%u message=%s\n",
        status, diagnostic.stage, diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("static-methods-current-vm required=0 actual=%u compiler-physical=0/0 runtime-physical=0/0 result=%s\n",
        status, status == XR_XIR_OK ? "PASS" : "FAIL");
    return status == XR_XIR_OK ? 0 : 1;
}
