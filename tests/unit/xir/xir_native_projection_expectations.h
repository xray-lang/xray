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
    static const uint8_t linked_policy[32]={0xcb,0xd7,0xa8,0xa4,0x87,0x89,0x4b,0x5c,0x4f,0x77,0x9e,0x3c,0x2a,0x58,0x35,0x1f,0x60,0x7f,0x54,0x1a,0xfb,0x65,0x68,0x0d,0x98,0xea,0x0f,0x18,0x28,0x97,0xb3,0x22};
    static const uint8_t original_policy[32]={0x52,0xde,0x1a,0x1c,0x46,0x62,0x17,0x7a,0xbd,0xb3,0x5d,0xc0,0x0f,0x5b,0xb3,0xc1,0x7b,0x16,0x8f,0x63,0x9a,0x6f,0x78,0xfa,0x7f,0x18,0x64,0x63,0x92,0x08,0x12,0xfb};
    CHECK(!memcmp(input->codegen_policy_id.bytes,linked ? linked_policy : original_policy,32));
    uint8_t actual[32]; xr_sha256((const uint8_t *)source->text, source->length, actual);
    CHECK(!memcmp(actual, input->generated_digest.bytes, 32));
}
#endif // XIR_NATIVE_PROJECTION_EXPECTATIONS_H
