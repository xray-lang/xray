/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_cases.h - Independent expectations for a real source closure
 *
 * KEY CONCEPT:
 *   Each instance initializes library state once and results outlive instances.
 */
#ifndef XIR_SOURCE_CASES_H
#define XIR_SOURCE_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include "xir_bitwise_cases.h"
typedef struct SourceOutput { uint32_t calls; bool reject_write; } SourceOutput;
static bool source_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    SourceOutput *output = context;
    CHECK(stream == (output->calls == 6 ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(output->calls < 40);
    if (!output->calls) CHECK(length == 8 && !memcmp(bytes, "a\xE4\xB8\xAD 20\n", 8));
    else if (output->calls == 36) CHECK(length == 18 && !memcmp(bytes, "changed method 97\n", 18));
    else if (output->calls == 39) CHECK(length == 41 && !memcmp(bytes, "made ctor fallback made 5 early loop 123\n", 41));
    else if (output->calls == 38) CHECK(length == 28 && !memcmp(bytes, "bound bound arg later 12 12\n", 28));
    else if (output->calls == 37) CHECK(length == 5 && !memcmp(bytes, "1 11\n", 5));
    else if (output->calls == 35) CHECK(length == 19 && !memcmp(bytes, "constructed 0 89 0\n", 19));
    else if (output->calls == 34) CHECK(length == 34 && !memcmp(bytes, "default-value generic explicit 83\n", 34));
    else if (output->calls == 33) CHECK(length == 3 && !memcmp(bytes, "73\n", 3));
    else if (output->calls == 32) CHECK(length == 17 && !memcmp(bytes, "7 generic before\n", 17));
    else if (output->calls == 31) CHECK(length == 2 && !memcmp(bytes, "0\n", 2));
    else if (output->calls == 30) CHECK(length == 36 && !memcmp(bytes, "0 true false complete 17 19 0 false\n", 36));
    else if (output->calls == 29) CHECK(length == 4 && !memcmp(bytes, "5 1\n", 4));
    else if (output->calls == 28) CHECK(length == 24 && !memcmp(bytes, "1 9 default 91 explicit\n", 24));
    else if (output->calls == 27) CHECK(length == 33 && !memcmp(bytes, "41 7 assigned 72 21 1 2 assigned\n", 33));
    else if (output->calls == 24) CHECK(length == 38 && !memcmp(bytes, "16777216 -7 -7 -7 true true true true\n", 38));
    else if (output->calls == 25) CHECK(length == 23 && !memcmp(bytes, "true true true 127 255\n", 23));
    else if (output->calls == 26) CHECK(length == 40 && !memcmp(bytes, "true true true true true true true true\n", 40));
    else if (output->calls == 21) CHECK(length == 54 && !memcmp(bytes, "-1 -1 -1 -1 255 65535 4294967295 18446744073709551615\n", 54));
    else if (output->calls == 22) CHECK(length == 75 && !memcmp(bytes, "-9223372036854775808 -127 129 -9223372036854775679 9223372036854775808 -56\n", 75));
    else if (output->calls == 23) CHECK(length == 31 && !memcmp(bytes, "-128 -32768 255 7 9 12 1 65535\n", 31));
    else if (output->calls == 17) CHECK(length == 87 && !memcmp(bytes, "-128 -32768 -2147483648 -9223372036854775808 255 65535 4294967295 18446744073709551615\n", 87));
    else if (output->calls == 18) CHECK(length == 32 && !memcmp(bytes, "-56 200 1 255 100 -128 -128 -56\n", 32));
    else if (output->calls == 19) CHECK(length == 68 && !memcmp(bytes, "-128 18446744073709551615 18446744073709551615 18446744073709551615\n", 68));
    else if (output->calls == 20) CHECK(length == 26 && !memcmp(bytes, "16 8 11 7 9 -128 0 121212\n", 26));
    else if (output->calls == 15) CHECK(length == 35 && !memcmp(bytes, "-128 -128 -127 0 0 4294967295 true\n", 35));
    else if (output->calls == 16) CHECK(length == 46 && !memcmp(bytes, "18446744073709551615 -32768 -2147483648 65535\n", 46));
    else if (output->calls == 14) CHECK(length == 20 && !memcmp(bytes, "2 5 7 6 bb cc xxy 9\n", 20));
    else if (output->calls == 13) CHECK(length == 25 && !memcmp(bytes, "1792 8 -1 -4 1 -35 aabab\n", 25));
    else if (output->calls == 12) CHECK(length == 13 && !memcmp(bytes, "-1 6 -2 -1 7\n", 13));
    else if (output->calls == 11) CHECK(length == 15 && !memcmp(bytes, "4 fxxx!dd 13 4\n", 15));
    else if (output->calls == 10) CHECK(length == 16 && !memcmp(bytes, "ssxxx! 7 9 true\n", 16));
    else if (output->calls == 1) CHECK(length == 3 && !memcmp(bytes, "10\n", 3));
    else if (output->calls == 2) CHECK(length == 6 && !memcmp(bytes, "11 12\n", 6));
    else if (output->calls == 3) CHECK(length == 5 && !memcmp(bytes, "true\n", 5));
    else if (output->calls == 4) CHECK(length == 11 && !memcmp(bytes, "13 14 ax y\n", 11));
    else if (output->calls == 5) CHECK(length == 16 && !memcmp(bytes, "aaxy true 42 az\n", 16));
    else if (output->calls == 6) CHECK(length == 4 && !memcmp(bytes, "aerr", 4));
    else if (output->calls == 8) CHECK(length == 4 && !memcmp(bytes, "aout", 4));
    else if (output->calls == 7 && output->reject_write) CHECK(length == 6 && !memcmp(bytes, "false\n", 6));
    else CHECK(length == 5 && !memcmp(bytes, "true\n", 5));
    ++output->calls;
    return !(output->reject_write && output->calls == 7);
}
static bool source_output(void *context, const XrXirOutputGroup *group) {
    XrXirOutputSink *sink = context;
    SourceOutput *output = sink->context;
    if (output->calls == 36) {
        CHECK(group && group->count == 3 && group->line && group->stream == XR_XIR_STDOUT);
        const char *expected[] = {"changed", "method"};
        for (uint32_t i = 0; i < 2; ++i) {
            const char *bytes = NULL; size_t length = 0;
            CHECK(group->values[i].type == XR_XIR_STRING && xr_xir_string_view(&group->values[i], &bytes, &length));
            CHECK(length == strlen(expected[i]) && !memcmp(bytes, expected[i], length));
        }
        CHECK(group->values[2].type == XR_XIR_I64 && group->values[2].payload == 97);
    }
    if (output->calls == 39) {
        CHECK(group && group->count == 8 && group->line && group->stream == XR_XIR_STDOUT);
        const char *expected[] = {"made", "ctor", "fallback", "made"};
        for (uint32_t i = 0; i < 4; ++i) {
            const char *bytes = NULL; size_t length = 0;
            CHECK(group->values[i].type == XR_XIR_STRING && xr_xir_string_view(&group->values[i], &bytes, &length));
            CHECK(length == strlen(expected[i]) && !memcmp(bytes, expected[i], length));
        }
        CHECK(group->values[4].type == XR_XIR_I64 && group->values[4].payload == 5);
        for (uint32_t i = 5; i < 7; ++i) {
            const char *bytes = NULL; size_t length = 0;
            const char *label = i == 5 ? "early" : "loop";
            CHECK(group->values[i].type == XR_XIR_STRING && xr_xir_string_view(&group->values[i], &bytes, &length));
            CHECK(length == strlen(label) && !memcmp(bytes, label, length));
        }
        CHECK(group->values[7].type == XR_XIR_I64 && group->values[7].payload == 123);
    }
    if (output->calls == 38) {
        CHECK(group && group->count == 6 && group->line && group->stream == XR_XIR_STDOUT);
        const char *expected[] = {"bound", "bound", "arg", "later"};
        for (uint32_t i = 0; i < 4; ++i) {
            const char *bytes = NULL; size_t length = 0;
            CHECK(group->values[i].type == XR_XIR_STRING && xr_xir_string_view(&group->values[i], &bytes, &length));
            CHECK(length == strlen(expected[i]) && !memcmp(bytes, expected[i], length));
        }
        for (uint32_t i = 4; i < 6; ++i) CHECK(group->values[i].type == XR_XIR_I64 && group->values[i].payload == 12);
    }
    if (output->calls == 37) {
        CHECK(group && group->count == 2 && group->line && group->stream == XR_XIR_STDOUT);
        CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 1);
        CHECK(group->values[1].type == XR_XIR_I64 && group->values[1].payload == 11);
    }
    if (output->calls == 35) {
        CHECK(group && group->count == 4 && group->line && group->stream == XR_XIR_STDOUT);
        const char *bytes = NULL; size_t length = 0;
        CHECK(group->values[0].type == XR_XIR_STRING && xr_xir_string_view(&group->values[0], &bytes, &length));
        CHECK(length == 11 && !memcmp(bytes, "constructed", 11));
        for (uint32_t i = 1; i < 4; ++i)
            CHECK(group->values[i].type == XR_XIR_I64 && group->values[i].payload == (i == 2 ? 89 : 0));
    }
    if (output->calls == 34) {
        CHECK(group && group->count == 4 && group->line && group->stream == XR_XIR_STDOUT);
        const char *expected[] = {"default-value", "generic", "explicit"};
        for (uint32_t i = 0; i < 3; ++i) {
            const char *bytes = NULL; size_t length = 0;
            CHECK(group->values[i].type == XR_XIR_STRING && xr_xir_string_view(&group->values[i], &bytes, &length));
            CHECK(length == strlen(expected[i]) && !memcmp(bytes, expected[i], length));
        }
        CHECK(group->values[3].type == XR_XIR_I64 && group->values[3].payload == 83);
    }
    if (output->calls == 33) {
        CHECK(group && group->count == 1 && group->line && group->stream == XR_XIR_STDOUT);
        CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 73);
    }
    if (output->calls == 32) {
        CHECK(group && group->count == 3 && group->line && group->stream == XR_XIR_STDOUT);
        CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 7);
        const char *bytes = NULL; size_t length = 0;
        CHECK(group->values[1].type == XR_XIR_STRING && xr_xir_string_view(&group->values[1], &bytes, &length));
        CHECK(length == 7 && !memcmp(bytes, "generic", 7));
        CHECK(group->values[2].type == XR_XIR_STRING && xr_xir_string_view(&group->values[2], &bytes, &length));
        CHECK(length == 6 && !memcmp(bytes, "before", 6));
    }
    if (output->calls == 27) {
        CHECK(group && group->count == 8 && group->line && group->stream == XR_XIR_STDOUT);
        const int64_t expected[] = {41, 7, 0, 72, 21, 1, 2, 0};
        for (uint32_t i = 0; i < 8; ++i) {
            if (i == 2 || i == 7) {
                const char *bytes = NULL; size_t length = 0;
                CHECK(group->values[i].type == XR_XIR_STRING &&
                    xr_xir_string_view(&group->values[i], &bytes, &length));
                CHECK(length == 8 && !memcmp(bytes, "assigned", 8));
            } else CHECK(group->values[i].type == XR_XIR_I64 && group->values[i].payload == expected[i]);
        }
    }
    if (output->calls == 28) {
        CHECK(group && group->count == 5 && group->line && group->stream == XR_XIR_STDOUT);
        CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 1);
        CHECK(group->values[1].type == XR_XIR_I64 && group->values[1].payload == 9);
        CHECK(group->values[3].type == XR_XIR_I64 && group->values[3].payload == 91);
        const char *bytes = NULL; size_t length = 0;
        CHECK(group->values[2].type == XR_XIR_STRING && xr_xir_string_view(&group->values[2], &bytes, &length));
        CHECK(length == 7 && !memcmp(bytes, "default", 7));
        CHECK(group->values[4].type == XR_XIR_STRING && xr_xir_string_view(&group->values[4], &bytes, &length));
        CHECK(length == 8 && !memcmp(bytes, "explicit", 8));
    }
    if (output->calls == 29) {
        CHECK(group && group->count == 2 && group->line && group->stream == XR_XIR_STDOUT);
        CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 5);
        CHECK(group->values[1].type == XR_XIR_I64 && group->values[1].payload == 1);
    }
    if (output->calls == 30) {
        CHECK(group && group->count == 8 && group->line && group->stream == XR_XIR_STDOUT);
        const uint32_t types[] = {XR_XIR_I64, XR_XIR_BOOL, XR_XIR_BOOL, XR_XIR_STRING,
            XR_XIR_I64, XR_XIR_I64, XR_XIR_I16, XR_XIR_BOOL};
        const int64_t values[] = {0, 1, 0, 0, 17, 19, 0, 0};
        for (uint32_t i = 0; i < 8; ++i) {
            CHECK(group->values[i].type == types[i]);
            if (i != 3) CHECK(group->values[i].payload == values[i]);
        }
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&group->values[3], &bytes, &length));
        CHECK(length == 8 && !memcmp(bytes, "complete", 8));
    }
    if (output->calls == 31) {
        CHECK(group && group->count == 1 && group->line && group->stream == XR_XIR_STDOUT);
        CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 0);
    }
    return xr_xir_output_render(sink, group);
}
static bool source_reject(void *context, const XrXirOutputGroup *group) {
    uint32_t *calls = context;
    CHECK(group && group->line && group->stream == XR_XIR_STDOUT && group->count == 2);
    ++*calls; return false;
}
static void source_resume_once(XrXirInstance *instance, XrXirInstanceResult suspended) {
    CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED && suspended.outcome.wake && suspended.epoch);
    CHECK(suspended.outcome.value.type == XR_XIR_UNIT && !suspended.outcome.value.payload);
    XrXirInstanceResult repeated = xr_xir_instance_poll(instance);
    CHECK(repeated.epoch == suspended.epoch && repeated.outcome.wake == suspended.outcome.wake &&
        repeated.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance, suspended.epoch + 1, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, suspended.epoch, suspended.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, suspended.epoch, suspended.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(instance, suspended.epoch, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
}
static XrXirCallResult source_drive(XrXirInstance *instance, unsigned expected_suspensions) {
    XrXirInstanceResult result = xr_xir_instance_poll(instance);
    for (unsigned i = 0; i < expected_suspensions; ++i) {
        source_resume_once(instance, result);
        result = xr_xir_instance_poll(instance);
    }
    CHECK(result.outcome.status != XR_XIR_CALL_SUSPENDED);
    return result.outcome;
}
static void source_resume_pair(XrXirInstance **instances, uint32_t entry, XrXirValue *results) {
    XrXirValue argument = {XR_XIR_BOOL, 0, 0};
    for (unsigned i = 0; i < 2; ++i) {
        argument.payload = i;
        CHECK(xr_xir_instance_start(instances[i], entry, &argument, 1) == XR_XIR_CALL_READY);
    }
    for (unsigned step = 0; step < 3; ++step) for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceResult result = xr_xir_instance_poll(instances[i]);
        if (step < 2) source_resume_once(instances[i], result);
        else {
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
        }
    }
}
static void source_cancel_cases(XrXirProgram *program, uint32_t entry, uint32_t resume_text) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        SourceOutput output = {0}; XrXirOutputSink sink = {source_bytes, &output, 65536};
        XrXirInstanceConfig config = xr_xir_instance_defaults(); config.poll_limit = 4000;
        config.output = (XrXirOutputProvider) {source_output, &sink};
        XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
        if (mode > 2) {
            CHECK(source_drive(instance, 4).status == XR_XIR_CALL_RETURNED && output.calls == 40);
            XrXirValue arg = {XR_XIR_BOOL, 0, 1};
            CHECK(xr_xir_instance_start(instance, resume_text, &arg, 1) == XR_XIR_CALL_READY);
        }
        XrXirInstanceResult suspended = {0};
        if (mode) {
            suspended = xr_xir_instance_poll(instance);
            CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED);
            if (mode == 2 || mode == 4) {
                source_resume_once(instance, suspended); suspended = xr_xir_instance_poll(instance);
                CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED);
            }
        }
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_resume(instance, suspended.epoch, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        if (mode <= 2) {
            CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_DRAINING && output.calls == 0);
            XrXirCallResult failure = {0};
            CHECK(xr_xir_instance_copy_failure(instance, &failure) == XR_XIR_CALL_CANCELLED &&
                failure.status == XR_XIR_CALL_CANCELLED && failure.value.type == XR_XIR_UNIT &&
                xr_xir_fault_empty(failure.fault));
            CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_CANCELLED);
        } else CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
}
static void source_failed_init(XrXirProgram *program, uint32_t entry, XrXirInstanceConfig config) {
    uint32_t calls = 0;
    config.output = (XrXirOutputProvider) {source_reject, &calls};
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(source_drive(instance, 2).status == XR_XIR_CALL_OUTPUT_ERROR);
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED && calls == 1);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_OUTPUT_ERROR && calls == 1);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
static void source_updates(XrXirInstance *instance, uint32_t update) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        int64_t start = mode == 0 ? INT64_MAX : mode == 1 ? INT64_MIN : 4;
        XrXirValue args[] = {{XR_XIR_I64, 0, start}, {XR_XIR_BOOL, 0, mode % 2 == 0}};
        CHECK(xr_xir_instance_start(instance, update, args, 2) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instance).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED);
        CHECK(result.value.payload == (mode == 0 ? INT64_MIN : mode == 1 ? INT64_MAX : mode == 2 ? 5 : 3));
        CHECK(args[0].payload == start);
    }
}
static void source_numeric(XrXirInstance *instance, uint32_t calculate) {
    struct Numeric { int64_t kind, left, right, expected; bool fault; };
    const struct Numeric cases[] = {
        {0, INT64_MAX, 1, INT64_MIN, false}, {0, INT64_MIN, -1, INT64_MAX, false},
        {1, INT64_MIN, 1, INT64_MAX, false}, {1, INT64_MAX, -1, INT64_MIN, false},
        {1, 0, INT64_MIN, INT64_MIN, false}, {2, INT64_MAX, 2, -2, false},
        {2, INT64_MIN, -1, INT64_MIN, false}, {2, INT64_MIN, INT64_MIN, 0, false},
        {2, -7, 3, -21, false}, {3, -7, 3, -2, false}, {3, 7, -3, -2, false},
        {3, -7, -3, 2, false}, {3, INT64_MIN, -1, INT64_MIN, false},
        {3, INT64_MIN, 1, INT64_MIN, false}, {3, 1, 0, 0, true},
        {3, 7, 3, 2, false}, {4, -7, 3, -1, false}, {4, 7, -3, 1, false},
        {4, -7, -3, -1, false}, {4, INT64_MIN, -1, 0, false}, {4, 1, 0, 0, true},
        {4, 7, 3, 1, false}, {5, INT64_MIN, 0, INT64_MIN, false}, {5, INT64_MAX, 0, -INT64_MAX, false}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrXirValue args[] = {{XR_XIR_I64, 0, cases[i].kind}, {XR_XIR_I64, 0, cases[i].left},
            {XR_XIR_I64, 0, cases[i].right}};
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instance).outcome;
        CHECK(result.status == (cases[i].fault ? XR_XIR_CALL_DIVIDE_BY_ZERO : XR_XIR_CALL_RETURNED));
        CHECK(result.value.type == (uint32_t) (cases[i].fault ? XR_XIR_UNIT : XR_XIR_I64));
        CHECK(result.value.payload == cases[i].expected && result.value.reserved == 0);
        CHECK(args[1].payload == cases[i].left && args[2].payload == cases[i].right);
    }
    const int64_t values[] = {INT64_MIN, -1, 0, 1, INT64_MAX};
    for (unsigned kind = 6; kind < 12; ++kind) for (unsigned i = 0; i < 5; ++i) for (unsigned j = 0; j < 5; ++j) {
        XrXirValue args[] = {{XR_XIR_I64, 0, kind}, {XR_XIR_I64, 0, values[i]}, {XR_XIR_I64, 0, values[j]}};
        bool expected = kind == 6 ? i == j : kind == 7 ? i != j : kind == 8 ? i < j :
            kind == 9 ? i <= j : kind == 10 ? i > j : i >= j;
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instance).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == expected);
    }
}
static void source_bitwise(XrXirInstance *instance, uint32_t calculate) {
    for (unsigned i = 0; i < sizeof(bitwise_cases) / sizeof(bitwise_cases[0]); ++i) {
        const XirBitwiseCase *row = &bitwise_cases[i];
        XrXirValue args[] = {{XR_XIR_I64, 0, 12 + row->operation},
            {XR_XIR_I64, 0, row->left}, {XR_XIR_I64, 0, row->right}};
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instance).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == row->expected);
        CHECK(args[1].payload == row->left && args[2].payload == row->right);
    }
    const int64_t inputs[] = {INT64_MIN, INT64_MAX, -1, 0, 1, 85};
    const int64_t expected[] = {INT64_MAX, INT64_MIN, 0, -1, -2, -86};
    for (unsigned i = 0; i < 6; ++i) {
        XrXirValue args[] = {{XR_XIR_I64, 0, 17}, {XR_XIR_I64, 0, inputs[i]}, {XR_XIR_I64, 0, 0}};
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instance).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == expected[i]);
    }
}
static void source_constructor_fault(XrXirInstance *instance, uint32_t function) {
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirValue args[] = {{XR_XIR_I64, 0, 22}, {XR_XIR_I64, 0, 18}, {XR_XIR_I64, 0, i ? 3 : 0}};
        CHECK(xr_xir_instance_start(instance, function, args, 3) == XR_XIR_CALL_READY);
        XrXirInstanceResult suspended = xr_xir_instance_poll(instance);
        CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED);
        source_resume_once(instance, suspended);
        XrXirCallResult result = source_drive(instance, 0);
        CHECK(result.status == (i ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_DIVIDE_BY_ZERO));
        CHECK(result.value.type == (uint32_t)(i ? XR_XIR_I64 : XR_XIR_UNIT));
        CHECK(result.value.payload == (i ? 6 : 0));
    }
}
static void source_compound_fault(XrXirInstance *instance, uint32_t calculate) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirValue args[] = {{XR_XIR_I64, 0, 18}, {XR_XIR_I64, 0, 9}, {XR_XIR_I64, 0, i ? 3 : 0}};
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instance).outcome;
        CHECK(result.status == (i ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_DIVIDE_BY_ZERO));
        CHECK(result.value.payload == (i ? 3 : 0));
        args[0].payload = 19;
        CHECK(xr_xir_instance_start(instance, calculate, args, 3) == XR_XIR_CALL_READY);
        result = xr_xir_instance_poll(instance).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == (i ? 3 : 0));
    }
}
static void source_float_fault(XrXirInstance *instance, uint32_t function) {
    const int64_t inputs[] = {127, 128, -128, -129, INT64_MAX, 0};
    int64_t stored = -7;
    for (unsigned i = 0; i < 6; ++i) {
        bool valid = i == 0 || i == 2 || i == 5;
        XrXirValue args[] = {{XR_XIR_I64, 0, 20}, {XR_XIR_I64, 0, inputs[i]}, {XR_XIR_I64, 0, 0}};
        CHECK(xr_xir_instance_start(instance, function, args, 3) == XR_XIR_CALL_READY);
        XrXirCallResult result = source_drive(instance, 1);
        CHECK(result.status == (valid ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_NUMERIC_RANGE));
        CHECK(result.value.type == (uint32_t) (valid ? XR_XIR_I64 : XR_XIR_UNIT));
        CHECK(!result.value.reserved && result.value.payload == (valid ? inputs[i] : 0));
        if (valid) stored = inputs[i];
        args[0].payload = 21;
        CHECK(xr_xir_instance_start(instance, function, args, 3) == XR_XIR_CALL_READY);
        result = source_drive(instance, 0);
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == stored);
    }
}
static void source_deep_pair(XrXirInstance **instances, uint32_t function, XrXirValue *results) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirValue argument = {XR_XIR_I64, 0, 64 + i};
        CHECK(xr_xir_instance_start(instances[i], function, &argument, 1) == XR_XIR_CALL_READY);
    }
    for (unsigned i = 0; i < 2; ++i) source_resume_once(instances[i], xr_xir_instance_poll(instances[i]));
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
    }
}
static void source_numeric_pair(XrXirInstance **instances, uint32_t function) {
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirValue argument = {XR_XIR_BOOL, 0, i};
        CHECK(xr_xir_instance_start(instances[i], function, &argument, 1) == XR_XIR_CALL_READY);
    }
    for (unsigned step = 0; step < 2; ++step) for (uint32_t i = 0; i < 2; ++i) source_resume_once(instances[i], xr_xir_instance_poll(instances[i]));
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(source_drive(instances[i], 0).status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instances[i], &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == (i ? -128 : 32767));
        xr_xir_value_drop(&value);
    }
}
typedef struct SourceFunctions { uint32_t result, advance, update, calculate, resume_text, stack_depth, numeric_pause, bound_result; } SourceFunctions;
static void source_pair(XrXirProgram *program, uint32_t entry, SourceFunctions functions, XrXirValue *results) {
    SourceOutput outputs[2] = {{0, false}, {0, true}};
    XrXirOutputSink sinks[2] = {{source_bytes, &outputs[0], 65536}, {source_bytes, &outputs[1], 65536}};
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    config.metadata_limit = 65536; config.value_limit = 65536; config.call_limit = 65536;
    config.poll_limit = 4000; config.depth_limit = 96;
    XrXirInstance *instances[2] = {NULL, NULL};
    for (uint32_t i = 0; i < 2; ++i) {
        config.output = (XrXirOutputProvider) {source_output, &sinks[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
    }
    for (unsigned step = 0; step < 5; ++step) for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceResult result = xr_xir_instance_poll(instances[i]);
        if (step < 4) {
            CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_INITIALIZING && outputs[i].calls == (step >= 2 ? 25u + step : 0u));
            source_resume_once(instances[i], result);
        } else {
            if (result.outcome.status != XR_XIR_CALL_RETURNED)
                fprintf(stderr, "Source initialization status=%u outputs=%u\n", result.outcome.status, outputs[i].calls);
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.payload);
            CHECK(outputs[i].calls == 40 && xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_READY);
        }
    }
    source_cancel_cases(program, entry, functions.resume_text);
    source_numeric_pair(instances, functions.numeric_pause);
    XrXirValue resumed[2] = {{0}, {0}};
    source_resume_pair(instances, functions.resume_text, resumed);
    XrXirValue deep[2] = {{0}, {0}};
    source_deep_pair(instances, functions.stack_depth, deep);
    source_failed_init(program, entry, config);
    for (unsigned i = 0; i < 2; ++i) source_constructor_fault(instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_compound_fault(instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_bitwise(instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_numeric(instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_float_fault(instances[i], functions.calculate);
    for (unsigned i = 0; i < 2; ++i) source_updates(instances[i], functions.update);
    for (int64_t expected = 15; expected < 17; ++expected) for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions.advance, NULL, 0) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instances[i]).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == expected && outputs[i].calls == 40);
    }
    XrXirValue bound[2] = {{0}, {0}}, bound_text[2] = {{0}, {0}};
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions.bound_result, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &bound[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_start_function(instances[1 - i], &bound[i], NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start_function(instances[i], &bound[i], NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &bound_text[i]) == XR_XIR_CALL_RETURNED);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions.result, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_stop(instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_function(instances[i], &bound[i], NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        XrXirValue copy = {0};
        CHECK(xr_xir_value_copy(&bound[i], &copy) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&bound[i]); xr_xir_value_drop(&copy);
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&bound_text[i], &bytes, &length) && length == 5 && !memcmp(bytes, "bound", 5));
        xr_xir_value_drop(&bound_text[i]);
        CHECK(xr_xir_string_view(&resumed[i], &bytes, &length) && length == 3 && !memcmp(bytes, i ? "ry!" : "rn!", 3));
        xr_xir_value_drop(&resumed[i]);
        CHECK(xr_xir_string_view(&deep[i], &bytes, &length) && length == 130 + i * 2);
        for (size_t at = 0; at < length; ++at) CHECK(bytes[at] == (at % 2 ? 'r' : 'f'));
        xr_xir_value_drop(&deep[i]);
    }
}
static void source_result_drop(XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == 2 && !memcmp(bytes, "a!", 2));
    xr_xir_value_drop(value);
}
#endif // XIR_SOURCE_CASES_H
