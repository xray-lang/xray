/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_integer_execution_cases.h - Independent typed VM and native expectations
 *
 * KEY CONCEPT:
 *   Every integer instruction result is checked against a fixed mathematical value.
 */
#ifndef XIR_INTEGER_EXECUTION_CASES_H
#define XIR_INTEGER_EXECUTION_CASES_H
#include "xir/xxir.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_output.h"

typedef struct IntegerExecutionCase {
    uint32_t function;
    XrXirType type, right_type, result;
    XrXirOp operation;
    uint32_t count;
    int64_t left, right, expected;
} IntegerExecutionCase;
static const IntegerExecutionCase integer_execution_rows[] = {
#define XIR_INTEGER_CASE(id, type, op, rhs, result, count, left, right, expected) \
    {id, type, rhs, result, op, count, left, right, expected},
#include "xir_integer_cases.def"
#undef XIR_INTEGER_CASE
};
static bool integer_output_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    unsigned *calls = context;
    const char expected[] = "-128 -32768 -2147483648 -9223372036854775808 255 65535 4294967295 18446744073709551615\n";
    CHECK(stream == XR_XIR_STDOUT && length == sizeof(expected) - 1);
    CHECK(!memcmp(bytes, expected, length));
    ++*calls; return true;
}
static void integer_value_boundaries(void) {
    XrXirValue values[] = {{XR_XIR_I8, 0, INT8_MIN}, {XR_XIR_I16, 0, INT16_MIN},
        {XR_XIR_I32, 0, INT32_MIN}, {XR_XIR_I64, 0, INT64_MIN}, {XR_XIR_U8, 0, UINT8_MAX},
        {XR_XIR_U16, 0, UINT16_MAX}, {XR_XIR_U32, 0, UINT32_MAX}, {XR_XIR_U64, 0, -1}};
    unsigned calls = 0;
    XrXirOutputSink sink = {integer_output_bytes, &calls, 1024};
    XrXirOutputGroup group = {XR_XIR_STDOUT, values, 8, true};
    CHECK(xr_xir_output_render(&sink, &group) && calls == 1);
    sink.byte_limit = 79;
    CHECK(!xr_xir_output_render(&sink, &group) && calls == 1);
    sink.byte_limit = 1024;
    values[0].payload = 128;
    CHECK(!xr_xir_output_render(&sink, &group) && calls == 1);
    values[0].payload = INT8_MIN;
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(4096, &domain) == XR_XIR_VALUE_OK);
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    for (unsigned i = 0; i < 8; ++i) {
        XrXirType type = (XrXirType) values[i].type;
        XrXirTypeNode node = {XR_XIR_TYPE_CELL,type,NULL,0,XR_XIR_UNIT,0,0, {0}};
        XrXirTypes types = {&node,1, NULL};
        XrXirBudget budget = {.parameters = 16, .metadata_bytes = 4096, .work = 64};
        XrXirTypeArena *arena = NULL;
        CHECK(xr_xir_type_arena_new(domain,&types,&budget,&arena) == XR_XIR_VALUE_OK);
        XrXirValueAdmission admission = {arena,domain,NULL,NULL,16,0};
        XrXirValue copy = {0}, cell = {0}, read = {0};
        CHECK(xr_xir_value_copy(&values[i], &copy) == XR_XIR_VALUE_OK);
        CHECK(copy.type == values[i].type && copy.payload == values[i].payload);
        CHECK(xr_xir_cell_new(domain,arena,(XrXirType)256,&copy,&admission,&cell) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_argument(&cell,arena,(XrXirType)256));
        CHECK(xr_xir_cell_read(&cell, &read) == XR_XIR_VALUE_OK && read.payload == copy.payload);
        CHECK(read.type == (uint32_t) type);
        copy.type = XR_XIR_BOOL;
        CHECK(xr_xir_cell_write(&cell,&copy,&admission) == XR_XIR_VALUE_BAD_ARGUMENT);
        copy.type = (uint32_t) type; copy.payload = 0;
        CHECK(xr_xir_cell_write(&cell,&copy,&admission) == XR_XIR_VALUE_OK);
        CHECK(read.payload == values[i].payload);
        xr_xir_value_drop(&copy); xr_xir_value_drop(&read); xr_xir_value_drop(&cell);
        CHECK(!copy.type && !read.type && !cell.type);
        xr_xir_type_arena_drop(arena);
        CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    }
    xr_xir_domain_drop(domain);
}
static void integer_execution_cases(FixtureRun run, void *owner) {
    for (unsigned i = 0; i < sizeof(integer_execution_rows) / sizeof(integer_execution_rows[0]); ++i) {
        const IntegerExecutionCase *row = &integer_execution_rows[i];
        XrXirValue arguments[] = {{row->type, 0, row->left}, {row->right_type, 0, row->right}}, result = {0};
        XrXirRunContext context = {2, 24, 0, 0, 0, 0};
        CHECK(run(owner, row->function, &context, arguments, row->count, &result) == XR_XIR_RUN_OK);
        CHECK(result.type == (uint32_t) row->result && !result.reserved && result.payload == row->expected);
        CHECK(!context.steps && !context.live_bytes && context.allocations == 1 && context.frees == 1);
        CHECK(xr_xir_value_argument(&result, NULL, row->result));
        if (row->operation == XR_XIR_DIV_INT || row->operation == XR_XIR_REM_INT) {
            arguments[1].payload = 0; context = (XrXirRunContext) {2, 24, 0, 0, 0, 0};
            CHECK(run(owner, row->function, &context, arguments, 2, &result) == XR_XIR_RUN_DIVIDE_BY_ZERO);
            CHECK(!result.type && !result.reserved && !result.payload);
            CHECK(!context.live_bytes && context.allocations == context.frees);
        }
        arguments[0].type = XR_XIR_BOOL; arguments[0].payload = 1;
        context = (XrXirRunContext) {2, 24, 0, 0, 0, 0};
        CHECK(run(owner, row->function, &context, arguments, row->count, &result) == XR_XIR_RUN_BAD_ARGUMENT);
        CHECK(context.steps == 2 && !context.allocations && !result.type && !result.payload);
        if (xr_xir_integer_bits(row->type) < 64) {
            arguments[0] = (XrXirValue) {row->type, 0, INT64_MAX};
            CHECK(run(owner, row->function, &context, arguments, row->count, &result) == XR_XIR_RUN_BAD_ARGUMENT);
            CHECK(context.steps == 2 && !context.allocations && !result.type);
        }
    }
    integer_value_boundaries();
    puts("Typed integer XIR: 192 operation/cast expectations, admission failures and unsigned output passed");
}
#endif // XIR_INTEGER_EXECUTION_CASES_H
