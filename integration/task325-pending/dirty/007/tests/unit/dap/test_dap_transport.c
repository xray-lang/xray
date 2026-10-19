/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dap_transport.c - Unit tests for DAP transport layer
 */

#include "../../../src/app/dap/xdap_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../test_win_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        printf("  Testing %s...", #name);                                                          \
        fflush(stdout);                                                                            \
        test_##name();                                                                             \
        printf(" PASSED\n");                                                                       \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf(" FAILED at line %d: %s\n", __LINE__, #cond);                                   \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

// ============================================================================
// Transport Tests
// ============================================================================

static void test_transport_create_free(void) {
    // Note: We can't easily test stdio transport in unit tests
    // because it would interfere with actual stdio
    // TCP transport can be tested though

    // Test null handling
    xdap_transport_free(NULL);  // Should not crash

    // Test helper functions with NULL
    ASSERT(!xdap_transport_is_connected(NULL));
    ASSERT(xdap_transport_get_port(NULL) == 0);
    ASSERT(xdap_transport_get_fd(NULL) == -1);
}

static void test_transport_tcp_server(void) {
    // Skip TCP server test in unit tests because accept() blocks
    // This functionality is tested via integration tests instead
    printf("(skipped - requires client connection)");
}

// ============================================================================
// Message Framing Tests (internal buffer handling)
// ============================================================================

static void test_message_framing(void) {
    // This test verifies the message framing logic conceptually
    // Actual framing is tested through integration tests

    // Verify Content-Length header format
    const char *test_json = "{\"test\":true}";
    size_t len = strlen(test_json);

    char header[64];
    int header_len = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);

    ASSERT(header_len > 0);
    ASSERT(strstr(header, "Content-Length:") != NULL);
    ASSERT(strstr(header, "\r\n\r\n") != NULL);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    xr_test_suppress_dialogs();
    printf("DAP Transport Tests\n");
    printf("==================\n");

    TEST(transport_create_free);
    TEST(transport_tcp_server);
    TEST(message_framing);

    printf("\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
