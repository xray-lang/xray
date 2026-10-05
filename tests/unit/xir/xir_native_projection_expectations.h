/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_projection_expectations.h - Independent source and policy expectations
 */
#ifndef XIR_NATIVE_PROJECTION_EXPECTATIONS_H
#define XIR_NATIVE_PROJECTION_EXPECTATIONS_H
#include "aot/program/xr_xir_native_projection.h"
#include "base/xsha256.h"

static void projection_expectations_check(const XrXirSourceProduct *product,
    const XrXirNativeProjection *projection, bool linked) {
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
    const XrXirNativeProjectionFacts *input = xr_compile_native_projection_facts(projection);
    const XrXirNativeProjectionSource *source = xr_compile_native_projection_source(projection);
    CHECK(input && source && source->text && source->length);
    CHECK(input->source.target.architecture == facts->target.architecture && input->source.entry == facts->entry);
    CHECK(input->source.function_count == facts->function_count && input->source.module_count == facts->module_count);
    CHECK(!memcmp(input->source.source_digest, facts->source_digest, 32));
    CHECK(!memcmp(input->source.closed_digest, facts->closed_digest, 32));
    CHECK(!memcmp(input->source.lowered_layout_digest, facts->lowered_layout_digest, 32));
    CHECK(!strcmp(input->prefix, linked ? "linked_source" : "original_source"));
    CHECK(xr_compile_native_projection_context(projection)->resources == xr_xir_compile_source_product_context(product)->resources);
    static const uint8_t linked_policy[32]={0xb8,0xba,0x8f,0xb6,0x3e,0xa3,0xd8,0x38,0xab,0x97,0xa4,0x35,0x51,0x9b,0x55,0x16,0xf1,0x11,0x5c,0x91,0x3e,0xe1,0x51,0x7a,0x95,0x1b,0x57,0xda,0xfe,0x00,0x5c,0xd9};
    static const uint8_t original_policy[32]={0x75,0x1f,0xa4,0xa1,0xbc,0x2f,0x0d,0x27,0xf7,0xd3,0xe3,0x46,0x87,0x46,0xf4,0x3a,0x0c,0xf1,0x0f,0x94,0x23,0x92,0x57,0xef,0x2f,0xaa,0xd7,0x7b,0x3b,0x51,0x64,0xcd};
    CHECK(!memcmp(input->codegen_policy_id.bytes,linked ? linked_policy : original_policy,32));
    uint8_t actual[32]; xr_sha256((const uint8_t *)source->text, source->length, actual);
    CHECK(!memcmp(actual, input->generated_digest.bytes, 32));
}
#endif // XIR_NATIVE_PROJECTION_EXPECTATIONS_H
