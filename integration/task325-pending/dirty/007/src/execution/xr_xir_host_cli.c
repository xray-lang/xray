/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_host_cli.c - One hosted entry policy for generated module programs
 *
 * KEY CONCEPT:
 *   Result owners outlive instance cleanup. Diagnostics borrow those owners
 *   and use the same formatter as other execution consumers.
 */
#include "xr_xir_host_cli.h"
#include "xr_xir_host_time.h"
#include "../xir/xxir_error.h"
#include "../xir/xxir_format.h"
#include "../xir/xxir_output.h"
#include <stdlib.h>
#if XR_OS_WINDOWS
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

typedef struct HostReportSink { XrValueFormatSink sink; bool failed; } HostReportSink;
static int host_report_write(void *context, const void *bytes, size_t length) {
    HostReportSink *sink = context;
    if (sink->failed) return 0;
    if (!sink->sink.write(sink->sink.context, bytes, length)) sink->failed = true;
    return !sink->failed;
}

XR_FUNC XrXirHostReportStatus xr_xir_host_report_result(const XrXirCallResult *result,
    XrValueFormatSink sink, bool color) {
    if (!sink.write || !xr_xir_call_result_valid(result)) return XR_XIR_HOST_REPORT_BAD_ARGUMENT;
    HostReportSink capture = {sink, false};
    XrValueFormatSink checked = {&capture, host_report_write};
    int written;
    if (result->status == XR_XIR_CALL_THROWN) {
        XrXirValue concrete = {0};
        const XrXirValue *value = &result->value;
        if (value->type == XR_XIR_ERROR) {
            if (!xr_xir_error_borrow(value, &concrete)) return XR_XIR_HOST_REPORT_BAD_ARGUMENT;
            value = &concrete;
        }
        written = xr_value_format_uncaught(xr_xir_value_format_reader(),
            (XrValueFormatNode){value, 0}, checked, 0, color);
    } else if (xr_xir_call_panic_status(result->status)) {
        char scratch[XR_XIR_PANIC_MESSAGE_CAPACITY];
        XrErrorCoreMessageView message = {0};
        if (!xr_xir_panic_message_borrow(&result->panic, scratch, sizeof(scratch), &message))
            return XR_XIR_HOST_REPORT_BAD_ARGUMENT;
        written = xr_value_format_panic(checked, result->panic.detail.code, 0, 0, 0,
            1, (const uint8_t *)message.message, message.message_len, color);
    } else {
        return result->status == XR_XIR_CALL_RETURNED ? XR_XIR_HOST_REPORT_OK : XR_XIR_HOST_REPORT_BAD_ARGUMENT;
    }
    return written ? XR_XIR_HOST_REPORT_OK :
        capture.failed ? XR_XIR_HOST_REPORT_IO : XR_XIR_HOST_REPORT_BAD_ARGUMENT;
}

static bool host_use_color(void) {
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0]) return false;
#if XR_OS_WINDOWS
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

static bool host_binary_streams(void) {
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return false;
    if (_setmode(_fileno(stderr), _O_BINARY) == -1) return false;
#endif
    return true;
}

static XrXirOutputStatus host_output_bytes(void *context, XrXirOutputStream stream,
    const char *bytes, size_t length) {
    (void)context;
    FILE *file = stream == XR_XIR_STDOUT ? stdout : stream == XR_XIR_STDERR ? stderr : NULL;
    if (!file) return XR_XIR_OUTPUT_BAD_ABI;
    return fwrite(bytes, 1, length, file) == length && fflush(file) == 0 ?
        XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
}

static int host_failure(const char *message, int code) {
    (void)fputs(message, stderr);
    (void)fflush(stderr);
    return code;
}

static int host_terminal(XrXirCallStatus status, const XrXirCallResult *result) {
    if (!xr_xir_call_result_valid(result))
        return host_failure("XR_RUN_6002: canonical execution returned an invalid owned outcome\n", 4);
    if (status == XR_XIR_CALL_THROWN || xr_xir_call_panic_status(status)) {
        if (result->status != status)
            return host_failure("XR_RUN_6002: canonical cleanup failed without an owned outcome\n", 4);
        XrXirHostReportStatus reported = xr_xir_host_report_result(result,
            (XrValueFormatSink){stderr, xr_value_format_file_write}, host_use_color());
        if (reported != XR_XIR_HOST_REPORT_OK)
            (void)fputs(status == XR_XIR_CALL_THROWN ?
                "XR_RUN_6005: cannot render uncaught typed error\n" :
                "XR_RUN_6005: cannot render uncaught typed panic\n", stderr);
        (void)fflush(stderr);
        return 1;
    }
    if (status == XR_XIR_CALL_RETURNED && result->status == status &&
        xr_xir_call_result_valid(result) && result->value.type == XR_XIR_I64 && !result->value.payload)
        return 0;
    if (status == XR_XIR_CALL_LIMIT)
        return host_failure("XR_RUN_6003: canonical execution exceeded its resource budget\n", 1);
    if (status == XR_XIR_CALL_OUTPUT_ERROR)
        return host_failure("XR_RUN_6002: canonical output failed\n", 1);
    if (status == XR_XIR_CALL_HOST_ERROR)
        return host_failure("XR_RUN_6007: a host clock or timer service failed\n", 1);
    if (status == XR_XIR_CALL_CANCELLED)
        return host_failure("XR_RUN_6002: canonical execution was cancelled\n", 1);
    return host_failure("XR_RUN_6002: canonical execution returned an invalid or unavailable outcome\n", 4);
}

XR_FUNC int xr_xir_host_program_main(XrXirProgram *owned_program, uint32_t entry) {
    if (!owned_program) return 4;
    if (!host_binary_streams()) {
        xr_xir_compile_program_drop(owned_program);
        return 4;
    }
    XrXirInstanceConfig config;
    if (xr_xir_instance_config_init(&config, sizeof(config)) != XR_XIR_CALL_READY) {
        xr_xir_compile_program_drop(owned_program);
        return host_failure("XR_RUN_6002: cannot initialize execution configuration\n", 4);
    }
    /* An ordinary process runs until it finishes or the user stops it; the embedding
     * default poll cap protects hosts that schedule many programs and does not apply. */
    config.poll_limit = UINT64_MAX;
    xr_xir_host_time_provider(&config.time);
    XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION, 0, host_output_bytes, NULL, config.value_limit};
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, xr_xir_output_render, &sink};
    XrXirCallResult result = {0};
    XrXirHostExecutionRequest request = {owned_program, &config, entry, NULL, 0};
    XrXirCallStatus status = xr_xir_host_execute(&request, &result);
    xr_xir_compile_program_drop(owned_program);
    int exit_code = host_terminal(status, &result);
    xr_xir_call_result_drop(&result);
    return exit_code;
}

XR_FUNC int xr_xir_host_main(const XrXirProgramSpec *spec) {
    const XrCompileResourceLimits limits = {UINT64_C(64) << 20, UINT64_C(16) << 20, UINT64_C(1) << 30};
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &context.resources);
    if (opened != XR_COMPILE_RESOURCE_OK)
        return host_binary_streams() ? host_failure("XR_RUN_6002: cannot initialize program admission\n",
            opened == XR_COMPILE_RESOURCE_BUDGET ? 1 : 4) : 4;
    XrXirProgram *program = NULL;
    XrXirStatus sealed = xr_xir_compile_program_seal(&context, spec, &program);
    xr_compile_resources_release(context.resources);
    if (sealed != XR_XIR_OK)
        return host_binary_streams() ? host_failure("XR_RUN_6002: cannot admit generated program\n",
            sealed == XR_XIR_BUDGET ? 1 : 4) : 4;
    return xr_xir_host_program_main(program, spec->declarations->entry_function);
}
