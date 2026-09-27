/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_float_format_cases.h - Exact text and atomic typed-output expectations
 *
 * KEY CONCEPT:
 *   Shortestness and nearest candidate bytes are independent oracle inputs.
 */
#ifndef XIR_FLOAT_FORMAT_CASES_H
#define XIR_FLOAT_FORMAT_CASES_H
#include "shared/xr_decimal_float.h"
#include "xir/xxir_output.h"
typedef struct FloatOutputProbe { unsigned calls; bool accept; } FloatOutputProbe;
static bool float_output_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    const char expected[] = "0.1 0.1 -0.0 inf nan 1e+16\n";
    FloatOutputProbe *probe = context;
    CHECK(stream == XR_XIR_STDOUT && length == sizeof(expected) - 1 && !memcmp(bytes, expected, length));
    ++probe->calls;
    return probe->accept;
}
static void float_output_group(void) {
    XrXirValue values[] = {
        {XR_XIR_F32, 0, INT64_C(0x3dcccccd)}, {XR_XIR_F64, 0, INT64_C(0x3fb999999999999a)},
        {XR_XIR_F64, 0, INT64_MIN}, {XR_XIR_F32, 0, INT64_C(0x7f800000)},
        {XR_XIR_F64, 0, INT64_C(0x7ff8000000000000)}, {XR_XIR_F64, 0, INT64_C(0x4341c37937e08000)}
    };
    FloatOutputProbe probe = {0, true};
    XrXirOutputSink sink = {float_output_bytes, &probe, sizeof("0.1 0.1 -0.0 inf nan 1e+16\n") - 1};
    XrXirOutputGroup group = {XR_XIR_STDOUT, values, 6, true};
    CHECK(xr_xir_output_render(&sink, &group) && probe.calls == 1);
    --sink.byte_limit;
    CHECK(!xr_xir_output_render(&sink, &group) && probe.calls == 1);
    ++sink.byte_limit; values[4].payload = INT64_C(0x7ff0000000000001);
    CHECK(!xr_xir_output_render(&sink, &group) && probe.calls == 1);
    values[4].payload = INT64_C(0x7ff8000000000000); values[4].reserved = 1;
    CHECK(!xr_xir_output_render(&sink, &group) && probe.calls == 1);
    values[4].reserved = 0; probe.accept = false;
    CHECK(!xr_xir_output_render(&sink, &group) && probe.calls == 2);
}
static void float_format_cases(void) {
    static const struct { uint32_t width; uint64_t bits; const char *expected; } cases[] = {
#define XIR_FLOAT_FORMAT(width, bits, expected) {width, bits, expected},
#include "xir_float_format_vectors.def"
#undef XIR_FLOAT_FORMAT
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char text[32]; size_t length = 99;
        CHECK(xr_xir_float_format(cases[i].width, cases[i].bits, text, sizeof(text), &length));
        if (strcmp(text, cases[i].expected)) fprintf(stderr, "format vector %zu: %s != %s\n", i, text, cases[i].expected);
        CHECK(length == strlen(cases[i].expected) && !strcmp(text, cases[i].expected));
        uint64_t infinity = cases[i].width == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
        if ((cases[i].bits & infinity) != infinity) {
            uint64_t decoded = 0;
            CHECK(xr_decimal_float_parse(text, length, cases[i].width, &decoded) && decoded == cases[i].bits);
        }
        size_t exact = length + 1;
        CHECK(xr_xir_float_format(cases[i].width, cases[i].bits, text, exact, &length));
        CHECK(!xr_xir_float_format(cases[i].width, cases[i].bits, text, exact - 1, &length) && !length && !text[0]);
    }
    char text[32] = "untouched"; size_t length = 99;
    CHECK(!xr_xir_float_format(16, 0, text, sizeof(text), &length) && !length && !text[0]);
    CHECK(!xr_xir_float_format(32, UINT64_C(0x100000000), text, sizeof(text), &length));
    CHECK(!xr_xir_float_format(64, 0, text, 0, &length) && !length);
    CHECK(!xr_xir_float_format(64, 0, NULL, 32, &length) && !length);
    CHECK(!xr_xir_float_format(64, 0, text, sizeof(text), NULL) && !text[0]);
    float_output_group();
}
#endif // XIR_FLOAT_FORMAT_CASES_H
