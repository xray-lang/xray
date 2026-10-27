/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compile_request_deadline.c - Deterministic monotonic deadline boundaries
 */
#include "base/xcompile_request.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)
static uint64_t observed_now;
static uint64_t clock_read(void) { return observed_now; }
#define xr_time_monotonic_ns clock_read
#include "base/xcompile_request.c"
#undef xr_time_monotonic_ns
static unsigned polls;
static bool cancelled(const void *context) { (void)context; ++polls; return true; }
int main(void) {
    XrCompileRequestOptions options = {cancelled, NULL, XR_COMPILE_DEADLINE_AFTER, 5};
    XrCompileRequestControl control = {0}; observed_now = UINT64_MAX - 4;
    CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OVERFLOW && !control.initialized);
    observed_now = 0;
    CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_CLOCK_FAILURE && !control.initialized);
    observed_now = 100; options.cancel_poll = NULL;
    OK(xr_compile_request_control_init(&options, &control));
    observed_now = 104; OK(xr_compile_request_control_poll(&control));
    observed_now = 105; CHECK(xr_compile_request_control_poll(&control) == XR_COMPILE_RESOURCE_DEADLINE);
    observed_now = 101; CHECK(xr_compile_request_control_poll(&control) == XR_COMPILE_RESOURCE_DEADLINE);
    XrCompileRequestControl fresh = {0}; options.cancel_poll = cancelled;
    OK(xr_compile_request_control_init(&options, &fresh));
    observed_now = 106; CHECK(xr_compile_request_control_poll(&fresh) == XR_COMPILE_RESOURCE_DEADLINE && !polls);
    XrCompileRequestControl no_clock = {0}; observed_now = 0;
    options = (XrCompileRequestOptions){0};
    OK(xr_compile_request_control_init(&options, &no_clock));
    OK(xr_compile_request_control_poll(&no_clock));
    XrCompileRequestControl failed_clock = {0}; observed_now = 200;
    options.deadline_mode = XR_COMPILE_DEADLINE_AFTER; options.timeout_ns = 5;
    OK(xr_compile_request_control_init(&options, &failed_clock));
    observed_now = 0; CHECK(xr_compile_request_control_poll(&failed_clock) == XR_COMPILE_RESOURCE_CLOCK_FAILURE);
    observed_now = 201; CHECK(xr_compile_request_control_poll(&failed_clock) == XR_COMPILE_RESOURCE_CLOCK_FAILURE);
    puts("request deadline boundaries: PASS"); return 0;
}
