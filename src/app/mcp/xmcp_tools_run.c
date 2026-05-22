/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_run.c - The xray_run sandbox tool (opt-in)
 *
 * KEY CONCEPT:
 *   Runs a short Xray snippet inside a fresh isolate with stdout captured
 *   in memory (open_memstream on POSIX, tmpfile on Windows).  Quotas come
 *   from xmcp_tools_internal.h so the schema and the runner stay in sync;
 *   the import allowlist is intentionally narrow because the runner shares
 *   the MCP server's process and cannot give up netd / fs / cluster
 *   modules cleanly on timeout.
 */

#include "xmcp_tools_internal.h"
#include "xmcp_server.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "xray_isolate.h"
#include <stdio.h>
#include <string.h>

/* Modules user snippets may import. Anything not on this list is rejected at
 * the module loader and surfaces inside the script as a runtime import
 * failure.  Pure data / formatting / regex modules only.  Networking
 * (`net`/`http`/`ws`), local I/O (`io`/`os`/`path`), cluster orchestration,
 * and dlopen-based packages are off the table because the MCP runner runs in
 * the same process as the server and cannot give them up cleanly on timeout.
 *
 * `prelude` is included so an explicit `import prelude` in user code
 * succeeds; the isolate's bootstrap import (issued by isolate_init_full()
 * before the allowlist is configured) is not subject to filtering. */
static const char *const RUN_ALLOWED_MODULES[] = {
    "prelude", "math", "json", "string", "regex", "encoding", "base64", "url",        "datetime",
    "time",    "log",  "csv",  "toml",   "xml",   "yaml",     "types",  "test_yield",
};
#define RUN_ALLOWED_MODULES_COUNT (sizeof(RUN_ALLOWED_MODULES) / sizeof(RUN_ALLOWED_MODULES[0]))

/* In-memory stdout capture for xray_run.  Avoids touching /tmp on POSIX
 * (open_memstream backs the FILE* with a libc-allocated buffer) and falls
 * back to tmpfile() on Windows where open_memstream is not available. */
typedef struct {
    FILE *file;
#ifndef XR_OS_WINDOWS
    char *mem_buf;   /* libc-malloc'd, owned by open_memstream until fclose */
    size_t mem_size; /* bytes flushed into mem_buf */
#endif
} XmcpRunCapture;

static bool xmcp_run_capture_open(XmcpRunCapture *cap) {
    XR_DCHECK(cap != NULL, "xmcp_run_capture_open: NULL cap");
#ifdef XR_OS_WINDOWS
    cap->file = tmpfile();
    return cap->file != NULL;
#else
    cap->mem_buf = NULL;
    cap->mem_size = 0;
    cap->file = open_memstream(&cap->mem_buf, &cap->mem_size);
    return cap->file != NULL;
#endif
}

/* Closes the capture and copies up to `limit` bytes into a freshly
 * xr_malloc'd, NUL-terminated buffer.  *out_size receives the byte count
 * before truncation; *out_truncated is set when the actual output exceeded
 * `limit`.  Caller owns the returned buffer. */
static char *xmcp_run_capture_finish(XmcpRunCapture *cap, int64_t limit, int *out_size,
                                     bool *out_truncated) {
    XR_DCHECK(cap != NULL && cap->file != NULL, "xmcp_run_capture_finish: invalid cap");
    XR_DCHECK(out_size != NULL && out_truncated != NULL,
              "xmcp_run_capture_finish: NULL out params");
    fflush(cap->file);
#ifdef XR_OS_WINDOWS
    long total = ftell(cap->file);
    if (total < 0)
        total = 0;
    long copy = total > (long) limit ? (long) limit : total;
    *out_truncated = total > copy;
    char *buf = xr_malloc((size_t) copy + 1);
    if (!buf) {
        fclose(cap->file);
        return NULL;
    }
    if (copy > 0) {
        fseek(cap->file, 0, SEEK_SET);
        size_t nread = fread(buf, 1, (size_t) copy, cap->file);
        copy = (long) nread;
    }
    buf[copy] = '\0';
    *out_size = (int) copy;
    fclose(cap->file);
    return buf;
#else
    size_t total = cap->mem_size;
    size_t copy = total > (size_t) limit ? (size_t) limit : total;
    *out_truncated = total > copy;
    char *buf = xr_malloc(copy + 1);
    if (!buf) {
        fclose(cap->file);
        free(cap->mem_buf);
        return NULL;
    }
    if (copy > 0)
        memcpy(buf, cap->mem_buf, copy);
    buf[copy] = '\0';
    *out_size = (int) copy;
    fclose(cap->file);
    free(cap->mem_buf); /* allocated by open_memstream, must use libc free */
    return buf;
#endif
}

/* Build the structured tool result described by xmcp_schema_run_output(). */
static XrJsonValue *build_run_structured(bool ok, int exit_code, const char *stdout_text,
                                         int output_bytes, bool timed_out, bool truncated) {
    XrJsonValue *s = xjson_new_object();
    XJSON_SET_BOOL(s, "ok", ok);
    XJSON_SET_INT(s, "exitCode", exit_code);
    xjson_object_set(s, "stdout", xjson_new_string(stdout_text ? stdout_text : ""));
    XJSON_SET_BOOL(s, "timedOut", timed_out);
    XJSON_SET_BOOL(s, "truncated", truncated);
    XJSON_SET_INT(s, "outputBytes", output_bytes);
    return s;
}

XR_FUNC XrJsonValue *xmcp_tool_xray_run(XmcpServer *server, const XmcpCallContext *ctx,
                                        XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "xmcp_tool_xray_run: NULL server");
    XR_DCHECK(ctx != NULL, "xmcp_tool_xray_run: NULL ctx");
    XR_DCHECK(arguments != NULL, "xmcp_tool_xray_run: NULL arguments");
    (void) server;
    (void) ctx;

    const char *code = xjson_get_string(arguments, "code");
    if (!code || code[0] == '\0') {
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, false, false);
        return xmcp_make_text_structured_result("Error: 'code' must not be empty", structured,
                                                true);
    }

    /* Resolve quotas. Negative / zero / oversized values fall back to the
     * default rather than getting silently clamped to a confusing minimum. */
    int64_t timeout_ms =
        xjson_get_int_or(arguments, "timeoutMs", XMCP_TOOLS_RUN_TIMEOUT_DEFAULT_MS);
    if (timeout_ms <= 0)
        timeout_ms = XMCP_TOOLS_RUN_TIMEOUT_DEFAULT_MS;
    if (timeout_ms > XMCP_TOOLS_RUN_TIMEOUT_HARD_MAX_MS)
        timeout_ms = XMCP_TOOLS_RUN_TIMEOUT_HARD_MAX_MS;

    int64_t output_limit =
        xjson_get_int_or(arguments, "outputLimit", XMCP_TOOLS_RUN_OUTPUT_DEFAULT);
    if (output_limit <= 0)
        output_limit = XMCP_TOOLS_RUN_OUTPUT_DEFAULT;
    if (output_limit > XMCP_TOOLS_RUN_OUTPUT_HARD_MAX)
        output_limit = XMCP_TOOLS_RUN_OUTPUT_HARD_MAX;

    /* Per-call in-memory stdout capture; written to via
     * xray_isolate_set_stdout() so we never touch the process-wide fd 1
     * (which is the MCP transport) or the filesystem. */
    XmcpRunCapture capture;
    if (!xmcp_run_capture_open(&capture)) {
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, false, false);
        return xmcp_make_text_structured_result("Error: failed to create capture buffer",
                                                structured, true);
    }

    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso) {
        int dummy_size = 0;
        bool dummy_trunc = false;
        char *junk = xmcp_run_capture_finish(&capture, 0, &dummy_size, &dummy_trunc);
        xr_free(junk);
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, false, false);
        return xmcp_make_text_structured_result("Error: failed to create isolate", structured,
                                                true);
    }

    /* Sandbox configuration. Order matters: the allowlist applies to user
     * imports issued by xray_isolate_dostring(); the isolate's own prelude
     * bootstrap already ran during xray_isolate_new() and is not affected. */
    xray_isolate_set_stdout(iso, capture.file);
    xray_isolate_set_module_allowlist(iso, RUN_ALLOWED_MODULES, RUN_ALLOWED_MODULES_COUNT);
    xray_isolate_set_deadline_ms(iso, timeout_ms);

    /* No multicore runtime: xr_execute() falls back to the in-place
     * interpreter, which (since the VM back-edge fallback) keeps refilling
     * reductions instead of yielding to a non-existent scheduler. The
     * wall-clock deadline above remains the sole termination guarantee for
     * tight loops. Skipping the runtime saves the per-call thread spin-up. */
    int exec_result = xray_isolate_dostring(iso, code);
    bool timed_out = xray_isolate_timed_out(iso);

    xray_isolate_delete(iso);

    int output_bytes = 0;
    bool truncated = false;
    char *output = xmcp_run_capture_finish(&capture, output_limit, &output_bytes, &truncated);
    if (!output) {
        XrJsonValue *structured = build_run_structured(false, -1, "", 0, timed_out, truncated);
        return xmcp_make_text_structured_result("Error: out of memory", structured, true);
    }

    bool ok = (exec_result == 0) && !timed_out;
    XrJsonValue *structured =
        build_run_structured(ok, exec_result, output, output_bytes, timed_out, truncated);

    /* Compose a text summary so plain-text MCP clients see the outcome. */
    char summary[1024];
    if (timed_out) {
        snprintf(summary, sizeof(summary), "[timed out after %lldms] %s%s%d byte%s captured%s",
                 (long long) timeout_ms, output_bytes > 0 ? "\n" : "",
                 output_bytes > 0 ? output : "", output_bytes, output_bytes == 1 ? "" : "s",
                 truncated ? " (truncated)" : "");
    } else if (exec_result != 0) {
        snprintf(summary, sizeof(summary), "[exit code %d] %s%s%s", exec_result,
                 output_bytes > 0 ? "\n" : "", output_bytes > 0 ? output : "",
                 truncated ? "\n(truncated)" : "");
    } else if (output_bytes == 0) {
        snprintf(summary, sizeof(summary), "(no output)");
    } else {
        snprintf(summary, sizeof(summary), "%s%s", output, truncated ? "\n(truncated)" : "");
    }

    XrJsonValue *result = xmcp_make_text_structured_result(summary, structured, !ok);
    xr_free(output);
    return result;
}
