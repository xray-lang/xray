/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_random_windows.c - OS request widths and fail-stop random filling
 */
#include "os/os_random.h"
#include <windows.h>
#include <bcrypt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static uintptr_t expected_address;
static size_t remaining;
static unsigned calls, fail_at;
static jmp_buf failure_return;

static NTSTATUS WINAPI checked_random(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer, ULONG count, ULONG flags) {
    REQUIRE(algorithm == NULL && flags == BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    REQUIRE((uintptr_t)buffer == expected_address);
    REQUIRE(count == (remaining > ULONG_MAX ? ULONG_MAX : (ULONG)remaining));
    REQUIRE(count != 0u);
    ++calls;
    if (calls == fail_at) return (NTSTATUS)-1;
    expected_address += count;
    remaining -= count;
    return 0;
}

static void intercepted_abort(void) {
    REQUIRE(fail_at != 0u && calls == fail_at);
    longjmp(failure_return, 1);
}

/* Compile the production loop with only its external OS and abort boundaries
 * intercepted. The ordinary symbol remains separately linked for the smoke test. */
#define BCryptGenRandom checked_random
#define xr_random_bytes checked_fill
#define abort intercepted_abort
#include "../../../src/os/win/random_win.c"
#undef abort
#undef xr_random_bytes
#undef BCryptGenRandom

int main(void) {
    size_t sizes[] = {0u, 1u, (size_t)ULONG_MAX - 1u, ULONG_MAX
#if UINTPTR_MAX > UINT32_MAX
        , (size_t)ULONG_MAX + 1u, (size_t)ULONG_MAX * 2u + 17u
#endif
    };
    size_t capacity = sizes[sizeof(sizes) / sizeof(sizes[0]) - 1u];
    unsigned char *reserved = VirtualAlloc(NULL, capacity, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(reserved != NULL);
    for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        remaining = sizes[i];
        expected_address = (uintptr_t)reserved;
        calls = fail_at = 0u;
        checked_fill(reserved, remaining);
        REQUIRE(remaining == 0u);
        REQUIRE(expected_address == (uintptr_t)reserved + sizes[i]);
        REQUIRE(calls == sizes[i] / ULONG_MAX + (sizes[i] % ULONG_MAX != 0u));
    }
#if UINTPTR_MAX > UINT32_MAX
    for (unsigned failure = 1u; failure <= 3u; ++failure) {
        remaining = capacity;
        expected_address = (uintptr_t)reserved;
        calls = 0u;
        fail_at = failure;
        if (!setjmp(failure_return)) {
            checked_fill(reserved, capacity);
            REQUIRE(false);
        }
        REQUIRE(calls == failure && remaining != 0u);
    }
#endif
    REQUIRE(VirtualFree(reserved, 0u, MEM_RELEASE));
    unsigned char real[66] = {0};
    real[0] = 0x5au;
    real[65] = 0xa5u;
    xr_random_bytes(NULL, 0u);
    xr_random_bytes(real + 1u, 64u);
    REQUIRE(real[0] == 0x5au && real[65] == 0xa5u);
    return 0;
}
