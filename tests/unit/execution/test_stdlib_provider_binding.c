/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_stdlib_provider_binding.c - Generated host calls and resource safety
 */

#include "execution/xr_stdlib_provider_binding.h"
#include "os/os_pipe.h"
#include "shared/xr_os_core.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL: %s\n", message);                                                \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static bool binding(const char *symbol, XrProviderOperationBinding *out) {
    for (size_t index = 0u; index < xr_stdlib_provider_count(); ++index) {
        const XrStdlibProviderDescriptor *descriptor = xr_stdlib_provider_at(index);
        if (strcmp(descriptor->symbol, symbol) != 0)
            continue;
        uint32_t behavior = 0u;
        if (!xr_stdlib_provider_operation_binding(descriptor, out, &behavior))
            break;
        CHECK(behavior == (XR_PROVIDER_BEHAVIOR_THREAD_SAFE | XR_PROVIDER_BEHAVIOR_REENTRANT),
              "binding behaviors match the declared host contract");
        return true;
    }
    fprintf(stderr, "FAIL: missing binding %s\n", symbol);
    ++failures;
    return false;
}

static void scalar_queries(void) {
    const char *clocks[] = {"time.__realtimeNanos", "time.__monotonicNanos", "time.__cpuNanos"};
    for (size_t index = 0u; index < sizeof(clocks) / sizeof(clocks[0]); ++index) {
        XrProviderOperationBinding operation;
        if (!binding(clocks[index], &operation))
            continue;
        CHECK(operation.trampoline_kind == XR_PROVIDER_TRAMPOLINE_I64_NULLARY,
              "clock uses a nullary typed entry");
        int64_t result = -1;
        CHECK(operation.entry.i64_nullary(NULL, &result) == XR_PROVIDER_CALL_OK && result >= 0,
              "generated clock adapter reaches the real host");
        CHECK(operation.entry.i64_nullary(NULL, NULL) == XR_PROVIDER_CALL_FAILED,
              "missing clock output is refused");
    }
    XrProviderOperationBinding utc;
    if (binding("time.__utcOffsetAt", &utc)) {
        CHECK(utc.trampoline_kind == XR_PROVIDER_TRAMPOLINE_I64_UNARY,
              "UTC query uses a unary typed entry");
        int64_t result = 777;
        CHECK(utc.entry.i64_unary(NULL, INT64_MAX, &result) == XR_PROVIDER_CALL_FAILED &&
                  result == 777,
              "host query failure reaches the typed refusal boundary without a result");
        CHECK(utc.entry.i64_unary(NULL, INT64_C(1704067200), &result) == XR_PROVIDER_CALL_OK &&
                  result >= -1440 && result <= 1440,
              "ordinary UTC query reaches the real host timezone");
    }
    XrProviderOperationBinding process;
    if (binding("os.__getpid", &process)) {
        CHECK(process.trampoline_kind == XR_PROVIDER_TRAMPOLINE_I64_NULLARY,
              "the new process family has a typed nullary entry");
        int64_t result = -1;
        CHECK(process.entry.i64_nullary(NULL, &result) == XR_PROVIDER_CALL_OK &&
                  result == xr_os_core_getpid() && result > 0,
              "getpid reaches the current process through its generated adapter");
    }
}

static void pipe_lifetime(void) {
    XrProviderOperationBinding open;
    XrProviderOperationBinding close;
    if (!binding("sys.__pipeOpen", &open) || !binding("sys.__pipeClose", &close))
        return;
    CHECK(open.trampoline_kind == XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY &&
              close.trampoline_kind == XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
          "pipe adapters keep distinct typed entries");
    bool present = false;
    int64_t first = -1;
    int64_t second = -1;
    CHECK(open.entry.optional_i64_pair_nullary(NULL, NULL, &first, &second) ==
                  XR_PROVIDER_CALL_FAILED &&
              first == -1 && second == -1,
          "missing pipe result storage is refused before resource acquisition");
    if (open.entry.optional_i64_pair_nullary(NULL, &present, &first, &second) !=
            XR_PROVIDER_CALL_OK ||
        !present) {
        CHECK(false, "real pipe acquisition succeeds");
        return;
    }
    bool closed = true;
    const char input = 'x';
    char output = 0;
    CHECK(xr_pipe_write((XrPipeHandle) second, &input, 1u) == 1,
          "the acquired pipe accepts one byte");
#ifndef XR_OS_WINDOWS
#if INTPTR_MAX > INT_MAX
    int64_t bad_read = first + (INT64_C(1) << 32);
    int64_t bad_write = second + (INT64_C(1) << 32);
    int64_t count = 123;
    CHECK(xr_pipe_read((XrPipeHandle) bad_read, &output, 1u) == -1 && output == 0,
          "out-of-range read cannot consume a live descriptor's data");
    CHECK(xr_pipe_write((XrPipeHandle) bad_write, &input, 1u) == -1,
          "out-of-range write cannot mutate a live descriptor");
    CHECK(xr_pipe_try_read((XrPipeHandle) bad_read, &output, 1u, &count) == XR_PIPE_IO_ERROR &&
              count == -1 && output == 0,
          "out-of-range nonblocking read is refused");
    count = 123;
    CHECK(xr_pipe_try_write((XrPipeHandle) bad_write, &input, 1u, &count) == XR_PIPE_IO_ERROR &&
              count == -1,
          "out-of-range nonblocking write is refused");
#endif
    CHECK(close.entry.bool_i64_unary(NULL, first + (INT64_C(1) << 32), &closed) ==
                  XR_PROVIDER_CALL_OK &&
              !closed,
          "an out-of-range pipe token cannot close another descriptor by truncation");
#endif
    CHECK(xr_pipe_read((XrPipeHandle) first, &output, 1u) == 1 && output == input,
          "invalid close preserves the live pipe and its contents");
    CHECK(close.entry.bool_i64_unary(NULL, first, &closed) == XR_PROVIDER_CALL_OK && closed,
          "the first acquired endpoint is closed");
    CHECK(close.entry.bool_i64_unary(NULL, second, &closed) == XR_PROVIDER_CALL_OK && closed,
          "the second acquired endpoint is closed");
    CHECK(close.entry.bool_i64_unary(NULL, first, &closed) == XR_PROVIDER_CALL_OK && !closed,
          "a failed physical close is a normal false result");
}

static void forged_descriptor(void) {
    const XrStdlibProviderDescriptor *descriptor = xr_stdlib_provider_at(0u);
    XrStdlibProviderDescriptor copy = *descriptor;
    XrProviderOperationBinding output;
    memset(&output, 0x5a, sizeof(output));
    XrProviderOperationBinding before = output;
    uint32_t behavior = 123u;
    CHECK(!xr_stdlib_provider_operation_binding(&copy, &output, &behavior) && behavior == 123u &&
              memcmp(&output, &before, sizeof(output)) == 0,
          "caller-authored descriptors cannot select host code or publish outputs");
}

int main(void) {
    scalar_queries();
    pipe_lifetime();
    forged_descriptor();
    if (failures)
        return 1;
    puts("Generated typed host bindings preserve query refusal and pipe lifetime");
    return 0;
}
