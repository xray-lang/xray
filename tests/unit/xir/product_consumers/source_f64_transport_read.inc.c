/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_f64_transport_read.inc.c - Detached SourceProduct binary64 carrier
 *
 * KEY CONCEPT:
 *   Source and closed Checked packets retain typed exported entry authority.
 *   The source owner and session die before detached lowering and execution.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
static const char *transport_source_root, *transport_source_file;
static const char transport_source[] =
    "export fn caller(value: f64) -> f64 { return carrier(value) }\n"
    "fn carrier(value: f64) -> f64 { var saved: f64 = value; return saved }\n";

static void source_roles(const XrXirArtifact *artifact, uint32_t *caller, uint32_t *carrier) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    CHECK(module && module->function_count == 4 && module->declarations);
    const XrXirDeclarations *d = module->declarations;
    CHECK(d->module_count == 1 && d->root_module == 0);
    *caller = UINT32_MAX; *carrier = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        bool first = fn->name_length == 6 && !memcmp(fn->name, "caller", 6);
        bool second = fn->name_length == 7 && !memcmp(fn->name, "carrier", 7);
        if (!first && !second) continue;
        CHECK(fn->parameter_count == 1 && fn->parameters[0] == XR_XIR_F64 && fn->result == XR_XIR_F64);
        CHECK(d->functions[f].exported == (first ? 1u : 0u) && !d->functions[f].test_role);
        CHECK(f != d->entry_function && f != d->modules[0].initializer);
        uint32_t *id = first ? caller : carrier; CHECK(*id == UINT32_MAX); *id = f;
    }
    CHECK(*caller != UINT32_MAX && *carrier != UINT32_MAX && *caller != *carrier);
    const XrXirFunction *fn = &module->functions[*caller]; unsigned calls = 0;
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        if (op->op == XR_XIR_CALL) { CHECK(op->immediate == *carrier && op->type == XR_XIR_F64); ++calls; }
    }
    CHECK(calls == 1);
    CHECK(module->functions[d->entry_function].result == XR_XIR_I64);
    CHECK(module->functions[d->modules[0].initializer].result == XR_XIR_UNIT);
    printf("source-f64 roles stage=%u functions=4 caller=%u carrier=%u entry=%u initializer=%u exported=1/private=1 call-edge=1\n",
        module->stage, *caller, *carrier, d->entry_function, d->modules[0].initializer);
}

static XrXirArtifact *read_lower(const XrXirCompileContext *context) {
    XrOsIoPolicy policy = xr_compile_io_policy(context->resources);
    uint8_t *bytes = NULL; size_t length = 0;
    CHECK(xr_os_io_read_regular_file(&policy, transport_source_file, sizeof(transport_source)-1,
        &bytes, &length) == XR_OS_IO_OK);
    CHECK(length == sizeof(transport_source)-1 && !memcmp(bytes, transport_source, length));
    xr_compile_resources_free(bytes);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, transport_source_root};
    XrXirSourceProductRequest request = {{session, transport_source_file, &authority, context,
        NULL, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    printf("source-f64 admission status=%u stage=%u source-status=%u line=%d column=%d message=%s\n",
        status, diagnostic.stage, diagnostic.source.status, diagnostic.source.line,
        diagnostic.source.column, diagnostic.source.message);
    CHECK(status == XR_XIR_OK && product);
    CHECK(xr_xir_compile_source_product_verify(product, 1048576, NULL) == XR_XIR_OK);
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
    CHECK(facts && facts->function_count == 4 && facts->module_count == 1);
    XrXirArtifact *packets[2] = {NULL, NULL}; uint32_t callers[2], carriers[2];
    for (unsigned k = 0; k < 2; ++k) {
        XrXirSourceProductPacketView view = {0}; XrXirCheckedPacket roundtrip = {0};
        CHECK(xr_xir_compile_source_product_packet(product, k ? XR_XIR_SOURCE_PRODUCT_CLOSED :
            XR_XIR_SOURCE_PRODUCT_SOURCE, &view) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context, view.bytes, view.length, &packets[k], NULL) == XR_XIR_OK);
        source_roles(packets[k], &callers[k], &carriers[k]);
        CHECK(xr_xir_compile_checked_write(packets[k], &roundtrip, NULL) == XR_XIR_OK);
        CHECK(roundtrip.bytes != view.bytes && roundtrip.length == view.length &&
            !memcmp(roundtrip.bytes, view.bytes, view.length));
        xr_xir_compile_checked_packet_free(&roundtrip);
    }
    CHECK(callers[0] == callers[1] && carriers[0] == carriers[1]);
    xr_compile_session_free(session);
    xr_xir_compile_source_product_free(product);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    for (unsigned k = 0; k < 2; ++k) CHECK(xr_xir_compile_artifact_verify(packets[k], NULL) == XR_XIR_OK);
    XrXirArtifact *lowered = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(packets[1], &target, &lowered, NULL) == XR_XIR_OK);
    source_roles(lowered, &caller_entry, &carrier_entry);
    CHECK(caller_entry == callers[1] && carrier_entry == carriers[1]);
    for (unsigned k = 0; k < 2; ++k) xr_xir_compile_artifact_free(packets[k]);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    puts("source-f64 source-packet=exact closed-packet=exact source-owner=destroyed session=destroyed detached-Lowered=verified");
    return lowered;
}
