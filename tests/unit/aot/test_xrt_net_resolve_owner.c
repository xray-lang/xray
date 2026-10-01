/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_net_resolve_owner.c - DNS request failure and cancellation ownership
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>

static int allocation_attempts;
static int live_allocations;
static int reject_allocation = -1;

static void *request_test_malloc(size_t size) {
    int attempt = allocation_attempts++;
    if (attempt == reject_allocation)
        return NULL;
    void *result = xr_malloc(size);
    if (result)
        live_allocations++;
    return result;
}

static void request_test_free(void *data) {
    if (data)
        live_allocations--;
    xr_free(data);
}

#define XRT_MALLOC(sz) request_test_malloc(sz)
#define XRT_CALLOC(n, sz) xr_calloc((n), (sz))
#define XRT_REALLOC(p, sz) xr_realloc((p), (sz))
#define XRT_FREE(p) request_test_free(p)
#include "aot/xrt_coll.h"
#include "coro/xaot_coro.h"

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

static void test_request_allocation_failures(void) {
    for (int fail_at = 0; fail_at < 2; fail_at++) {
        allocation_attempts = 0;
        reject_allocation = fail_at;
        REQUIRE(xrt_net_resolve_request_new("127.0.0.1", 9) == NULL);
        REQUIRE(live_allocations == 0);
    }
    reject_allocation = -1;
}

static void test_submit_rejection_releases_only_pool_owner(void) {
    xrt_net_resolve_request_t *request = xrt_net_resolve_request_new("127.0.0.1", 9);
    REQUIRE(request && live_allocations == 2);
    xrt_net_resolve_request_retain(request);
    XrAotResult result = xr_aot_async_submit(NULL, xrt_net_resolve_request_invoke, request,
                                             xrt_net_resolve_request_release);
    REQUIRE(result.kind == XR_AOT_RUN_ERROR && atomic_load(&request->owners) == 1u);
    REQUIRE(live_allocations == 2);
    xrt_net_resolve_request_release(request);
    REQUIRE(live_allocations == 0);
}

static void test_cancelled_caller_and_late_completion(void) {
    char hostname[] = "127.0.0.1";
    xrt_net_resolve_request_t *request = xrt_net_resolve_request_new(hostname, 9);
    REQUIRE(request && live_allocations == 2 && !xrt_net_resolve_request_is_complete(request));
    xrt_net_resolve_request_retain(request);
    hostname[0] = 'x';
    xrt_net_resolve_request_release(request);
    REQUIRE(live_allocations == 2 && atomic_load(&request->owners) == 1u);
    xrt_net_resolve_request_invoke(request);
    REQUIRE(xrt_net_resolve_request_is_complete(request) && request->candidates.error == 0 &&
            request->candidates.count == 1 && request->candidates.addresses[0].family == AF_INET);
    char address[INET_ADDRSTRLEN];
    REQUIRE(inet_ntop(AF_INET, &request->candidates.addresses[0].addr.v4,
                      address, sizeof(address)) != NULL && strcmp(address, "127.0.0.1") == 0);
    xrt_net_resolve_request_release(request);
    REQUIRE(live_allocations == 0);
}

static void test_job_owner_released_before_resume(void) {
    xrt_net_resolve_request_t *request = xrt_net_resolve_request_new("", 0);
    REQUIRE(request && live_allocations == 2);
    xrt_net_resolve_request_retain(request);
    xrt_net_resolve_request_invoke(request);
    xrt_net_resolve_request_release(request);
    REQUIRE(xrt_net_resolve_request_is_complete(request) && atomic_load(&request->owners) == 1u &&
            request->candidates.error == EAI_NONAME && request->candidates.count == 0);
    xrt_net_resolve_request_release(request);
    REQUIRE(live_allocations == 0);
}

int main(void) {
    test_request_allocation_failures();
    test_submit_rejection_releases_only_pool_owner();
    test_cancelled_caller_and_late_completion();
    test_job_owner_released_before_resume();
    puts("DNS allocation, rejected submit, cancelled caller and late completion owners passed");
    return 0;
}
