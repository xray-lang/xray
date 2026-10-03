/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_time_services.c - Synchronous clocks, UTC offsets and timer suspension
 *
 * Real Source programs import the standard time module, run on the VM with
 * scripted providers, and are checked against independently computed values.
 */
#include "app/cli/xcli_canonical_source.h"
#include "execution/xr_xir_host_time.h"
#include "os/os_time.h"
#include "xir/xxir_output.h"
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct Capture {
    char text[512];
    size_t length;
} Capture;
static XrXirOutputStatus capture_write(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    Capture *capture = context;
    CHECK(stream == XR_XIR_STDOUT && capture->length + length < sizeof(capture->text));
    memcpy(capture->text + capture->length, bytes, length);
    capture->length += length;
    capture->text[capture->length] = 0;
    return XR_XIR_OUTPUT_OK;
}

/* Scripted provider: fixed readings and a configurable offset outcome. */
typedef struct Script {
    XrXirTimeStatus clock_status, offset_status;
    int64_t realtime, cpu, monotonic;
    unsigned clock_calls, offset_calls;
} Script;
static XrXirTimeStatus script_clock(void *context, XrXirClockKind clock, int64_t *nanoseconds) {
    Script *script = context;
    ++script->clock_calls;
    if (script->clock_status != XR_XIR_TIME_OK) return script->clock_status;
    *nanoseconds = clock == XR_XIR_CLOCK_REALTIME ? script->realtime :
        clock == XR_XIR_CLOCK_CPU ? script->cpu : script->monotonic;
    return XR_XIR_TIME_OK;
}
static XrXirTimeStatus script_offset(void *context, int64_t seconds, int64_t *minutes) {
    Script *script = context;
    ++script->offset_calls;
    if (script->offset_status != XR_XIR_TIME_OK) return script->offset_status;
    *minutes = seconds / 60 + 7;
    return XR_XIR_TIME_OK;
}

typedef struct Fixture {
    XrCompileResources *resources;
    XrXirProgram *program;
    uint32_t entry;
} Fixture;
static Fixture build(const char *entry_path, const char *stdlib) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    Fixture fixture = {0};
    CHECK(xr_compile_resources_new(&limits, &fixture.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {fixture.resources, xr_xir_compile_default_limits()};
    XrCliCompileSourceRequest request = {&context, entry_path, stdlib, NULL,
        xr_cli_compile_default_manifest_limits(), {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    XrCliCompileSourceDiagnostic diagnostic = {0};
    XrCliCompileSourceStatus status = xr_cli_compile_source_build(&request, &product, &diagnostic);
    if (status != XR_CLI_COMPILE_SOURCE_OK) {
        char text[512];
        xr_cli_compile_source_diagnostic_format(&diagnostic, text, sizeof(text));
        fprintf(stderr, "build failed: %s\n", text);
    }
    CHECK(status == XR_CLI_COMPILE_SOURCE_OK);
    fixture.entry = xr_xir_compile_source_product_facts(product)->entry;
    CHECK(xr_xir_compile_source_product_vm_take(product, &fixture.program) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    xr_cli_compile_source_diagnostic_free(&diagnostic);
    return fixture;
}
static XrXirInstance *start(const Fixture *fixture, Capture *capture, const XrXirTimeProvider *provider) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    /* The sink is borrowed for the instance lifetime, which outlives this call. */
    static XrXirOutputSink sink;
    sink = (XrXirOutputSink) {XR_XIR_CALL_ABI_VERSION, 0, capture_write, capture, config.value_limit};
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, xr_xir_output_render, &sink};
    config.poll_limit = UINT64_MAX;
    if (provider) config.time = *provider;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(fixture->program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, fixture->entry, NULL, 0) == XR_XIR_CALL_READY);
    return instance;
}
static XrXirInstanceResult run(XrXirInstance *instance) {
    XrXirInstanceResult result;
    do result = xr_xir_instance_poll_bounded(instance, 4096);
    while (result.outcome.status == XR_XIR_CALL_READY);
    return result;
}
static void finish(Fixture *fixture, XrXirInstance *instance) {
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(fixture->program);
    xr_compile_resources_release(fixture->resources);
}

static void clocks(const char *path, const char *stdlib) {
    Script script = {XR_XIR_TIME_OK, XR_XIR_TIME_OK, INT64_C(1700000000123456789), INT64_C(42000000),
        INT64_C(5123456789), 0, 0};
    XrXirTimeProvider provider = {XR_XIR_CALL_ABI_VERSION, 0, script_clock, script_offset, &script};
    Fixture fixture = build(path, stdlib);
    Capture capture = {{0}, 0};
    XrXirInstance *instance = start(&fixture, &capture, &provider);
    XrXirInstanceResult result = run(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    /* nanos, micros and ms share the monotonic clock; wall ms and CPU ms scale their own readings. */
    CHECK(!strcmp(capture.text, "5123456789\n5123456\n5123\n1700000000123\n42\n7\n"));
    CHECK(script.clock_calls == 5 && script.offset_calls == 1);
    finish(&fixture, instance);
    puts("scripted clocks, scaling and UTC offset PASS");
}
static void provider_failures(const char *clock_path, const char *offset_path, const char *stdlib) {
    /* An absent provider is fail-closed: every service is a host error, not a language panic. */
    Fixture fixture = build(clock_path, stdlib);
    Capture capture = {{0}, 0};
    XrXirInstance *instance = start(&fixture, &capture, NULL);
    XrXirInstanceResult result = run(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_HOST_ERROR && !capture.length);
    CHECK(!xr_xir_call_panic_status(result.outcome.status) && xr_xir_call_result_valid(&result.outcome));
    finish(&fixture, instance);
    /* Failed and out-of-contract readings are host errors; a negative nanosecond count is not a time. */
    for (unsigned mode = 0; mode < 2; ++mode) {
        Script script = {mode ? XR_XIR_TIME_RANGE : XR_XIR_TIME_FAILED, XR_XIR_TIME_OK, -1, 0, 1, 0, 0};
        XrXirTimeProvider provider = {XR_XIR_CALL_ABI_VERSION, 0, script_clock, script_offset, &script};
        fixture = build(clock_path, stdlib);
        capture = (Capture) {{0}, 0};
        instance = start(&fixture, &capture, &provider);
        result = run(instance);
        CHECK(result.outcome.status == XR_XIR_CALL_HOST_ERROR && !capture.length && script.clock_calls == 1);
        finish(&fixture, instance);
    }
    {
        Script script = {XR_XIR_TIME_OK, XR_XIR_TIME_OK, -1, -1, -1, 0, 0};
        XrXirTimeProvider provider = {XR_XIR_CALL_ABI_VERSION, 0, script_clock, script_offset, &script};
        fixture = build(clock_path, stdlib);
        capture = (Capture) {{0}, 0};
        instance = start(&fixture, &capture, &provider);
        result = run(instance);
        CHECK(result.outcome.status == XR_XIR_CALL_HOST_ERROR && !capture.length);
        finish(&fixture, instance);
    }
    /* An offset the platform cannot represent is the numeric-range language fault. */
    Script range = {XR_XIR_TIME_OK, XR_XIR_TIME_RANGE, 0, 0, 1, 0, 0};
    XrXirTimeProvider provider = {XR_XIR_CALL_ABI_VERSION, 0, script_clock, script_offset, &range};
    fixture = build(offset_path, stdlib);
    capture = (Capture) {{0}, 0};
    instance = start(&fixture, &capture, &provider);
    result = run(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_NUMERIC_RANGE && !capture.length && range.offset_calls == 1);
    CHECK(xr_xir_call_panic_status(result.outcome.status) && xr_xir_call_result_valid(&result.outcome));
    CHECK(result.outcome.panic.detail.code == 422);
    finish(&fixture, instance);
    Script failed = {XR_XIR_TIME_OK, XR_XIR_TIME_FAILED, 0, 0, 1, 0, 0};
    provider.context = &failed;
    fixture = build(offset_path, stdlib);
    capture = (Capture) {{0}, 0};
    instance = start(&fixture, &capture, &provider);
    result = run(instance);
    CHECK(result.outcome.status == XR_XIR_CALL_HOST_ERROR);
    finish(&fixture, instance);
    /* Providers must be absent or complete; a half-filled one is refused before any execution. */
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.time = (XrXirTimeProvider) {XR_XIR_CALL_ABI_VERSION, 0, script_clock, NULL, NULL};
    fixture = build(offset_path, stdlib);
    XrXirInstance *refused = NULL;
    CHECK(xr_xir_instance_new(fixture.program, &config, &refused) == XR_XIR_CALL_BAD_ABI && !refused);
    config.time = (XrXirTimeProvider) {XR_XIR_CALL_ABI_VERSION + 1, 0, script_clock, script_offset, NULL};
    CHECK(xr_xir_instance_new(fixture.program, &config, &refused) == XR_XIR_CALL_BAD_ABI && !refused);
    xr_xir_compile_program_drop(fixture.program);
    xr_compile_resources_release(fixture.resources);
    puts("absent, failed, negative, range and malformed providers are classified exactly PASS");
}
static void sleeping(const char *path, const char *stdlib) {
    Script script = {XR_XIR_TIME_OK, XR_XIR_TIME_OK, 1, 1, 1, 0, 0};
    XrXirTimeProvider provider = {XR_XIR_CALL_ABI_VERSION, 0, script_clock, script_offset, &script};
    Fixture fixture = build(path, stdlib);
    Capture capture = {{0}, 0};
    XrXirInstance *instance = start(&fixture, &capture, &provider);
    XrXirInstanceResult first = run(instance);
    CHECK(first.outcome.status == XR_XIR_CALL_SUSPENDED && first.outcome.wake && !capture.length);
    XrXirWaitRequest request = {7, 7, 7};
    CHECK(xr_xir_instance_wait_request(instance, first.epoch + 1, first.outcome.wake, &request) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_wait_request(instance, first.epoch, first.outcome.wake + 1, &request) == XR_XIR_CALL_BAD_STATE);
    CHECK(request.kind == 7 && request.reserved == 7 && request.after_ms == 7);
    CHECK(xr_xir_instance_wait_request(NULL, first.epoch, first.outcome.wake, &request) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_wait_request(instance, first.epoch, first.outcome.wake, NULL) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_wait_request(instance, first.epoch, first.outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_TIMER_MS && !request.reserved && request.after_ms == 5);
    /* Resuming is the host's decision; a stale or wrong token never resumes the program. */
    CHECK(xr_xir_instance_resume(instance, first.epoch, first.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, first.epoch, first.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_wait_request(instance, first.epoch, first.outcome.wake, &request) == XR_XIR_CALL_BAD_STATE);
    /* Zero and negative durations never suspend; the next real timer is clamped to one day. */
    XrXirInstanceResult second = run(instance);
    CHECK(second.outcome.status == XR_XIR_CALL_SUSPENDED && second.outcome.wake != first.outcome.wake);
    CHECK(!strcmp(capture.text, "done\nagain\n"));
    CHECK(xr_xir_instance_wait_request(instance, second.epoch, second.outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_TIMER_MS && request.after_ms == XR_XIR_TIMER_MAX_MS);
    /* Cancelling a pending timer clears its request and the activation drains to cancellation. */
    CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_wait_request(instance, second.epoch, second.outcome.wake, &request) == XR_XIR_CALL_BAD_STATE);
    XrXirInstanceResult drained = run(instance);
    CHECK(drained.outcome.status == XR_XIR_CALL_CANCELLED);
    CHECK(!strcmp(capture.text, "done\nagain\n"));
    finish(&fixture, instance);
    puts("timer suspension, exact wait request, clamp, stale tokens and cancellation PASS");
}
static uint64_t now_ns(void) {
    uint64_t now = xr_time_monotonic_ns();
    CHECK(now);
    return now;
}
static void host_waits(void) {
    XrXirHostWait wait = {0};
    XrXirWaitRequest bad[] = {{XR_XIR_WAIT_NONE, 0, 0}, {XR_XIR_WAIT_TIMER_MS, 0, 0},
        {XR_XIR_WAIT_TIMER_MS, 0, XR_XIR_TIMER_MAX_MS + 1}, {XR_XIR_WAIT_TIMER_MS, 1, 5},
        {XR_XIR_WAIT_YIELD, 0, 1}, {99, 0, 0}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(xr_xir_host_wait_begin(&wait, &bad[i]) == XR_XIR_HOST_WAIT_BAD_ARGUMENT && !wait.active);
    CHECK(xr_xir_host_wait_begin(NULL, &bad[1]) == XR_XIR_HOST_WAIT_BAD_ARGUMENT);
    CHECK(xr_xir_host_wait_poll(&wait, 0) == XR_XIR_HOST_WAIT_BAD_ARGUMENT);
    XrXirWaitRequest yield = {XR_XIR_WAIT_YIELD, 0, 0};
    CHECK(xr_xir_host_wait_begin(&wait, &yield) == XR_XIR_HOST_WAIT_PENDING);
    CHECK(xr_xir_host_wait_poll(&wait, 0) == XR_XIR_HOST_WAIT_DUE);
    XrXirWaitRequest timer = {XR_XIR_WAIT_TIMER_MS, 0, 20};
    uint64_t started = now_ns();
    CHECK(xr_xir_host_wait_begin(&wait, &timer) == XR_XIR_HOST_WAIT_PENDING);
    CHECK(xr_xir_host_wait_poll(&wait, 0) == XR_XIR_HOST_WAIT_PENDING);
    unsigned polls = 0;
    XrXirHostWaitStatus status;
    /* A short sleep slice only delays readiness: every wake recomputes elapsed from the clock. */
    while ((status = xr_xir_host_wait_poll(&wait, UINT64_C(1000000))) == XR_XIR_HOST_WAIT_PENDING) CHECK(++polls < 1000);
    CHECK(status == XR_XIR_HOST_WAIT_DUE && now_ns() - started >= UINT64_C(20000000) && polls > 0);
    XrXirTimeProvider provider;
    xr_xir_host_time_provider(&provider);
    CHECK(xr_xir_time_provider_valid(&provider) && provider.abi_version == XR_XIR_CALL_ABI_VERSION);
    int64_t first = 0, second = 0, wall = 0, cpu = 0, offset = 0;
    CHECK(provider.clock(NULL, XR_XIR_CLOCK_MONOTONIC, &first) == XR_XIR_TIME_OK && first > 0);
    CHECK(provider.clock(NULL, XR_XIR_CLOCK_MONOTONIC, &second) == XR_XIR_TIME_OK && second >= first);
    CHECK(provider.clock(NULL, XR_XIR_CLOCK_REALTIME, &wall) == XR_XIR_TIME_OK && wall > INT64_C(1600000000000000000));
    CHECK(provider.clock(NULL, XR_XIR_CLOCK_CPU, &cpu) == XR_XIR_TIME_OK && cpu >= 0);
    CHECK(provider.clock(NULL, (XrXirClockKind) 9, &cpu) == XR_XIR_TIME_FAILED);
    CHECK(provider.utc_offset(NULL, 0, &offset) == XR_XIR_TIME_OK && offset >= -1440 && offset <= 1440);
    int64_t kept = 12345;
    CHECK(provider.utc_offset(NULL, INT64_MAX, &kept) == XR_XIR_TIME_RANGE && kept == 12345);
    puts("host timer readiness and operating-system provider PASS");
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    char clocks_path[1024], offset_path[1024], sleep_path[1024];
    CHECK(snprintf(clocks_path, sizeof(clocks_path), "%s/clocks.xr", argv[1]) > 0);
    CHECK(snprintf(offset_path, sizeof(offset_path), "%s/offset.xr", argv[1]) > 0);
    CHECK(snprintf(sleep_path, sizeof(sleep_path), "%s/sleep.xr", argv[1]) > 0);
    clocks(clocks_path, argv[2]);
    provider_failures(clocks_path, offset_path, argv[2]);
    sleeping(sleep_path, argv[2]);
    host_waits();
    return 0;
}
