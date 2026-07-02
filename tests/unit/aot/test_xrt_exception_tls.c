#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;

static void *test_malloc(size_t size) {
    return malloc(size);
}

static void *test_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

static void *test_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

static void test_free(void *ptr) {
    free(ptr);
}

static void *test_alloc_aligned(size_t size) {
    void *ptr = NULL;
    return posix_memalign(&ptr, XRT_DATA_ALIGN, size) == 0 ? ptr : NULL;
}

static void test_free_aligned(void *ptr) {
    free(ptr);
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
#include "../../../src/aot/xrt_defer.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static inline void xrt_dispatch_destructor(uint16_t type_id, void *obj) {
    (void) type_id;
    (void) obj;
}

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
    int pending_error_i;
    int has_exc_top;
    int has_defer_top;
} ThreadTlsSnapshot;

static void *worker_tls_probe(void *arg) {
    ThreadTlsSnapshot *snapshot = (ThreadTlsSnapshot *) arg;
    XrtExcFrame frame;
    XrtDeferScope scope;

    xrt_pending_error = XR_FROM_INT(22);
    xrt_exc_top = &frame;
    xrt_defer_enter(&scope);

    snapshot->pending_error_i = (int) XR_TO_INT(xrt_pending_error);
    snapshot->has_exc_top = xrt_exc_top == &frame;
    snapshot->has_defer_top = xrt_defer_top == &scope;

    xrt_defer_leave(&scope);
    xrt_exc_top = NULL;
    xrt_pending_error = XR_NULL_VAL;
    return NULL;
}

static void test_aot_exception_defer_state_is_thread_local(void) {
    XrtExcFrame main_frame;
    XrtDeferScope main_scope;
    ThreadTlsSnapshot snapshot = {0};
    pthread_t thread;

    xrt_pending_error = XR_FROM_INT(11);
    xrt_exc_top = &main_frame;
    xrt_defer_enter(&main_scope);

    ASSERT_TRUE(pthread_create(&thread, NULL, worker_tls_probe, &snapshot) == 0,
                "worker thread should start");
    ASSERT_TRUE(pthread_join(thread, NULL) == 0, "worker thread should join");

    ASSERT_TRUE(snapshot.pending_error_i == 22, "worker sees its own pending error");
    ASSERT_TRUE(snapshot.has_exc_top, "worker sees its own exception stack");
    ASSERT_TRUE(snapshot.has_defer_top, "worker sees its own defer stack");

    ASSERT_TRUE(XR_TO_INT(xrt_pending_error) == 11, "main pending error survives worker write");
    ASSERT_TRUE(xrt_exc_top == &main_frame, "main exception stack survives worker write");
    ASSERT_TRUE(xrt_defer_top == &main_scope, "main defer stack survives worker write");

    xrt_defer_leave(&main_scope);
    xrt_exc_top = NULL;
    xrt_pending_error = XR_NULL_VAL;
}

int main(void) {
    test_aot_exception_defer_state_is_thread_local();
    if (g_failed != 0) {
        fprintf(stderr, "test_xrt_exception_tls: %d passed, %d failed\n", g_passed, g_failed);
        return 1;
    }
    printf("test_xrt_exception_tls: %d passed\n", g_passed);
    return 0;
}
