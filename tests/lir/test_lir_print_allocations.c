/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_print_allocations.c - A failed display map preserves the input
 */
#include "lir_test_programs.h"
#include "lir/xlir_print.h"
#include "lir/xlir_verify.h"
#include "base/xmalloc.h"

static bool fail_next;

static void *injected_allocate(size_t size) {
    if (fail_next) {
        fail_next = false;
        return NULL;
    }
    return xr_malloc(size);
}

#undef xr_malloc
#define xr_malloc(size) injected_allocate(size)
#include "lir/xlir_print.c"

static bool discard_text(void *context, const char *bytes, size_t length) {
    (void) context;
    (void) bytes;
    (void) length;
    return true;
}

int main(void) {
    XrLirModule *fib = test_fib(), *sum = test_sum();
    fail_next = true;
    CHECK(xr_lir_print(fib, discard_text, NULL) == XR_LIR_OUT_OF_MEMORY && !fail_next);
    OK(xr_lir_verify(fib, NULL));
    OK(xr_lir_print(fib, discard_text, NULL));
    OK(xr_lir_print(sum, discard_text, NULL));
    xr_lir_module_destroy(fib);
    xr_lir_module_destroy(sum);
    puts("LIR display-map allocation failure leaves the module reusable");
    return 0;
}
