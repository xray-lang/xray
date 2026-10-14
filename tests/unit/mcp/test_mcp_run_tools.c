/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_run_tools.c - Independent results from the production owned runner
 *
 * KEY CONCEPT:
 *   Requests die before JSON observation; compiler fault loops keep all
 *   failure sites and retry on the same finite ledger without refunds.
 */
#include "app/mcp/xmcp_run_source.h"
#include "app/mcp/xmcp_tools_internal.h"
#include "app/mcp/xmcp_server.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir/xir_source_compile_owner.h"
static void physical_zero(void) { CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes); }
static XrJsonValue *run(const char *code, size_t length, int timeout, int limit) {
    XmcpServer server = {0}; XmcpCallContext context = {0};
    XrJsonValue *args = xjson_new_object();
    xjson_object_set(args, "code", xjson_new_string_n(code, length));
    XJSON_SET_STRING(args, "moduleId", "mcp-run-tools");
    if (timeout) XJSON_SET_INT(args, "timeoutMs", timeout);
    if (limit) XJSON_SET_INT(args, "outputLimit", limit);
    XrJsonValue *result = xmcp_tool_xray_run(&server, &context, args);
    xjson_free(args); CHECK(result); physical_zero();
    return result;
}
static void expect_output(const char *code, const char *output, size_t length, int limit, bool truncated) {
    XrJsonValue *result = run(code, strlen(code), 0, limit);
    XrJsonValue *value = xjson_get_object(result, "structuredContent");
    if (xjson_get_bool(result, "isError")) {
        size_t size = 0; char *json = xjson_stringify(result, &size);
        fprintf(stderr, "%s\n", json ? json : "no JSON"); xr_free(json);
    }
    CHECK(value && xjson_get_bool(value, "ok") && !xjson_get_bool(result, "isError"));
    CHECK(xjson_get_int_or(value, "exitCode", -1) == 0);
    CHECK(xjson_get_int_or(value, "runtimeStatus", -1) == XR_XIR_CALL_RETURNED);
    CHECK(xjson_get_bool(value, "truncated") == truncated && !xjson_get_bool(value, "timedOut"));
    XrJsonValue *bytes = xjson_get(value, "stdout");
    CHECK(xjson_is_string(bytes) && bytes->string_len == length && !memcmp(bytes->as.string, output, length));
    CHECK(xjson_get_int_or(value, "outputBytes", -1) == (int64_t)length);
    size_t size = 0; char *json = xjson_stringify(result, &size); CHECK(json && size);
    xr_free(json); xjson_free(result);
}
static void task_wait_cases(void) {
    const char task_timer[] = "import time\nfn worker()->i64 { time.sleep(5); return 42 }\n"
        "fn main()->i64 { return await go worker() }\nprint(main())\n";
    expect_output(task_timer, "42\n", 3, 0, false);
    const char task_timeout[] = "import time\nfn worker()->i64 { time.sleep(2000); return 42 }\n"
        "fn main()->i64 { return await go worker() }\nprint(main())\n";
    XrJsonValue *task_result=run(task_timeout,sizeof(task_timeout)-1,50,0);
    XrJsonValue *task_value=xjson_get_object(task_result,"structuredContent");
    CHECK(xjson_get_bool(task_result,"isError") && xjson_get_bool(task_value,"timedOut"));
    CHECK(xjson_get_int_or(task_value,"runtimeStatus",-1)==XR_XIR_CALL_CANCELLED);
    CHECK(xjson_get_int_or(task_value,"outputBytes",-1)==0);
    xjson_free(task_result);
    expect_output("print(42)\n", "42\n", 3, 0, false);
    puts("MCP Task timer normal/cancelled terminal ownership and recovery PASS");
}
static void constructed_task_cases(void) {
    expect_output("fn worker()->Array<i64> { Coro.yield(); return [10,20] }\n"
        "fn main()->i64 { const task=go worker(); var first=await task; first[0]=99; const second=await task; return second[0] }\n"
        "print(main())\n", "10\n", 3, 0, false);
    expect_output("fn worker()->Array<Array<string>> { Coro.yield(); return [[\"owned\"]] }\n"
        "fn main()->string { const task=go worker(); var first=await task; first[0][0]=\"changed\"; const second=await task; return second[0][0] }\n"
        "print(main())\n", "owned\n", 6, 0, false);
    expect_output("fn worker()->(i64,string) { Coro.yield(); return (42,\"owned\") }\n"
        "fn main()->i64 { const task=go worker(); const first=await task; const second=await task; return second.0 }\n"
        "print(main())\n", "42\n", 3, 0, false);
    expect_output("fn leaf()->i64 { Coro.yield(); return 42 }\n"
        "fn worker()->Task<i64> { Coro.yield(); return go leaf() }\n"
        "fn main()->i64 { const outer=go worker(); const inner=await outer; return await inner }\n"
        "print(main())\n", "42\n", 3, 0, false);
    puts("MCP constructed Task array copy isolation, nested strings, tuple and nested Task PASS");
}
static void normal_cases(void) {
    expect_output("print(\"hello\")\n", "hello\n", 6, 0, false);
    expect_output("print(\"A\\0B\")\n", "A\0B\n", 4, 0, false);
    expect_output("fn twice(n:i64)->i64 { return n*2 }\nprint(twice(21))\n", "42\n", 3, 0, false);
    expect_output("print(\"ABCDEFGHIJKLMNOPQRSTUVWXYZ\")\n", "ABCDEFGHIJ", 10, 10, true);
    expect_output("print(\"isolated\")\n", "isolated\n", 9, 0, false);
    expect_output("import time\ntime.sleep(20)\nprint(\"after-timer\")\n", "after-timer\n", 12, 0, false);
    const char *blocked[] = {"net", "http", "io", "os", "process", "cluster", "ws", "path"};
    for (size_t i = 0; i < sizeof(blocked)/sizeof(blocked[0]); ++i) {
        char code[96]; snprintf(code, sizeof(code), "import %s\n", blocked[i]);
        XrJsonValue *result = run(code, strlen(code), 0, 0);
        XrJsonValue *value = xjson_get_object(result, "structuredContent");
        CHECK(xjson_get_bool(result, "isError") && !xjson_get_bool(value, "ok"));
        CHECK(!xjson_get_bool(value, "timedOut") && xjson_get_int_or(value, "exitCode", 0) != 0);
        CHECK(xjson_get_int_or(value, "sourceStatus", -1) == XR_XIR_BAD_STRUCTURE);
        xjson_free(result);
    }
    const char *refused[] = {"import \"./private\"\n", "import \"owner/name\"\n",
        "import {read} from io\n", "export * from io\n", "var x:i64 = \"bad\"\n", "var x = ;\n"};
    for (size_t i = 0; i < sizeof(refused)/sizeof(refused[0]); ++i) {
        XrJsonValue *result = run(refused[i], strlen(refused[i]), 0, 0);
        CHECK(xjson_get_bool(result, "isError")); xjson_free(result);
    }
    const char nul[] = "print(\"wrong\")\n\0print(\"hidden\")\n";
    XrJsonValue *result = run(nul, sizeof(nul)-1, 0, 0);
    CHECK(xjson_get_bool(result, "isError"));
    CHECK(xjson_get_int_or(xjson_get_object(result, "structuredContent"), "outputBytes", -1) == 0);
    xjson_free(result);
    result = run("", 0, 0, 0); CHECK(xjson_get_bool(result, "isError")); xjson_free(result);
    result = run("assert(false, \"owned-panic\")\n", 29, 0, 0);
    XrJsonValue *value = xjson_get_object(result, "structuredContent");
    CHECK(xjson_get_bool(result, "isError") && strstr(xjson_get_string(value, "stderr"), "owned-panic"));
    xjson_free(result);
    const char loop[] = "var i = 0\nwhile (i == 0) { var x = 1 }\n";
    result = run(loop, sizeof(loop)-1, 50, 0); value = xjson_get_object(result, "structuredContent");
    CHECK(xjson_get_bool(result, "isError") && xjson_get_bool(value, "timedOut"));
    xjson_free(result);
    const char timer[] = "import time\ntime.sleep(1000)\nprint(\"too-late\")\n";
    result = run(timer, sizeof(timer)-1, 50, 0); value = xjson_get_object(result, "structuredContent");
    CHECK(xjson_get_bool(result, "isError") && xjson_get_bool(value, "timedOut"));
    CHECK(xjson_get_int_or(value, "outputBytes", -1) == 0);
    xjson_free(result);
    expect_output("print(\"after-timeout\")\n", "after-timeout\n", 14, 0, false);
    puts("MCP owned run: normal, byte output, isolation, truncation, imports, panic, deadline and recovery PASS");
}
static void tool_faults(void) {
    const char code[] = "print(42)\n"; size_t sites = 0;
    for (size_t probe = 0; probe <= sites; ++probe) {
        source_fixture_compile_attempts = 0;
        source_fixture_compile_fail_at = probe ? probe - 1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrJsonValue *result = run(code, sizeof(code)-1, 30000, 0);
        XrJsonValue *value = xjson_get_object(result, "structuredContent"); CHECK(value);
        if (!probe) { CHECK(!xjson_get_bool(result, "isError")); sites = source_fixture_compile_attempts; CHECK(sites); }
        else {
            CHECK(source_fixture_compile_injected && xjson_get_bool(result, "isError"));
            CHECK(xjson_get_int_or(value, "sourceStatus", -1) == XR_XIR_OUT_OF_MEMORY);
            CHECK(xjson_get_int_or(value, "outputBytes", -1) == 0);
        }
        xjson_free(result); physical_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX;
    printf("MCP run: %zu complete compiler allocation failures, exact OOM and physical0\n", sites);
}
static XrXirStatus product_on(XrCompileResources *resources, XrXirSourceProduct **product,
    XrXirSourceProductDiagnostic *diagnostic) {
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    const char code[] = "print(42)\n";
    XmcpSourceRequest request = {code, sizeof(code)-1, "mcp-run-owner", &context};
    return xmcp_run_source_product(&request, product, diagnostic);
}
static void same_ledger_faults(void) {
    size_t sites = 0;
    for (size_t probe = 0; probe <= sites; ++probe) {
        XrCompileResourceLimits limits = xmcp_source_default_limits(); XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats baseline, failed, retried;
        CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
        source_fixture_compile_attempts = 0; source_fixture_compile_injected = false;
        source_fixture_compile_fail_at = probe ? probe - 1 : SIZE_MAX;
        XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
        XrXirStatus status = product_on(resources, &product, &diagnostic);
        if (!probe) { CHECK(status == XR_XIR_OK); sites = source_fixture_compile_attempts; CHECK(sites); }
        else { CHECK(status == XR_XIR_OUT_OF_MEMORY && !product && source_fixture_compile_injected); }
        source_fixture_compile_fail_at = SIZE_MAX;
        xr_xir_compile_source_product_free(product); product = NULL;
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        CHECK(xr_compile_resources_stats(resources, &failed) == XR_COMPILE_RESOURCE_OK);
        CHECK(failed.live_bytes == baseline.live_bytes && failed.work > baseline.work);
        CHECK(product_on(resources, &product, &diagnostic) == XR_XIR_OK);
        xr_xir_compile_source_product_free(product); xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        CHECK(xr_compile_resources_stats(resources, &retried) == XR_COMPILE_RESOURCE_OK);
        CHECK(retried.live_bytes == baseline.live_bytes && retried.work > failed.work);
        CHECK(retried.allocated_bytes > failed.allocated_bytes);
        xr_compile_resources_release(resources); physical_zero();
    }
    printf("MCP run helper: %zu complete failures and same-ledger retries, physical0\n", sites);
}
static void axes(void) {
    XrCompileResourceLimits limits = xmcp_source_default_limits(); XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    CHECK(product_on(resources, &product, &diagnostic) == XR_XIR_OK);
    XrCompileResourceStats observed; CHECK(xr_compile_resources_stats(resources, &observed) == XR_COMPILE_RESOURCE_OK);
    xr_xir_compile_source_product_free(product); xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_resources_release(resources); physical_zero();
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        limits = xmcp_source_default_limits();
        uint64_t exact = axis == 0 ? observed.allocated_bytes : axis == 1 ? observed.peak_bytes : observed.work;
        uint64_t boundary = delta < 0 ? exact - 1 : delta > 0 ? exact + 1 : exact;
        if (axis == 0) limits.allocated_bytes = boundary;
        else if (axis == 1) limits.live_bytes = boundary;
        else limits.work = boundary;
        resources = NULL; product = NULL; memset(&diagnostic, 0, sizeof(diagnostic));
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrXirStatus status = product_on(resources, &product, &diagnostic);
        CHECK(status == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK((status == XR_XIR_OK) == (product != NULL));
        xr_xir_compile_source_product_free(product); xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        xr_compile_resources_release(resources); physical_zero();
    }
    printf("MCP run exact/minus1/plus1 allocated=%llu peak=%llu work=%llu, physical0\n",
        (unsigned long long)observed.allocated_bytes, (unsigned long long)observed.peak_bytes,
        (unsigned long long)observed.work);
}
int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    if (argc == 2 && !strcmp(argv[1], "--task-wait")) { task_wait_cases(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--constructed-tasks")) { constructed_task_cases(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--resources")) { tool_faults(); same_ledger_faults(); axes(); return 0; }
    CHECK(argc == 1); normal_cases(); return 0;
}
