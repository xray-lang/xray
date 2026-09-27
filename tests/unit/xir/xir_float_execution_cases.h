/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_float_execution_cases.h - Independent floating instruction expectations
 *
 * KEY CONCEPT:
 *   Each executor consumes literal bit witnesses and releases every frame.
 */
#ifndef XIR_FLOAT_EXECUTION_CASES_H
#define XIR_FLOAT_EXECUTION_CASES_H
#include "xir/xxir.h"
#include "xir/xxir_float.h"
#include <fenv.h>
typedef struct FloatInstructionCase {
    uint32_t id;
    XrXirType source;
    XrXirOp operation;
    XrXirType target;
    uint32_t count;
    int64_t immediate;
} FloatInstructionCase;
static const FloatInstructionCase float_functions[] = {
#define XIR_FLOAT_FUNCTION(id, type, op, result, count, immediate) {id, type, op, result, count, immediate},
#include "xir_float_functions.def"
#undef XIR_FLOAT_FUNCTION
};
static uint32_t floating_function(XrXirType source, XrXirType target, XrXirOp op) {
    for (size_t i = 0; i < sizeof(float_functions) / sizeof(float_functions[0]); ++i)
        if (float_functions[i].source == source && float_functions[i].target == target && float_functions[i].operation == op)
            return float_functions[i].id;
    CHECK(false); return 0;
}
static XrXirValue floating_argument(XrXirType type, uint64_t bits) {
    int64_t payload;
    memcpy(&payload, &bits, sizeof(bits));
    return (XrXirValue) {(uint32_t) type, 0, payload};
}
static bool floating_canonical(XrXirType type, uint64_t bits) {
    if (type != XR_XIR_F32 && type != XR_XIR_F64) return true;
    uint64_t magnitude = bits & (type == XR_XIR_F32 ? UINT64_C(0x7fffffff) : UINT64_C(0x7fffffffffffffff));
    uint64_t infinity = type == XR_XIR_F32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    uint64_t nan = type == XR_XIR_F32 ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
    return !(type == XR_XIR_F32 && bits > UINT32_MAX) && (magnitude <= infinity || bits == nan);
}
static void floating_conversions(FixtureRun run, void *owner) {
    static const struct { XrXirType source, target; uint64_t input, expected; XrXirRunStatus status; } cases[] = {
#define XIR_F2F(source, target, input, output) {XR_XIR_F##source, XR_XIR_F##target, input, output, XR_XIR_RUN_OK},
#define XIR_I2F(width, sign, target, input, output) {sign ? XR_XIR_I##width : XR_XIR_U##width, XR_XIR_F##target, input, output, XR_XIR_RUN_OK},
#define XIR_F2I(source, width, sign, input, status, output) {XR_XIR_F##source, sign ? XR_XIR_I##width : XR_XIR_U##width, input, output, status == 0 ? XR_XIR_RUN_OK : XR_XIR_RUN_NUMERIC_RANGE},
#include "xir_float_vectors.def"
#undef XIR_F2F
#undef XIR_I2F
#undef XIR_F2I
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirValue input = floating_argument(cases[i].source, cases[i].input), output = {99, 99, 99};
        XrXirRunContext context = {2, 16, 0, 0, 0, 0};
        XrXirRunStatus expected = floating_canonical(cases[i].source, cases[i].input) ? cases[i].status : XR_XIR_RUN_BAD_ARGUMENT;
        uint32_t id = floating_function(cases[i].source, cases[i].target, XR_XIR_CONVERT_NUMBER);
        CHECK(run(owner, id, &context, &input, 1, &output) == expected);
        CHECK(output.type == (uint32_t) (expected == XR_XIR_RUN_OK ? cases[i].target : XR_XIR_UNIT));
        CHECK((uint64_t) output.payload == (expected == XR_XIR_RUN_OK ? cases[i].expected : 0) && !output.reserved);
        CHECK(!context.live_bytes && context.allocations == context.frees);
    }
}
static void floating_relations(FixtureRun run, void *owner) {
    static const struct { uint64_t narrow, wide; int rank; } values[] = {
        {UINT64_C(0xff800000), UINT64_C(0xfff0000000000000), 0},
        {UINT64_C(0xbf800000), UINT64_C(0xbff0000000000000), 1},
        {UINT64_C(0x80000001), UINT64_C(0x8000000000000001), 2},
        {UINT64_C(0x80000000), UINT64_C(0x8000000000000000), 3},
        {0, 0, 3}, {1, 1, 4},
        {UINT64_C(0x3f800000), UINT64_C(0x3ff0000000000000), 5},
        {UINT64_C(0x7f800000), UINT64_C(0x7ff0000000000000), 6},
        {UINT64_C(0x7fc00000), UINT64_C(0x7ff8000000000000), -1}
    };
    const XrXirOp ops[] = {XR_XIR_EQ_FLOAT, XR_XIR_NE_FLOAT, XR_XIR_LT_FLOAT, XR_XIR_LE_FLOAT, XR_XIR_GT_FLOAT, XR_XIR_GE_FLOAT};
    for (unsigned width = 0; width < 2; ++width) for (unsigned a = 0; a < 9; ++a) {
        XrXirType type = width ? XR_XIR_F64 : XR_XIR_F32;
        XrXirValue input = floating_argument(type, width ? values[a].wide : values[a].narrow), output;
        XrXirRunContext context = {2, 16, 0, 0, 0, 0};
        CHECK(run(owner, floating_function(type, type, XR_XIR_NEG_FLOAT), &context, &input, 1, &output) == XR_XIR_RUN_OK);
        uint64_t expected = (uint64_t) input.payload;
        if (values[a].rank >= 0) expected ^= UINT64_C(1) << (width ? 63 : 31);
        CHECK(output.type == (uint32_t) type && (uint64_t) output.payload == expected && !output.reserved);
        CHECK(!context.live_bytes && context.allocations == context.frees);
        for (unsigned b = 0; b < 9; ++b) for (unsigned op = 0; op < 6; ++op) {
            bool unordered = values[a].rank < 0 || values[b].rank < 0;
            int left = values[a].rank, right = values[b].rank;
            const bool expected_relations[] = {!unordered && left == right, unordered || left != right,
                !unordered && left < right, !unordered && left <= right, !unordered && left > right, !unordered && left >= right};
            XrXirValue arguments[] = {input, floating_argument(type, width ? values[b].wide : values[b].narrow)};
            context = (XrXirRunContext) {2, 24, 0, 0, 0, 0};
            CHECK(run(owner, floating_function(type, XR_XIR_BOOL, ops[op]), &context, arguments, 2, &output) == XR_XIR_RUN_OK);
            CHECK(output.type == XR_XIR_BOOL && output.payload == expected_relations[op] && !output.reserved);
            CHECK(!context.live_bytes && context.allocations == context.frees);
        }
    }
}
static void floating_arithmetic_execution(FixtureRun run, void *owner) {
    static const struct { XrXirType type; XrXirFloatOperation operation; uint64_t a, b, expected; } cases[] = {
#define XIR_FLOAT_ARITHMETIC(width, operation, a, b, result) {XR_XIR_F##width, operation, a, b, result},
#include "xir_float_arithmetic_vectors.def"
#undef XIR_FLOAT_ARITHMETIC
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirType type = cases[i].type;
        XrXirValue inputs[] = {floating_argument(type, cases[i].a), floating_argument(type, cases[i].b)};
        XrXirValue output = {99, 99, 99};
        XrXirRunContext context = {2, 24, 0, 0, 0, 0};
        XrXirRunStatus expected = floating_canonical(type, cases[i].a) && floating_canonical(type, cases[i].b) ?
            XR_XIR_RUN_OK : XR_XIR_RUN_BAD_ARGUMENT;
        XrXirOp op = (XrXirOp) (XR_XIR_ADD_FLOAT + cases[i].operation);
        CHECK(run(owner, floating_function(type, type, op), &context, inputs, 2, &output) == expected);
        CHECK(output.type == (uint32_t) (expected == XR_XIR_RUN_OK ? type : XR_XIR_UNIT));
        CHECK((uint64_t) output.payload == (expected == XR_XIR_RUN_OK ? cases[i].expected : 0) && !output.reserved);
        CHECK(!context.live_bytes && context.allocations == context.frees);
    }
}
static void floating_execution_cases(FixtureRun run, void *owner) {
    fenv_t saved; CHECK(fegetenv(&saved) == 0);
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    for (unsigned mode = 0; mode < 4; ++mode) {
        CHECK(fesetround(modes[mode]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 && feraiseexcept(FE_INEXACT) == 0);
        int flags = fetestexcept(FE_ALL_EXCEPT);
        floating_conversions(run, owner); floating_relations(run, owner);
        floating_arithmetic_execution(run, owner);
        CHECK(fegetround() == modes[mode] && fetestexcept(FE_ALL_EXCEPT) == flags);
    }
    CHECK(fesetenv(&saved) == 0);
    for (unsigned width = 0; width < 2; ++width) {
        XrXirType type = width ? XR_XIR_F64 : XR_XIR_F32;
        XrXirRunContext context = {2, 8, 0, 0, 0, 0}; XrXirValue output;
        CHECK(run(owner, floating_function(type, type, XR_XIR_CONST_FLOAT), &context, NULL, 0, &output) == XR_XIR_RUN_OK);
        CHECK(output.type == (uint32_t) type && (uint64_t) output.payload == (UINT64_C(1) << (width ? 63 : 31)));
        CHECK(!context.live_bytes && context.allocations == context.frees);
    }
    puts("Floating XIR execution: 784 conversions, 972 relations, 18 negations in four rounding modes; 2 constants passed");
}
#endif // XIR_FLOAT_EXECUTION_CASES_H
