/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_projection_fixture.h - Explicit identity fixtures without target authority
 */
#ifndef XIR_NATIVE_PROJECTION_FIXTURE_H
#define XIR_NATIVE_PROJECTION_FIXTURE_H
#include "aot/program/xr_xir_native_projection.h"
#include "base/xsha256.h"

static XrToolchainBinding projection_fixture_binding(const XrXirSourceProduct *product) {
    const XrXirCompileContext *context = xr_xir_compile_source_product_context(product);
    CHECK(context && context->resources);
    XrToolchainInput input = {XR_TOOLCHAIN_BINDING_SCHEMA_VERSION, XR_TOOLCHAIN_BINDING_PROVIDER_MSVC,
        {0}, "fixture-cl", "fixture-windows-x86_64", "fixture-opt=2", {{1}}, {{2}}, {{3}}};
    XrToolchainBinding binding;
    CHECK(xr_compile_toolchain_binding_build(context->resources, &input, &binding) == XR_TOOLCHAIN_BINDING_OK);
    return binding;
}
static void projection_fixture_check(const XrXirSourceProduct *product,
    const XrXirNativeProjection *projection, bool linked) {
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
    const XrXirNativeInput *input = &projection->input;
    CHECK(input->schema_version == 2 && input->checked_schema == 22 && input->checked_contract == 58);
    CHECK(input->value_abi == 18 && input->call_abi == 22 && input->program_abi == 27);
    CHECK(input->architecture == facts->target.architecture && input->entry == facts->entry);
    CHECK(input->function_count == facts->function_count && input->module_count == facts->module_count);
    CHECK(!memcmp(input->source_checked_id.bytes, facts->source_digest, 32));
    CHECK(!memcmp(input->closed_checked_id.bytes, facts->closed_digest, 32));
    CHECK(!memcmp(input->lowered_layout_id.bytes, facts->lowered_layout_digest, 32));
    static const uint8_t linked_policy[32]={0xe9,0x0d,0xc9,0x8b,0xd5,0xa1,0x4d,0x9f,0x23,0x5f,0x77,0x66,0xd1,0x9a,0x6a,0xd1,0x38,0xe1,0xdf,0x38,0x3b,0xf2,0x12,0x9c,0x6d,0xab,0x39,0x24,0x9c,0x1c,0x90,0x72};
    static const uint8_t original_policy[32]={0x6b,0xa6,0x5c,0x37,0x19,0x38,0xdf,0x34,0x54,0xcf,0x94,0x53,0x97,0x13,0x1a,0x65,0x4d,0xa0,0xfb,0x7c,0xd7,0x38,0xa6,0x27,0x53,0x5f,0xe0,0xc9,0xdc,0xf4,0x01,0xa0};
    CHECK(!memcmp(input->codegen_policy_id.bytes,linked ? linked_policy : original_policy,32));
    uint8_t actual[32]; xr_sha256((const uint8_t *)projection->source.text, projection->source.length, actual);
    CHECK(!memcmp(actual, input->generated_digest.bytes, 32));
}
#endif // XIR_NATIVE_PROJECTION_FIXTURE_H
