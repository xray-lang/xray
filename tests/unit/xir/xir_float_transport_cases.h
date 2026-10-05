/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_float_transport_cases.h - Independent COPY payload witnesses
 *
 * KEY CONCEPT:
 *   Raw integer witnesses preserve IEEE bits without host floating comparison.
 */
#ifndef XIR_FLOAT_TRANSPORT_CASES_H
#define XIR_FLOAT_TRANSPORT_CASES_H
#include <fenv.h>
#include <string.h>
static void float_transport_cases(FixtureRun run, void *owner) {
    static const struct { XrXirType type; uint64_t bits; bool admitted; } cases[] = {
        {XR_XIR_F64, UINT64_C(0x7ff0000000000001), true},
        {XR_XIR_F64, UINT64_C(0xfff0000000000001), true},
        {XR_XIR_F64, UINT64_C(0x7ff8deadbeef1234), true},
        {XR_XIR_F64, UINT64_C(0xffffffffffffffff), true},
        {XR_XIR_F64, UINT64_C(0x8000000000000000), true},
        {XR_XIR_F64, UINT64_C(0x8000000000000001), true},
        {XR_XIR_F64, UINT64_C(0x7ff0000000000000), true},
        {XR_XIR_F64, UINT64_C(0xfff0000000000000), true},
        {XR_XIR_F64, UINT64_C(0x3ff0000000000000), true},
        {XR_XIR_F64, UINT64_C(0), true},
        {XR_XIR_F32, UINT64_C(0x7fc00000), true},
        {XR_XIR_F32, UINT64_C(0x80000000), true},
        {XR_XIR_F32, UINT64_C(1), true},
        {XR_XIR_F32, UINT64_C(0x7f800000), true},
        {XR_XIR_F32, UINT64_C(0x100000000), false},
        {XR_XIR_F32, UINT64_C(0xffffffff80000000), false},
        {XR_XIR_F32, UINT64_C(0x7f800001), false},
        {XR_XIR_F32, UINT64_C(0xffc00000), false}
    };
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    fenv_t saved;
    CHECK(fegetenv(&saved) == 0);
    for (unsigned mode = 0; mode < 4; ++mode) {
        CHECK(fesetround(modes[mode]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0);
        CHECK(feraiseexcept(FE_INEXACT) == 0);
        int flags = fetestexcept(FE_ALL_EXCEPT);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            int64_t payload;
            memcpy(&payload, &cases[i].bits, sizeof(payload));
            XrXirValue input = {(uint32_t) cases[i].type, 0, payload}, output = {99, 99, 99};
            XrXirRunContext context = {2, 16, 0, 0, 0, 0};
            XrXirRunStatus expected = cases[i].admitted ? XR_XIR_RUN_OK : XR_XIR_RUN_BAD_ARGUMENT;
            CHECK(run(owner, cases[i].type == XR_XIR_F64 ? 0 : 1,
                &context, &input, 1, &output) == expected);
            CHECK(output.type == (uint32_t) (cases[i].admitted ? cases[i].type : XR_XIR_UNIT));
            CHECK((uint64_t) output.payload == (cases[i].admitted ? cases[i].bits : 0));
            CHECK(!output.reserved && !context.live_bytes && context.allocations == context.frees);
            CHECK(fegetround() == modes[mode] && fetestexcept(FE_ALL_EXCEPT) == flags);
        }
    }
    CHECK(fesetenv(&saved) == 0);
    puts("Floating COPY transport: 14 valid bit witnesses and 4 F32 rejections in four rounding modes passed");
}
#endif // XIR_FLOAT_TRANSPORT_CASES_H
