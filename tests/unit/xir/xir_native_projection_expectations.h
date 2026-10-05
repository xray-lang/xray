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
    static const uint8_t linked_policy[32]={0xcf,0x28,0x65,0x18,0x3e,0x02,0x1d,0xfc,0x21,0xf2,0x4e,0xa3,0xb0,0xcb,0x15,0x1e,0xf1,0x64,0x7f,0x0e,0x3e,0x69,0x4b,0xd6,0x7f,0x81,0x17,0xba,0xaf,0x8a,0xfe,0xa2};
    static const uint8_t original_policy[32]={0x7e,0x5c,0xbf,0x69,0x30,0x07,0xfc,0xa0,0x2b,0x8e,0xcb,0x5a,0xf0,0xbf,0x64,0xff,0xb2,0x9b,0x56,0xe1,0x23,0x69,0xeb,0x4e,0x5a,0x4e,0xd2,0xd8,0x0a,0x1d,0x5c,0xcf};
    CHECK(!memcmp(input->codegen_policy_id.bytes,linked ? linked_policy : original_policy,32));
    uint8_t actual[32]; xr_sha256((const uint8_t *)source->text, source->length, actual);
    CHECK(!memcmp(actual, input->generated_digest.bytes, 32));
}
#endif // XIR_NATIVE_PROJECTION_EXPECTATIONS_H
