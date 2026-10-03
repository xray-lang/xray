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
    static const uint8_t linked_policy[32]={0xac,0x8f,0x4b,0x96,0x3d,0x1d,0x50,0xfc,0xd6,0x86,0xb8,0xd5,0x19,0xb9,0x52,0xce,0x1a,0x1b,0x9e,0xce,0x15,0x50,0xc5,0xe9,0x1b,0x7a,0x95,0xb1,0xce,0xfa,0xa8,0xb8};
    static const uint8_t original_policy[32]={0x2b,0xd4,0x72,0x1f,0x42,0x3d,0x0c,0x4f,0x57,0xe5,0x3e,0xaa,0x46,0x23,0x7d,0x88,0x95,0x69,0x29,0xcc,0xbf,0x78,0x83,0xce,0xe4,0x8f,0x88,0xc1,0x1c,0x9e,0xc4,0x45};
    CHECK(!memcmp(input->codegen_policy_id.bytes,linked ? linked_policy : original_policy,32));
    uint8_t actual[32]; xr_sha256((const uint8_t *)source->text, source->length, actual);
    CHECK(!memcmp(actual, input->generated_digest.bytes, 32));
}
#endif // XIR_NATIVE_PROJECTION_EXPECTATIONS_H
