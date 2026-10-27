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
 *   failure sites and retry on the same memory owner after every failure.
 */
#include "app/mcp/xmcp_run_source.h"
#include "app/mcp/xmcp_tools_internal.h"
#include "app/mcp/xmcp_server.h"
#include "api/xsource_paths.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "mcp_compile_memory.h"
#include "mcp_timed_channel_cases.h"
#include "app/mcp/xmcp_tools_run.c"
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
static void ready_task_wait_case(const char *code) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileRequestOptions options={0};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XmcpSourceRequest request={code,strlen(code),"mcp-ready-task-wait",&context,&control};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    CHECK(xmcp_run_source_product(&request,&product,&diagnostic)==XR_XIR_OK);
    uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_resources_release(context.resources);
    memset(&context,0,sizeof(context));memset(&request,0,sizeof(request));memset(&control,0,sizeof(control));
    RunCapture capture={0};XmcpServer server={0};
    CHECK(bytes_open(&capture.out,XMCP_TOOLS_RUN_OUTPUT_DEFAULT));
    CHECK(bytes_open(&capture.error,XMCP_TOOLS_RUN_OUTPUT_DEFAULT));
    uint64_t start=xr_time_monotonic_ns();CHECK(start);
    RunExecution execution=execute_program(program,entry,&capture,&server,start,UINT64_C(50000000));
    CHECK(execution.status==XR_XIR_CALL_CANCELLED && execution.owned.status==XR_XIR_CALL_CANCELLED);
    CHECK(execution.timed_out && !capture.out.used && !capture.error.used);
    xr_xir_compile_program_drop(program);xr_xir_call_result_drop(&execution.owned);
    xr_free(capture.out.bytes);xr_free(capture.error.bytes);physical_zero();
    puts("same Source ready after request death: production Task/await execution50ms terminal CANCELLED; all owners released PASS");
}
static void task_wait_cases(void) {
    const char task_timer[] = "import time\nfn worker()->i64 { time.sleep(5); return 42 }\n"
        "fn main() throws(Error)->i64 { return try await go worker() }\nprint(try main())\n";
    expect_output(task_timer, "42\n", 3, 0, false);
    const char task_timeout[] = "import time\nfn worker()->i64 { time.sleep(2000); return 42 }\n"
        "fn main() throws(Error)->i64 { return try await go worker() }\nprint(try main())\n";
    XrJsonValue *task_result=run(task_timeout,sizeof(task_timeout)-1,50,0);
    XrJsonValue *task_value=xjson_get_object(task_result,"structuredContent");
    CHECK(xjson_get_bool(task_result,"isError") && xjson_get_bool(task_value,"timedOut"));
    size_t json_length=0;char *json=xjson_stringify(task_result,&json_length);
    CHECK(json && json_length);fprintf(stderr,"whole-request50ms: %s\n",json);xr_free(json);
    int64_t source_status=xjson_get_int_or(task_value,"sourceStatus",-1);
    if(source_status==XR_XIR_DEADLINE)
        CHECK(xjson_get_int_or(task_value,"runtimeStatus",-1)==XR_XIR_CALL_READY);
    else CHECK(source_status==XR_XIR_OK && xjson_get_int_or(task_value,"runtimeStatus",-1)==XR_XIR_CALL_CANCELLED);
    CHECK(xjson_get_int_or(task_value,"outputBytes",-1)==0);
    xjson_free(task_result);
    expect_output("print(42)\n", "42\n", 3, 0, false);
    ready_task_wait_case(task_timeout);
    puts("MCP Task timer normal, precise whole-request deadline stage, terminal ownership and recovery PASS");
}
static void constructed_task_cases(void) {
    expect_output("fn worker()->Array<i64> { Coro.yield(); return [10,20] }\n"
        "fn main() throws(Error)->i64 { const task=go worker(); var first=try await task; first[0]=99; const second=try await task; return second[0] }\n"
        "print(try main())\n", "10\n", 3, 0, false);
    expect_output("fn worker()->Array<Array<string>> { Coro.yield(); return [[\"owned\"]] }\n"
        "fn main() throws(Error)->string { const task=go worker(); var first=try await task; first[0][0]=\"changed\"; const second=try await task; return second[0][0] }\n"
        "print(try main())\n", "owned\n", 6, 0, false);
    expect_output("fn worker()->(i64,string) { Coro.yield(); return (42,\"owned\") }\n"
        "fn main() throws(Error)->i64 { const task=go worker(); const first=try await task; const second=try await task; return second.0 }\n"
        "print(try main())\n", "42\n", 3, 0, false);
    expect_output("fn leaf()->i64 { Coro.yield(); return 42 }\n"
        "fn worker()->Task<i64> { Coro.yield(); return go leaf() }\n"
        "fn main() throws(Error)->i64 { const outer=go worker(); const inner=try await outer; return try await inner }\n"
        "print(try main())\n", "42\n", 3, 0, false);
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
    const char *refused[] = {"import \"./private\"\n", "import \"owner/name\"\n", "import prelude\n",
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
static void tool_faults(const char *code) {
    size_t sites = 0;
    for (size_t probe = 0; probe <= sites; ++probe) {
        source_fixture_compile_attempts = 0;
        source_fixture_compile_fail_at = probe ? probe - 1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrJsonValue *result = run(code, strlen(code), 30000, 0);
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
static XrXirStatus product_on(XrCompileResources *resources, const char *code, XrXirSourceProduct **product,
    XrXirSourceProductDiagnostic *diagnostic) {
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XmcpSourceRequest request = {code, strlen(code), "mcp-run-owner", &context,NULL};
    return xmcp_run_source_product(&request, product, diagnostic);
}
static void same_owner_faults(const char *code) {
    size_t sites = 0;
    for (size_t probe = 0; probe <= sites; ++probe) {
        XrCompileResourceLimits limits = xmcp_source_default_limits(); limits.profile_allocations=true; XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats baseline, failed, retried;
        CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
        source_fixture_compile_attempts = 0; source_fixture_compile_injected = false;
        source_fixture_compile_fail_at = probe ? probe - 1 : SIZE_MAX;
        XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
        XrXirStatus status = product_on(resources, code, &product, &diagnostic);
        if (!probe) { CHECK(status == XR_XIR_OK); sites = source_fixture_compile_attempts; CHECK(sites); }
        else { CHECK(status == XR_XIR_OUT_OF_MEMORY && !product && source_fixture_compile_injected); }
        source_fixture_compile_fail_at = SIZE_MAX;
        xr_xir_compile_source_product_free(product); product = NULL;
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        CHECK(xr_compile_resources_stats(resources, &failed) == XR_COMPILE_RESOURCE_OK);
        CHECK(failed.held_bytes == baseline.held_bytes);
        CHECK(product_on(resources, code, &product, &diagnostic) == XR_XIR_OK);
        xr_xir_compile_source_product_free(product); xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        CHECK(xr_compile_resources_stats(resources, &retried) == XR_COMPILE_RESOURCE_OK);
        CHECK(retried.held_bytes == baseline.held_bytes);
        CHECK(retried.allocated_bytes > failed.allocated_bytes);
        xr_compile_resources_release(resources); physical_zero();
    }
    printf("MCP run helper: %zu complete failures and same-owner retries, physical0\n", sites);
}
static void axes(const char *code) {
    XrCompileResourceLimits limits = xmcp_source_default_limits(); limits.profile_allocations=true; XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    CHECK(product_on(resources, code, &product, &diagnostic) == XR_XIR_OK);
    XrCompileResourceStats observed; CHECK(xr_compile_resources_stats(resources, &observed) == XR_COMPILE_RESOURCE_OK);
    xr_xir_compile_source_product_free(product); xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_resources_release(resources); physical_zero();
    for (int delta=-1;delta<=1;++delta) {
        uint64_t boundary=delta<0?observed.peak_bytes-1:observed.peak_bytes+(unsigned)delta;
        limits=(XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,boundary,true};
        resources=NULL; product=NULL; memset(&diagnostic,0,sizeof(diagnostic));
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=product_on(resources,code,&product,&diagnostic);
        CHECK(status==(delta<0?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        CHECK((status==XR_XIR_OK)==(product!=NULL));
        xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        xr_compile_resources_release(resources);physical_zero();
    }
    printf("MCP run held peak=%llu minus1/exact/plus1, physical0\n",(unsigned long long)observed.peak_bytes);
}
static void path_status_cases(bool policy_limit) {
    const char code[]="print(42)\n";
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrCompileResources *resources=NULL;XrCompileResourceStats constructor={0},after={0};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(resources,&constructor)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources);physical_zero();resources=NULL;
    if (!policy_limit) {limits.memory_mode=XR_COMPILE_MEMORY_FINITE;limits.memory_limit=constructor.held_bytes;}
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrStdlibPath path={0};XrSourcePathsDiagnostic located={0};
    XrSourceCompileStatus selected=xr_source_stdlib_path(resources,&path,&located);
    printf("stdlib refusal actual=%u expected=%u io=%u held=%llu\n",(unsigned)selected,
        (unsigned)(policy_limit?XR_SOURCE_COMPILE_BUDGET:XR_SOURCE_COMPILE_MEMORY_LIMIT),
        (unsigned)located.io_status,(unsigned long long)constructor.held_bytes);fflush(stdout);
    CHECK(!path.path && !path.origin && selected==located.status);
    CHECK(located.io_status==(policy_limit?XR_OS_IO_OVERFLOW:XR_OS_IO_MEMORY_LIMIT));
    xr_source_paths_diagnostic_free(&located);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==constructor.held_bytes);
    xr_compile_resources_release(resources);physical_zero();resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=product_on(resources,code,&product,&diagnostic);
    XrXirStatus expected=policy_limit?XR_XIR_BUDGET:XR_XIR_MEMORY_LIMIT;
    printf("MCP refusal actual=%u expected=%u diagnostic=%u\n",(unsigned)status,(unsigned)expected,(unsigned)diagnostic.status);fflush(stdout);
    CHECK(!product && diagnostic.status==status && diagnostic.source.status==status);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==constructor.held_bytes);
    xr_compile_resources_release(resources);physical_zero();resources=NULL;
    CHECK(selected==(policy_limit?XR_SOURCE_COMPILE_BUDGET:XR_SOURCE_COMPILE_MEMORY_LIMIT));
    CHECK(status==expected);
    if (policy_limit) {puts("real oversized stdlib path keeps local budget classification and output/owner cleanup PASS");return;}
    limits=(XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_source_stdlib_path(resources,&path,&located)==XR_SOURCE_COMPILE_OK && path.path);
    xr_source_paths_diagnostic_free(&located);xr_compile_resources_release(resources);resources=NULL;
    CHECK(strlen(path.path));xr_source_stdlib_path_free(&path);physical_zero();
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(product_on(resources,code,&product,&diagnostic)==XR_XIR_OK && product);
    xr_compile_resources_release(resources);
    CHECK(xr_xir_compile_source_product_facts(product) && xr_xir_compile_source_product_dependencies(product));
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);physical_zero();
    puts("constructor-held finite memory refusal; actual stdlib/product positive after owner release; physical0 PASS");
}
int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    if (argc==2 && !strcmp(argv[1],"--path-memory")) {path_status_cases(false);return 0;}
    if (argc==2 && !strcmp(argv[1],"--path-policy")) {path_status_cases(true);return 0;}
    if (argc == 2 && !strcmp(argv[1], "--task-wait")) { task_wait_cases(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--constructed-tasks")) { constructed_task_cases(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--resources")) {
        tool_faults("print(42)\n"); same_owner_faults("print(42)\n"); axes("print(42)\n"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--channel-resources")) {
        const char code[]="fn send(ch:Channel<i64>) throws(SendResult)->() { try ch.send(42) }\n"
            "fn main() throws(Error)->i64 { const ch=Channel<i64>(); const task=go send(ch); "
            "const n=match(ch.recv()) { Recv.Value { value:item } -> item, Recv.Empty -> -1, "
            "Recv.Timeout -> -2, Recv.Closed -> -3 }; try await task; ch.close(); return n+ch.recvOr(0) }\nprint(try main())\n";
        expect_output(code,"42\n",3,0,false);
        tool_faults(code); same_owner_faults(code); axes(code); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--timed-channel")) {
        expect_output(mcp_timed_channel_code,"14177\n",6,0,false);
        puts("MCP real timed buffered/empty/Task/Unit outputs, natural deadlines and once evaluation PASS");return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--timed-channel-resources")) {
        tool_faults(mcp_timed_channel_code);same_owner_faults(mcp_timed_channel_code);
        axes(mcp_timed_channel_code);return 0;
    }
    CHECK(argc == 1); normal_cases(); return 0;
}
