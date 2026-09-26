/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_execution_cases.h - Scalar execution verification
 *
 * KEY CONCEPT:
 *   Literal semantic expectations are run independently against each executor.
 */

#ifndef XIR_EXECUTION_CASES_H
#define XIR_EXECUTION_CASES_H
#include "xir/xxir_scalar.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef CHECK
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#endif
typedef XrXirRunStatus (*FixtureRun)(void *, uint32_t, XrXirRunContext *,
                                    const XrXirValue *, uint32_t, XrXirValue *);

static void numeric_cases(FixtureRun run, void *owner) {
    struct NumericCase { uint32_t id; int64_t left, right, expected; XrXirRunStatus status; };
    const struct NumericCase cases[] = {
        {9, 7, 3, 4, XR_XIR_RUN_OK}, {9, INT64_MIN, 1, INT64_MAX, XR_XIR_RUN_OK},
        {9, INT64_MAX, -1, INT64_MIN, XR_XIR_RUN_OK}, {9, 0, INT64_MIN, INT64_MIN, XR_XIR_RUN_OK},
        {10, -7, 3, -21, XR_XIR_RUN_OK}, {10, INT64_MAX, 2, -2, XR_XIR_RUN_OK},
        {10, INT64_MIN, -1, INT64_MIN, XR_XIR_RUN_OK}, {10, INT64_MIN, 0, 0, XR_XIR_RUN_OK},
        {10, INT64_MIN, INT64_MIN, 0, XR_XIR_RUN_OK},
        {11, 7, 3, 2, XR_XIR_RUN_OK}, {11, -7, 3, -2, XR_XIR_RUN_OK},
        {11, 7, -3, -2, XR_XIR_RUN_OK}, {11, -7, -3, 2, XR_XIR_RUN_OK},
        {11, INT64_MIN, -1, INT64_MIN, XR_XIR_RUN_OK}, {11, INT64_MIN, 1, INT64_MIN, XR_XIR_RUN_OK},
        {11, INT64_MAX, INT64_MIN, 0, XR_XIR_RUN_OK}, {11, 7, 0, 0, XR_XIR_RUN_DIVIDE_BY_ZERO},
        {12, 7, 3, 1, XR_XIR_RUN_OK}, {12, -7, 3, -1, XR_XIR_RUN_OK},
        {12, 7, -3, 1, XR_XIR_RUN_OK}, {12, -7, -3, -1, XR_XIR_RUN_OK},
        {12, INT64_MIN, -1, 0, XR_XIR_RUN_OK}, {12, INT64_MIN, INT64_MIN, 0, XR_XIR_RUN_OK},
        {12, INT64_MAX, INT64_MIN, INT64_MAX, XR_XIR_RUN_OK}, {12, 7, 0, 0, XR_XIR_RUN_DIVIDE_BY_ZERO}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirRunContext context = {2, 24, 0, 0, 0, 0};
        XrXirValue args[] = {{XR_XIR_I64, 0, cases[i].left}, {XR_XIR_I64, 0, cases[i].right}}, result;
        CHECK(run(owner, cases[i].id, &context, args, 2, &result) == cases[i].status);
        CHECK(result.type == (uint32_t) (cases[i].status == XR_XIR_RUN_OK ? XR_XIR_I64 : XR_XIR_UNIT));
        CHECK(result.payload == cases[i].expected && result.reserved == 0);
        CHECK(context.steps == (cases[i].status == XR_XIR_RUN_OK ? 0u : 1u));
        CHECK(context.live_bytes == 0 && context.allocations == 1 && context.frees == 1);
    }
    const int64_t values[] = {INT64_MIN, -1, 0, 1, INT64_MAX};
    for (uint32_t id = 13; id <= 16; ++id) for (unsigned i = 0; i < 5; ++i) for (unsigned j = 0; j < 5; ++j) {
        XrXirRunContext context = {2, 24, 0, 0, 0, 0};
        XrXirValue args[] = {{XR_XIR_I64, 0, values[i]}, {XR_XIR_I64, 0, values[j]}}, result;
        bool expected = id == 13 ? i != j : id == 14 ? i <= j : id == 15 ? i > j : i >= j;
        CHECK(run(owner, id, &context, args, 2, &result) == XR_XIR_RUN_OK);
        CHECK(result.type == XR_XIR_BOOL && result.payload == expected && result.reserved == 0);
        CHECK(context.live_bytes == 0 && context.allocations == context.frees);
    }
}

static void execution_cases(FixtureRun run, void *owner) {
    numeric_cases(run, owner);
    struct AddCase { int64_t branch, left, right, expected; XrXirRunStatus status; };
    const struct AddCase cases[] = {
        {1, 40, 2, 42, XR_XIR_RUN_OK}, {1, -7, 2, -5, XR_XIR_RUN_OK},
        {1, INT64_MAX, 0, INT64_MAX, XR_XIR_RUN_OK},
        {1, INT64_MIN, 0, INT64_MIN, XR_XIR_RUN_OK},
        {1, INT64_MIN, INT64_MAX, -1, XR_XIR_RUN_OK},
        {1, INT64_MAX, 1, INT64_MIN, XR_XIR_RUN_OK},
        {1, INT64_MIN, -1, INT64_MAX, XR_XIR_RUN_OK},
        {0, INT64_MAX, 1, -7, XR_XIR_RUN_OK}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirValue args[] = {{XR_XIR_BOOL, 0, cases[i].branch},
            {XR_XIR_I64, 0, cases[i].left}, {XR_XIR_I64, 0, cases[i].right}};
        XrXirRunContext context = {10, 48, 0, 0, 0, 0};
        XrXirValue result = {99, 99, 99};
        CHECK(run(owner, 0, &context, args, 3, &result) == cases[i].status);
        CHECK(result.type == (uint32_t) (cases[i].status == XR_XIR_RUN_OK ? XR_XIR_I64 : XR_XIR_UNIT));
        CHECK(result.reserved == 0 && result.payload == cases[i].expected);
        CHECK(context.live_bytes == 0 && context.peak_bytes == 48);
        CHECK(context.allocations == 1 && context.frees == 1);
        CHECK(context.steps == (cases[i].branch ? 6u : 7u));
    }
    XrXirValue args[] = {{XR_XIR_BOOL, 0, 1}, {XR_XIR_I64, 0, 40}, {XR_XIR_I64, 0, 2}};
    XrXirRunContext reused = {12, 48, 0, 0, 0, 0};
    XrXirValue reused_result = {99, 99, 99};
    CHECK(run(owner, 0, NULL, args, 3, &reused_result) == XR_XIR_RUN_BAD_ARGUMENT);
    CHECK(reused_result.type == 0 && reused_result.payload == 0);
    CHECK(run(owner, 0, &reused, args, 3, NULL) == XR_XIR_RUN_BAD_ARGUMENT);
    CHECK(reused.steps == 12 && reused.allocations == 0);
    for (uint64_t repeat = 1; repeat <= 3; ++repeat) {
        CHECK(run(owner, 0, &reused, args, 3, &reused_result) == XR_XIR_RUN_OK);
        CHECK(reused_result.type == XR_XIR_I64 && reused_result.payload == 42);
        CHECK(reused.steps == 12 - 4 * repeat && reused.live_bytes == 0);
        CHECK(reused.allocations == repeat && reused.frees == repeat && reused.peak_bytes == 48);
    }
    for (uint32_t steps = 0; steps < 4; ++steps) {
        XrXirRunContext context = {steps, 48, 0, 0, 0, 0};
        XrXirValue result = {99, 99, 99};
        CHECK(run(owner, 0, &context, args, 3, &result) == XR_XIR_RUN_STEP_LIMIT);
        CHECK(result.type == 0 && result.reserved == 0 && result.payload == 0);
        CHECK(context.steps == 0 && context.live_bytes == 0 && context.allocations == context.frees);
    }
    for (uint32_t bad = 0; bad < 8; ++bad) {
        XrXirValue invalid[] = {{XR_XIR_BOOL, 0, 1}, {XR_XIR_I64, 0, 40}, {XR_XIR_I64, 0, 2}};
        XrXirRunContext context = {10, 48, 0, 0, 0, 0};
        XrXirValue result = {99, 99, 99};
        if (bad == 2) invalid[0].payload = 2;
        if (bad == 3) invalid[1].type = XR_XIR_BOOL;
        if (bad == 4) invalid[2].reserved = 1;
        if (bad == 5) context.frame_limit = 47;
        if (bad == 6) context.frees = 1;
        if (bad == 7) context.live_bytes = 1;
        CHECK(run(owner, 0, &context, bad == 0 ? NULL : invalid, bad == 1 ? 2 : 3, &result) ==
            (bad == 5 ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARGUMENT));
        CHECK(result.type == 0 && result.reserved == 0 && result.payload == 0);
        CHECK(context.steps == 10 && context.allocations == 0);
        CHECK(context.live_bytes == (bad == 7 ? 1u : 0u));
    }
    const int64_t equal_values[] = {0, -1, INT64_MIN, INT64_MAX};
    for (size_t i = 0; i < 4; ++i) for (size_t j = 0; j < 4; ++j) {
        XrXirValue pair[] = {{XR_XIR_I64, 0, equal_values[i]}, {XR_XIR_I64, 0, equal_values[j]}};
        XrXirRunContext context = {2, 24, 0, 0, 0, 0};
        XrXirValue result;
        CHECK(run(owner, 1, &context, pair, 2, &result) == XR_XIR_RUN_OK);
        CHECK(result.type == XR_XIR_BOOL && result.payload == (i == j));
        CHECK(context.steps == 0 && context.live_bytes == 0 && context.frees == 1);
    }
    for (int64_t value = 0; value <= 1; ++value) {
        XrXirValue argument = {XR_XIR_BOOL, 0, value}, result;
        XrXirRunContext context = {2, 16, 0, 0, 0, 0};
        CHECK(run(owner, 2, &context, &argument, 1, &result) == XR_XIR_RUN_OK);
        CHECK(result.type == XR_XIR_BOOL && result.payload == value);
        CHECK(context.live_bytes == 0 && context.frees == 1);
    }
    struct Nullary { uint32_t id, type; int64_t value; uint64_t steps, bytes; XrXirRunStatus status; };
    const struct Nullary nullary[] = {
        {3, XR_XIR_UNIT, 0, 1, 0, XR_XIR_RUN_OK},
        {4, XR_XIR_I64, INT64_MIN, 2, 8, XR_XIR_RUN_OK},
        {5, XR_XIR_UNIT, 0, 3, 0, XR_XIR_RUN_STEP_LIMIT},
        {6, XR_XIR_BOOL, 1, 6, 24, XR_XIR_RUN_OK},
        {7, XR_XIR_BOOL, 0, 2, 8, XR_XIR_RUN_OK},
        {8, XR_XIR_I64, INT64_MAX, 2, 8, XR_XIR_RUN_OK}
    };
    for (size_t i = 0; i < sizeof(nullary) / sizeof(nullary[0]); ++i) {
        XrXirRunContext context = {nullary[i].steps, nullary[i].bytes, 0, 0, 0, 0};
        XrXirValue result = {99, 99, 99};
        CHECK(run(owner, nullary[i].id, &context, NULL, 0, &result) == nullary[i].status);
        CHECK(result.type == nullary[i].type && result.reserved == 0 && result.payload == nullary[i].value);
        CHECK(context.steps == 0 && context.live_bytes == 0 && context.peak_bytes == nullary[i].bytes);
        CHECK(context.allocations == (nullary[i].bytes ? 1u : 0u) && context.frees == context.allocations);
    }
}
#endif
