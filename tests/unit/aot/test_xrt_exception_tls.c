#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../test_win_compat.h"

#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#endif

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;
static uint64_t g_heap_calls;

static void *test_malloc(size_t size) {
    g_heap_calls++;
    return malloc(size);
}

static void *test_calloc(size_t count, size_t size) {
    g_heap_calls++;
    return calloc(count, size);
}

static void *test_realloc(void *ptr, size_t size) {
    g_heap_calls++;
    return realloc(ptr, size);
}

static void test_free(void *ptr) {
    free(ptr);
}

static void *test_alloc_aligned(size_t size) {
    g_heap_calls++;
    return xr_test_alloc_aligned(size, XRT_DATA_ALIGN);
}

static void test_free_aligned(void *ptr) {
    xr_test_free_aligned(ptr);
}

#define XRT_MALLOC(sz) test_malloc(sz)
#define XRT_CALLOC(n, sz) test_calloc((n), (sz))
#define XRT_REALLOC(p, sz) test_realloc((p), (sz))
#define XRT_FREE(p) test_free(p)
#define XRT_ALLOC_ALIGNED(sz) test_alloc_aligned(sz)
#define XRT_FREE_ALIGNED(p) test_free_aligned(p)

#define XRT_IMPL
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-internal"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "../../../src/aot/xrt_exception.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

typedef struct ThreadTlsSnapshot {
    int saved_error_i;
    int has_exc_top;
    int cleanup_depth;
    int has_cleanup_barrier;
} ThreadTlsSnapshot;

static int g_defer_calls;

static XrValue defer_capture_probe(xrt_closure_t *closure) {
    if (!closure || closure->nupvals != 1 || XR_TO_INT(xrt_cell_get(closure->upvals[0])) != 37) {
        g_failed++;
        return XR_NULL_VAL;
    }
    g_defer_calls++;
    return XR_NULL_VAL;
}

static void test_defer_consumes_closure_and_captured_graph(void) {
    static const XrAotCallableDesc callable = {
        .target_id = 1,
        .effect_bits = 0,
        .signature_key = 1,
        .sync_entry = (void (*)(void)) defer_capture_probe,
    };
    XrtExecutionArena *arena = xrt_execution_current();
    uint64_t baseline = arena->live_objects;
    XrValue captured = xrt_cell_new(XR_FROM_INT(37));
    XrValue closure = xrt_closure_new(&callable, 1);
    ((xrt_closure_t *) closure.ptr)->upvals[0] = captured;
    ASSERT_TRUE(arena->live_objects == baseline + 2,
                "capture and defer closure are registered in the execution arena");

    XrtDeferScope scope;
    xrt_defer_enter(&scope);
    xrt_defer_push(&scope, closure);
    xrt_defer_leave(&scope);

    ASSERT_TRUE(g_defer_calls == 1, "defer callback runs exactly once");
    ASSERT_TRUE(arena->live_objects == baseline,
                "draining defer releases the closure and its captured graph");
}

static void *worker_tls_probe(void *arg) {
    ThreadTlsSnapshot *snapshot = (ThreadTlsSnapshot *) arg;
    XrtExcFrame frame;

    xrt_pending_error = XR_FROM_INT(22);
    xrt_exc_top = &frame;
    xrt_cleanup_enter();

    snapshot->saved_error_i = (int) XR_TO_INT(xrt_cleanup_saved_errors[0]);
    snapshot->has_exc_top = xrt_exc_top == &frame;
    snapshot->cleanup_depth = xrt_cleanup_depth;
    snapshot->has_cleanup_barrier = xrt_cleanup_exc_barriers[0] == &frame;

    xrt_cleanup_leave();
    xrt_exc_top = NULL;
    xrt_pending_error = XR_NULL_VAL;
    return NULL;
}

#ifdef _WIN32
static unsigned __stdcall worker_tls_probe_win(void *arg) {
    worker_tls_probe(arg);
    return 0;
}
#endif

static void test_aot_exception_cleanup_state_is_thread_local(void) {
    XrtExcFrame main_frame;
    ThreadTlsSnapshot snapshot = {0};
#ifdef _WIN32
    uintptr_t thread;
#else
    pthread_t thread;
#endif

    xrt_pending_error = XR_FROM_INT(11);
    xrt_exc_top = &main_frame;
    xrt_cleanup_enter();

#ifdef _WIN32
    thread = _beginthreadex(NULL, 0, worker_tls_probe_win, &snapshot, 0, NULL);
    ASSERT_TRUE(thread != 0, "worker thread should start");
    ASSERT_TRUE(WaitForSingleObject((HANDLE) thread, INFINITE) == WAIT_OBJECT_0,
                "worker thread should join");
    CloseHandle((HANDLE) thread);
#else
    ASSERT_TRUE(pthread_create(&thread, NULL, worker_tls_probe, &snapshot) == 0,
                "worker thread should start");
    ASSERT_TRUE(pthread_join(thread, NULL) == 0, "worker thread should join");
#endif

    ASSERT_TRUE(snapshot.saved_error_i == 22, "worker sees its own saved error");
    ASSERT_TRUE(snapshot.has_exc_top, "worker sees its own exception stack");
    ASSERT_TRUE(snapshot.cleanup_depth == 1, "worker sees its own cleanup depth");
    ASSERT_TRUE(snapshot.has_cleanup_barrier, "worker sees its own cleanup barrier");

    ASSERT_TRUE(XR_IS_NULL(xrt_pending_error), "main cleanup keeps its error channel isolated");
    ASSERT_TRUE(XR_TO_INT(xrt_cleanup_saved_errors[0]) == 11,
                "main saved error survives worker write");
    ASSERT_TRUE(xrt_exc_top == &main_frame, "main exception stack survives worker write");
    ASSERT_TRUE(xrt_cleanup_depth == 1, "main cleanup depth survives worker write");
    ASSERT_TRUE(xrt_cleanup_exc_barriers[0] == &main_frame,
                "main cleanup barrier survives worker write");

    xrt_cleanup_leave();
    ASSERT_TRUE(XR_TO_INT(xrt_pending_error) == 11, "main cleanup restores its pending error");
    xrt_exc_top = NULL;
    xrt_pending_error = XR_NULL_VAL;
}

static void test_static_cleanup_barrier_has_zero_heap_calls(void) {
    const uint32_t iterations = UINT32_C(1000000);
    g_heap_calls = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        xrt_cleanup_enter();
        xrt_cleanup_leave();
    }
    ASSERT_TRUE(g_heap_calls == 0, "one million cleanup barriers must not allocate");
    ASSERT_TRUE(xrt_cleanup_depth == 0, "cleanup barrier depth must return to zero");
}

int main(void) {
    test_aot_exception_cleanup_state_is_thread_local();
    test_static_cleanup_barrier_has_zero_heap_calls();
    if (g_failed != 0) {
        fprintf(stderr, "test_xrt_exception_tls: %d passed, %d failed\n", g_passed, g_failed);
        return 1;
    }
    printf("test_xrt_exception_tls: %d passed\n", g_passed);
    return 0;
}
