/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_code_alloc.c - Unit tests for JIT executable memory allocator
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "../../../src/jit/xm_code_alloc.h"
#include "../test_win_compat.h"

static void crash_handler(int sig) {
    const char *msg = "\n!!! SIGNAL received: ";
    write(2, msg, strlen(msg));
    if (sig == 11)
        write(2, "SIGSEGV\n", 8);
    else if (sig == 10)
        write(2, "SIGBUS\n", 7);
    else
        write(2, "OTHER\n", 6);
    _exit(128 + sig);
}

// arm64 machine code for: return 42
// mov w0, #42; ret
static const uint8_t arm64_ret42[] = {
    0x40, 0x05, 0x80, 0x52,  // mov w0, #42
    0xC0, 0x03, 0x5F, 0xD6,  // ret
};

// arm64 machine code for: return arg0 + arg1
// add w0, w0, w1; ret
static const uint8_t arm64_add[] = {
    0x00, 0x00, 0x01, 0x0B,  // add w0, w0, w1
    0xC0, 0x03, 0x5F, 0xD6,  // ret
};

static void test_basic_alloc(void) {
    fprintf(stderr, "  test_basic_alloc...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    // Allocate some memory
    void *p1 = xm_code_alloc(&alloc, 64, 16);
    assert(p1 != NULL);
    assert(((uintptr_t) p1 & 15) == 0);  // 16-byte aligned

    void *p2 = xm_code_alloc(&alloc, 128, 16);
    assert(p2 != NULL);
    assert(p2 != p1);
    assert(((uintptr_t) p2 & 15) == 0);

    // Check stats
    assert(xm_code_alloc_used(&alloc) == 64 + 128);
    assert(xm_code_alloc_total(&alloc) >= 64 + 128);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, " PASS\n");
}

static void test_large_alloc(void) {
    fprintf(stderr, "  test_large_alloc...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    // Allocate larger than default page size
    size_t large = XM_CODE_PAGE_SIZE * 2;
    void *p = xm_code_alloc(&alloc, large, 16);
    assert(p != NULL);

    // Should still be able to allocate more
    void *p2 = xm_code_alloc(&alloc, 64, 16);
    assert(p2 != NULL);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, " PASS\n");
}

static void test_execute_code(void) {
    fprintf(stderr, "  test_execute_code...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    // Allocate and write arm64 machine code
    void *code = xm_code_alloc(&alloc, sizeof(arm64_ret42), 16);
    assert(code != NULL);
    fprintf(stderr, "alloc=%p ", code);

#ifdef __APPLE__
    xm_code_make_writable(code, sizeof(arm64_ret42));
    fprintf(stderr, "writable ");
#endif

    memcpy(code, arm64_ret42, sizeof(arm64_ret42));
    fprintf(stderr, "copied ");

    xm_code_make_executable(code, sizeof(arm64_ret42));
    fprintf(stderr, "executable ");

    xm_code_flush_icache(code, sizeof(arm64_ret42));
    fprintf(stderr, "flushed ");

    // Cast to function pointer and call
    typedef int (*IntFunc)(void);
    IntFunc fn = (IntFunc) code;
    fprintf(stderr, "calling... ");
    int result = fn();
    fprintf(stderr, "result=%d ", result);

    assert(result == 42);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, "PASS\n");
}

static void test_execute_add(void) {
    fprintf(stderr, "  test_execute_add...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    void *code = xm_code_alloc(&alloc, sizeof(arm64_add), 16);
    assert(code != NULL);

#ifdef __APPLE__
    xm_code_make_writable(code, sizeof(arm64_add));
#endif

    memcpy(code, arm64_add, sizeof(arm64_add));
    xm_code_make_executable(code, sizeof(arm64_add));
    xm_code_flush_icache(code, sizeof(arm64_add));

    typedef int (*AddFunc)(int, int);
    AddFunc fn = (AddFunc) code;

    fprintf(stderr, "calling... ");
    assert(fn(10, 32) == 42);
    assert(fn(0, 0) == 0);
    assert(fn(100, -58) == 42);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, "PASS\n");
}

static void test_multiple_functions(void) {
    fprintf(stderr, "  test_multiple_functions...");

    XmCodeAlloc alloc;
    xm_code_alloc_init(&alloc);

    // Allocate two functions in the same page
    void *code1 = xm_code_alloc(&alloc, sizeof(arm64_ret42), 16);
    void *code2 = xm_code_alloc(&alloc, sizeof(arm64_add), 16);
    assert(code1 != NULL && code2 != NULL);
    assert(code1 != code2);

#ifdef __APPLE__
    xm_code_make_writable(code1, sizeof(arm64_ret42));
#endif

    memcpy(code1, arm64_ret42, sizeof(arm64_ret42));
    memcpy(code2, arm64_add, sizeof(arm64_add));

    xm_code_make_executable(code1, sizeof(arm64_ret42));
    xm_code_flush_icache(code1, sizeof(arm64_ret42));
    xm_code_flush_icache(code2, sizeof(arm64_add));

    typedef int (*IntFunc)(void);
    typedef int (*AddFunc)(int, int);

    assert(((IntFunc) code1)() == 42);
    assert(((AddFunc) code2)(10, 20) == 30);

    xm_code_alloc_destroy(&alloc);
    fprintf(stderr, "PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    signal(SIGSEGV, crash_handler);
#ifdef SIGBUS
    signal(SIGBUS, crash_handler);
#endif
    fprintf(stderr, "=== test_code_alloc ===\n");

    test_basic_alloc();
    test_large_alloc();

#ifdef __aarch64__
    test_execute_code();
    test_execute_add();
    test_multiple_functions();
#else
    fprintf(stderr, "  (skipping arm64 execution tests on non-arm64 platform)\n");
#endif

    fprintf(stderr, "All tests passed!\n");
    return 0;
}
