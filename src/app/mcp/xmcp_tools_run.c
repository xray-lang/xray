/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_run.c - Owned bounded execution for the opt-in MCP runner
 *
 * KEY CONCEPT:
 *   SourceProduct transfers genuine code to a HostCall. Output never reaches
 *   process descriptors, and a terminal result outlives every producer owner.
 */
#include "xmcp_json.h"
#include "xmcp_tools_internal.h"
#include "xmcp_server.h"
#include "xmcp_run_source.h"
#include "../../base/xmalloc.h"
#include "../../execution/xr_xir_host_execution.h"
#include "../../execution/xr_xir_host_cli.h"
#include "../../execution/xr_xir_host_time.h"
#include "../../xir/xxir_output.h"
#include "../../os/os_time.h"
#include <stdio.h>
#include <string.h>

typedef struct RunBytes { char *bytes; size_t used, limit; bool truncated; } RunBytes;
typedef struct RunCapture { RunBytes out, error; } RunCapture;
typedef struct RunExecution {
    XrXirCallResult owned;
    XrXirCallStatus status;
    bool timed_out;
} RunExecution;
static bool bytes_open(RunBytes *buffer, size_t limit) {
    buffer->bytes = xr_malloc(limit + 1);
    if (!buffer->bytes) return false;
    buffer->bytes[0] = 0; buffer->limit = limit;
    return true;
}
static void bytes_append(RunBytes *buffer, const char *bytes, size_t length) {
    size_t remaining = buffer->limit - buffer->used;
    size_t copy = length < remaining ? length : remaining;
    if (copy) memcpy(buffer->bytes + buffer->used, bytes, copy);
    buffer->used += copy; buffer->bytes[buffer->used] = 0;
    if (length > copy) buffer->truncated = true;
}
static XrXirOutputStatus capture_write(void *context, XrXirOutputStream stream,
    const char *bytes, size_t length) {
    RunCapture *capture = context;
    RunBytes *buffer = stream == XR_XIR_STDOUT ? &capture->out :
        stream == XR_XIR_STDERR ? &capture->error : NULL;
    if (!buffer || (length && !bytes)) return XR_XIR_OUTPUT_BAD_ARGUMENT;
    bytes_append(buffer, bytes, length);
    return XR_XIR_OUTPUT_OK;
}
static int report_write(void *context, const void *bytes, size_t length) {
    bytes_append(context, bytes, length); return 1;
}
static bool execution_stopped(const XmcpServer *server, uint64_t start, uint64_t duration,
    RunExecution *execution) {
    uint64_t now = xr_time_monotonic_ns();
    if (!now || now < start) { execution->status = XR_XIR_CALL_HOST_ERROR; return true; }
    if (now - start >= duration) { execution->timed_out = true; return true; }
    if (server->shutdown) { execution->status = XR_XIR_CALL_CANCELLED; return true; }
    return false;
}
static XrXirCallStatus resume_wait(XrXirHostCall *call, XrXirInstanceResult observed,
    const XmcpServer *server, uint64_t start, uint64_t duration, RunExecution *execution) {
    XrXirWaitRequest request = {0}; XrXirHostWait wait = {0};
    XrXirCallStatus status = xr_xir_host_call_wait_request(call, observed.epoch,
        observed.outcome.wake, &request);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirHostWaitStatus waiting = xr_xir_host_wait_begin(&wait, &request);
    while (waiting == XR_XIR_HOST_WAIT_PENDING) {
        if (execution_stopped(server, start, duration, execution)) return XR_XIR_CALL_CANCEL_REQUESTED;
        waiting = xr_xir_host_wait_poll(&wait, UINT64_C(1000000));
    }
    return waiting == XR_XIR_HOST_WAIT_DUE ?
        xr_xir_host_call_resume(call, observed.epoch, observed.outcome.wake) : XR_XIR_CALL_HOST_ERROR;
}
static RunExecution execute_program(XrXirProgram *program, uint32_t entry, RunCapture *capture,
    const XmcpServer *server, uint64_t start, uint64_t duration) {
    RunExecution execution = {0};
    if (execution_stopped(server, start, duration, &execution)) return execution;
    XrXirInstanceConfig config;
    execution.status = xr_xir_instance_config_init(&config, sizeof(config));
    if (execution.status != XR_XIR_CALL_READY) return execution;
    XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION, 0, capture_write, capture, (size_t)config.value_limit};
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, xr_xir_output_render, &sink};
    xr_xir_host_time_provider(&config.time);
    XrXirHostExecutionRequest request = {program, &config, entry, NULL, 0};
    XrXirHostCall *call = NULL;
    execution.status = xr_xir_host_call_begin(&request, &call);
    if (execution.status != XR_XIR_CALL_READY) return execution;
    XrXirInstanceResult observed = xr_xir_host_call_step_bounded(call, 256);
    bool cancelling = false;
    while (observed.outcome.status == XR_XIR_CALL_READY || observed.outcome.status == XR_XIR_CALL_SUSPENDED) {
        if (!cancelling && execution_stopped(server, start, duration, &execution)) cancelling = true;
        if (!cancelling && observed.outcome.status == XR_XIR_CALL_SUSPENDED) {
            XrXirCallStatus resumed = resume_wait(call, observed, server, start, duration, &execution);
            if (resumed != XR_XIR_CALL_READY) {
                if (resumed != XR_XIR_CALL_CANCEL_REQUESTED) execution.status = resumed;
                cancelling = true;
            }
        }
        if (cancelling) {
            XrXirCallStatus cancelled = xr_xir_host_call_request_cancel(call);
            if (cancelled != XR_XIR_CALL_CANCEL_REQUESTED) { execution.status = cancelled; break; }
            do observed = xr_xir_host_call_step_bounded(call, 256);
            while (observed.outcome.status == XR_XIR_CALL_READY);
            break;
        }
        observed = xr_xir_host_call_step_bounded(call, 256);
    }
    XrXirCallStatus taken = xr_xir_host_call_take(call, &execution.owned);
    if (execution.status == XR_XIR_CALL_READY) execution.status = taken;
    XrXirCallStatus cleanup = xr_xir_host_call_drop(call);
    if (cleanup != XR_XIR_CALL_READY) {
        xr_xir_call_result_drop(&execution.owned); execution.status = cleanup;
    }
    return execution;
}
static XrJsonValue *run_result(bool ok, int exit_code, const RunCapture *capture,
    const RunExecution *execution, XrXirStatus source_status, const char *summary) {
    XrJsonValue *structured = xjson_new_object();
    XMCP_JSON_BOOL(structured, "ok", ok); XMCP_JSON_INT(structured, "exitCode", exit_code);
    const RunBytes *out = &capture->out, *error = &capture->error;
    xmcp_json_set(structured, "stdout", xjson_new_string_n(out->bytes ? out->bytes : "", out->used));
    xmcp_json_set(structured, "stderr", xjson_new_string_n(error->bytes ? error->bytes : "", error->used));
    XMCP_JSON_BOOL(structured, "timedOut", execution->timed_out);
    XMCP_JSON_BOOL(structured, "truncated", out->truncated);
    XMCP_JSON_BOOL(structured, "stderrTruncated", error->truncated);
    XMCP_JSON_INT(structured, "outputBytes", (int64_t)out->used);
    XMCP_JSON_INT(structured, "sourceStatus", source_status);
    XMCP_JSON_INT(structured, "runtimeStatus", execution->status);
    XrJsonValue *result = xmcp_make_text_structured_result(summary, structured, !ok);
    if (ok && out->used) {
        XrJsonValue *item = xjson_array_get(xjson_get_array(result, "content"), 0);
        xmcp_json_set(item, "text", xjson_new_string_n(out->bytes, out->used));
    }
    return xmcp_json_finish(result);
}
XR_FUNC XrJsonValue *xmcp_tool_xray_run(XmcpServer *server, const XmcpCallContext *ctx,
    XrJsonValue *arguments) {
    if (!server || !ctx || !arguments) return NULL;
    RunCapture capture = {0}; RunExecution execution = {0};
    XrXirStatus status = XR_XIR_BAD_STRUCTURE;
    char summary[384] = "Error: 'code' must not be empty";
    XrJsonValue *code = xjson_get(arguments, "code"), *identity = xjson_get(arguments, "moduleId");
    if (!xjson_is_string(code) || !code->string_len) goto publish;
    snprintf(summary, sizeof(summary), "Error: 'moduleId' must be an explicit valid memory-module identity");
    if (!xjson_is_string(identity) || memchr(identity->as.string, 0, identity->string_len)) goto publish;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, identity->as.string, NULL};
    if (!xr_module_identity_authority_valid(&authority)) goto publish;
    int64_t timeout = xjson_get_int_or(arguments, "timeoutMs", XMCP_TOOLS_RUN_TIMEOUT_DEFAULT_MS);
    if (timeout <= 0) timeout = XMCP_TOOLS_RUN_TIMEOUT_DEFAULT_MS;
    if (timeout > XMCP_TOOLS_RUN_TIMEOUT_HARD_MAX_MS) timeout = XMCP_TOOLS_RUN_TIMEOUT_HARD_MAX_MS;
    int64_t limit = xjson_get_int_or(arguments, "outputLimit", XMCP_TOOLS_RUN_OUTPUT_DEFAULT);
    if (limit <= 0) limit = XMCP_TOOLS_RUN_OUTPUT_DEFAULT;
    if (limit > XMCP_TOOLS_RUN_OUTPUT_HARD_MAX) limit = XMCP_TOOLS_RUN_OUTPUT_HARD_MAX;
    snprintf(summary, sizeof(summary), "Error: out of memory capturing output");
    if (!bytes_open(&capture.out, (size_t)limit) || !bytes_open(&capture.error, (size_t)limit)) {
        status = XR_XIR_OUT_OF_MEMORY; goto publish;
    }
    uint64_t start = xr_time_monotonic_ns();
    if (!start) {
        execution.status = XR_XIR_CALL_HOST_ERROR;
        snprintf(summary, sizeof(summary), "Error: monotonic host clock failed"); goto publish;
    }
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    XrCompileResourceLimits limits = xmcp_source_default_limits();
    XrCompileResourceStatus created = xr_compile_resources_new(&limits, &context.resources);
    status = created == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        created == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XmcpSourceRequest request = {code->as.string, code->string_len, identity->as.string, &context};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirProgram *program = NULL; uint32_t entry = 0;
    if (status == XR_XIR_OK) status = xmcp_run_source_product(&request, &product, &diagnostic);
    if (status == XR_XIR_OK) {
        entry = xr_xir_compile_source_product_facts(product)->entry;
        status = xr_xir_compile_source_product_vm_take(product, &program);
    }
    snprintf(summary, sizeof(summary), "Source failure (%d) at %d:%d: %s", status,
        diagnostic.source.line, diagnostic.source.column, diagnostic.source.message);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product); xr_compile_resources_release(context.resources);
    if (status == XR_XIR_OK) execution = execute_program(program, entry, &capture, server, start,
        (uint64_t)timeout * UINT64_C(1000000));
    xr_xir_compile_program_drop(program);
    if (status == XR_XIR_OK) {
        if (execution.owned.status == XR_XIR_CALL_THROWN || xr_xir_call_panic_status(execution.owned.status)) {
            XrValueFormatSink sink = {&capture.error, report_write};
            if (xr_xir_host_report_result(&execution.owned, sink, false) != XR_XIR_HOST_REPORT_OK)
                execution.status = XR_XIR_CALL_HOST_ERROR;
        }
        if (execution.timed_out) snprintf(summary, sizeof(summary), "Execution timed out after %lldms",
            (long long)timeout);
        else snprintf(summary, sizeof(summary), "Execution status %d; %zu stdout bytes captured%s",
            execution.status, capture.out.used, capture.out.truncated ? " (truncated)" : "");
    }
    xr_xir_call_result_drop(&execution.owned);
publish:;
    bool ok = status == XR_XIR_OK && execution.status == XR_XIR_CALL_RETURNED && !execution.timed_out;
    XrJsonValue *result = run_result(ok, ok ? 0 : -1, &capture, &execution, status, summary);
    xr_free(capture.out.bytes); xr_free(capture.error.bytes);
    return xmcp_json_finish(result);
}
