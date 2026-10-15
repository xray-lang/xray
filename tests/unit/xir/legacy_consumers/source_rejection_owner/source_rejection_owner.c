/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_rejection_owner.c - Controlled Task result admission through real owners
 *
 * KEY CONCEPT:
 *   A rejected Task result needs successful same-class and scalar worker controls.
 *   Type proofs use an authentic SOURCE packet after its original product dies.
 */
#include "api/xsource_compile.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <string.h>

typedef struct CaseEvidence {
    unsigned status, public_stage, check_stage, check_status, source_status;
    unsigned storage, sendable, function_count;
    bool product_published, packet_owned, producer_dropped, result_i64, result_class, qualified;
} CaseEvidence;

static bool class_result(const XrXirModule *module, XrXirType type) {
    if (!module || !module->types || !module->types->nominals) return false;
    const XrXirTypeNode *node = xr_xir_type_node(module->types, type);
    if (!node || node->kind != XR_XIR_TYPE_NOMINAL ||
        node->nominal.declaration >= module->types->nominals->count) return false;
    const XrXirNominalDeclaration *declaration =
        &module->types->nominals->declarations[node->nominal.declaration];
    return declaration->kind == XR_XIR_NOMINAL_CLASS && declaration->parameter_count == 0 &&
        declaration->field_count == 1 && declaration->fields && declaration->fields[0].type == XR_XIR_I64 &&
        node->nominal.field_count == 1 && node->nominal.fields && node->nominal.fields[0] == XR_XIR_I64;
}

static bool result_proof(const XrXirCompileContext *context, XrXirArtifact *owned,
    unsigned ordinal, CaseEvidence *evidence) {
    const XrXirModule *module = xr_xir_compile_artifact_module(owned);
    if (!module) return false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != 8 || !function->name || memcmp(function->name, "buildBox", 8)) continue;
        ++evidence->function_count;
        evidence->result_i64 = function->result == XR_XIR_I64;
        evidence->result_class = class_result(module, function->result);
        XrXirProofContext proof = {module, {XR_XIR_CONTEXT_FUNCTION, f, 0}};
        evidence->storage = (unsigned)xr_xir_compile_type_storage_prove(context, &proof, function->result);
        evidence->sendable = (unsigned)xr_xir_compile_type_markers_prove(context, &proof,
            function->result, XR_XIR_CONSTRAINT_SENDABLE);
    }
    return evidence->function_count == 1 && evidence->storage == XR_XIR_OK &&
        (ordinal == 1 ? evidence->result_class && !evidence->result_i64 && evidence->sendable == XR_XIR_BAD_TYPE :
         ordinal == 2 && evidence->result_i64 && !evidence->result_class && evidence->sendable == XR_XIR_OK);
}

static bool positive_owner(const XrXirCompileContext *context, XrXirSourceProduct **product,
    unsigned ordinal, CaseEvidence *evidence) {
    XrXirArtifact *owned = NULL;
    XrXirSourceProductPacketView packet = {0};
    XrXirSourceDiagnostic admission = {0};
    bool ok = product && *product &&
        xr_xir_compile_source_product_verify(*product, 16777216, NULL) == XR_XIR_OK &&
        xr_xir_compile_source_product_public_admit(*product, &admission) == XR_XIR_OK &&
        xr_xir_compile_source_product_packet(*product, XR_XIR_SOURCE_PRODUCT_SOURCE, &packet) == XR_XIR_OK &&
        xr_xir_compile_checked_read(context, packet.bytes, packet.length, &owned, NULL) == XR_XIR_OK;
    evidence->packet_owned = ok && owned != NULL;
    xr_xir_compile_source_product_free(*product); *product = NULL;
    evidence->producer_dropped = true;
    if (ok) ok = result_proof(context, owned, ordinal, evidence);
    xr_xir_compile_artifact_free(owned);
    return ok;
}

static bool source_case(const XrXirCompileContext *context, const char *path,
    const char *stdlib, unsigned ordinal, CaseEvidence *evidence) {
    XrSourceCompileRequest request = {0};
    request.context = context; request.absolute_entry_path = path; request.absolute_stdlib_path = stdlib;
    request.manifest_limits = xr_source_compile_default_manifest_limits();
    request.target = (XrXirTarget){XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrSourceCompileDiagnostic diagnostic = {0};
    XrXirSourceProduct *product = NULL;
    XrSourceCompileStatus status = xr_source_compile_build(&request, &product, &diagnostic);
    evidence->status = (unsigned)status; evidence->public_stage = (unsigned)diagnostic.stage;
    evidence->check_stage = (unsigned)diagnostic.source.stage;
    evidence->check_status = (unsigned)diagnostic.source.status;
    evidence->source_status = (unsigned)diagnostic.source.source.status;
    evidence->product_published = product != NULL;
    bool ok = ordinal == 0 ? status == XR_SOURCE_COMPILE_REJECTED && !product &&
        diagnostic.stage == XR_SOURCE_COMPILE_PRODUCT && diagnostic.source.stage == XR_XIR_SOURCE_PRODUCT_CHECK &&
        diagnostic.source.status == XR_XIR_BAD_TYPE && diagnostic.source.source.status == XR_XIR_BAD_TYPE :
        status == XR_SOURCE_COMPILE_OK && positive_owner(context, &product, ordinal, evidence);
    xr_xir_compile_source_product_free(product);
    xr_source_compile_diagnostic_free(&diagnostic);
    evidence->qualified = ok;
    return ok;
}

static const char *boolean(bool value) { return value ? "true" : "false"; }

static void report_case(const CaseEvidence *evidence, unsigned ordinal) {
    printf("%s{\"ordinal\":%u,\"status\":%u,\"public_stage\":%u,\"check_stage\":%u,"
        "\"check_status\":%u,\"source_status\":%u,\"product_published\":%s,"
        "\"packet_owned\":%s,\"producer_dropped\":%s,\"result_i64\":%s,\"result_class\":%s,"
        "\"function_count\":%u,\"storage\":%u,\"sendable\":%u,\"qualified\":%s}",
        ordinal ? "," : "", ordinal, evidence->status, evidence->public_stage, evidence->check_stage,
        evidence->check_status, evidence->source_status, boolean(evidence->product_published),
        boolean(evidence->packet_owned), boolean(evidence->producer_dropped), boolean(evidence->result_i64),
        boolean(evidence->result_class), evidence->function_count, evidence->storage, evidence->sendable,
        boolean(evidence->qualified));
}

int main(int argc, char **argv) {
    if (argc != 5) { fputs("Expected negative, direct-class, scalar-worker and stdlib paths.\n", stderr); return 2; }
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
    if (xr_compile_resources_new(&limits, &context.resources) != XR_COMPILE_RESOURCE_OK) return 2;
    XrCompileResourceStats before = {0}, after = {0};
    bool ok = xr_compile_resources_stats(context.resources, &before) == XR_COMPILE_RESOURCE_OK;
    CaseEvidence cases[3] = {0};
    if (ok) for (unsigned i = 0; i < 3; ++i) {
        bool qualified = source_case(&context, argv[i + 1], argv[4], i, &cases[i]);
        ok = qualified && ok;
    }
    bool measured = xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK;
    ok = ok && measured && after.live_bytes == before.live_bytes;
    printf("{\"schema\":1,\"scope\":\"CONTROLLED_CHECKED_TASK_RESULT\",\"qualified\":%s,"
        "\"checked_schema\":%u,\"semantic_identity\":%u,\"value_abi\":%u,\"cases\":[",
        boolean(ok), XR_XIR_CHECKED_SCHEMA, XR_XIR_CHECKED_CONTRACT, XR_XIR_VALUE_ABI_VERSION);
    for (unsigned i = 0; i < 3; ++i) report_case(&cases[i], i);
    printf("],\"ledger_baseline\":%llu,\"ledger_final\":%llu,\"physical_zero_verified\":false}\n",
        (unsigned long long)before.live_bytes, (unsigned long long)after.live_bytes);
    xr_compile_resources_release(context.resources);
    return ok ? 0 : 1;
}
