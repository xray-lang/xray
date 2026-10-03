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
    static const uint8_t linked_policy[32]={0xdc,0xd3,0x4f,0x51,0x1c,0x94,0xa8,0xc1,0x2b,0x74,0x03,0x02,0x77,0x51,0xc5,0xc5,0x27,0x9e,0xc9,0x87,0x05,0xcb,0x89,0x2f,0xbc,0xd4,0x12,0x02,0xb9,0x07,0x71,0x14};
    static const uint8_t original_policy[32]={0xd5,0x61,0xc1,0xb3,0x5a,0x3f,0x4c,0x9d,0xd1,0x87,0x6b,0x0a,0xca,0x66,0xb1,0xc2,0xb3,0x46,0xe4,0x4a,0x4c,0x43,0x98,0x59,0x1d,0x3b,0x16,0xb0,0x7c,0x83,0xb9,0x1a};
    CHECK(!memcmp(input->codegen_policy_id.bytes,linked ? linked_policy : original_policy,32));
    uint8_t actual[32]; xr_sha256((const uint8_t *)source->text, source->length, actual);
    CHECK(!memcmp(actual, input->generated_digest.bytes, 32));
}
#endif // XIR_NATIVE_PROJECTION_EXPECTATIONS_H
