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
#include "../../../src/os/os_net.h"
#include "../../../src/base/xmalloc.h"
#include "../test_win_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

#define TEST(name)                                                                                \
    do {                                                                                          \
        printf("  Testing %s...", #name);                                                        \
        fflush(stdout);                                                                           \
        int failed_before = tests_failed;                                                         \
        int skipped_before = tests_skipped;                                                       \
        test_##name();                                                                            \
        if (tests_skipped != skipped_before) {                                                   \
            printf(" SKIPPED\n");                                                               \
        } else if (tests_failed == failed_before) {                                                      \
            printf(" PASSED\n");                                                                \
            tests_passed++;                                                                       \
        }                                                                                         \
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
    tests_skipped++;
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


/* A real loopback stream exercises the transport independently of the VM. */
typedef struct {
    XdapTransport *transport;
    xr_socket_t peer;
} DapStreamPair;

static bool stream_pair_create(DapStreamPair *pair) {
    memset(pair, 0, sizeof(*pair));
    pair->peer = XR_INVALID_SOCKET;
    if (xr_winsock_init() != 0)
        return false;
    xr_socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == XR_INVALID_SOCKET)
        goto fail;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *) &address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
        goto fail;
    socklen_t size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *) &address, &size) != 0)
        goto fail;
    pair->transport = xdap_transport_tcp_connect("127.0.0.1", ntohs(address.sin_port));
    if (!pair->transport)
        goto fail;
    pair->peer = accept(listener, NULL, NULL);
    if (pair->peer == XR_INVALID_SOCKET)
        goto fail;
    xr_closesocket(listener);
    return true;
fail:
    if (listener != XR_INVALID_SOCKET)
        xr_closesocket(listener);
    xdap_transport_free(pair->transport);
    pair->transport = NULL;
    xr_winsock_cleanup();
    return false;
}

static void stream_pair_free(DapStreamPair *pair) {
    if (pair->peer != XR_INVALID_SOCKET)
        xr_closesocket(pair->peer);
    xdap_transport_free(pair->transport);
    xr_winsock_cleanup();
}

static bool stream_ready(XdapTransport *transport) {
    fd_set inputs;
    FD_ZERO(&inputs);
    FD_SET((xr_socket_t) transport->read_fd, &inputs);
    struct timeval timeout = {1, 0};
    return select(transport->read_fd + 1, &inputs, NULL, NULL, &timeout) == 1;
}

static bool stream_send(DapStreamPair *pair, const char *bytes, size_t length) {
    while (length) {
        ssize_t sent = xr_socket_send(pair->peer, bytes, length);
        if (sent <= 0)
            return false;
        bytes += (size_t) sent;
        length -= (size_t) sent;
    }
    return stream_ready(pair->transport);
}

static void test_transport_fragmented_frame(void) {
    DapStreamPair pair;
    ASSERT(stream_pair_create(&pair));
    bool blocked = false;
    size_t length = 0;
    char *message = NULL;
    bool ok = stream_send(&pair, "Content-Length: 3\r\n", 19);
    if (ok) {
        message = xdap_transport_try_read(pair.transport, &length, &blocked);
        ok = !message && blocked && xdap_transport_is_connected(pair.transport);
    }
    xr_free(message);
    message = NULL;
    if (ok)
        ok = stream_send(&pair, "\r\nA\0B", 5);
    if (ok) {
        message = xdap_transport_try_read(pair.transport, &length, &blocked);
        ok = message && !blocked && length == 3 && memcmp(message, "A\0B", 3) == 0 &&
             message[length] == '\0';
    }
    xr_free(message);
    stream_pair_free(&pair);
    ASSERT(ok);
}

static void test_transport_peer_eof(void) {
    DapStreamPair pair;
    ASSERT(stream_pair_create(&pair));
    xr_closesocket(pair.peer);
    pair.peer = XR_INVALID_SOCKET;
    bool blocked = true;
    size_t length = 0;
    bool ok = stream_ready(pair.transport);
    char *message = ok ? xdap_transport_try_read(pair.transport, &length, &blocked) : NULL;
    ok = ok && !message && !blocked && !xdap_transport_is_connected(pair.transport);
    xr_free(message);
    stream_pair_free(&pair);
    ASSERT(ok);
}

static void test_transport_partial_frame_eof(void) {
    DapStreamPair pair;
    ASSERT(stream_pair_create(&pair));
    const char partial[] = "Content-Length: 10\r\n\r\n{}";
    bool blocked = false;
    size_t length = 0;
    bool ok = stream_send(&pair, partial, sizeof(partial) - 1);
    char *message = ok ? xdap_transport_try_read(pair.transport, &length, &blocked) : NULL;
    ok = ok && !message && blocked;
    xr_free(message);
    xr_closesocket(pair.peer);
    pair.peer = XR_INVALID_SOCKET;
    message = NULL;
    if (ok && stream_ready(pair.transport)) {
        message = xdap_transport_try_read(pair.transport, &length, &blocked);
        ok = !message && !blocked && !xdap_transport_is_connected(pair.transport);
    } else {
        ok = false;
    }
    xr_free(message);
    stream_pair_free(&pair);
    ASSERT(ok);
}

static void test_transport_buffered_frame_eof(void) {
    DapStreamPair pair;
    ASSERT(stream_pair_create(&pair));
    const char complete[] = "Content-Length: 2\r\n\r\n{}";
    /* A previous socket read may already have queued the final complete frame. */
    memcpy(pair.transport->read_buf, complete, sizeof(complete));
    pair.transport->read_len = sizeof(complete) - 1;
    xr_closesocket(pair.peer);
    pair.peer = XR_INVALID_SOCKET;
    bool blocked = false;
    size_t length = 0;
    bool ok = stream_ready(pair.transport);
    char *message = ok ? xdap_transport_try_read(pair.transport, &length, &blocked) : NULL;
    ok = ok && message && length == 2 && memcmp(message, "{}", 2) == 0 && !blocked;
    xr_free(message);
    message = NULL;
    if (ok) {
        message = xdap_transport_try_read(pair.transport, &length, &blocked);
        ok = !message && !blocked && !xdap_transport_is_connected(pair.transport);
    }
    xr_free(message);
    stream_pair_free(&pair);
    ASSERT(ok);
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
    TEST(transport_fragmented_frame);
    TEST(transport_peer_eof);
    TEST(transport_partial_frame_eof);
    TEST(transport_buffered_frame_eof);

    printf("\n");
    printf("Results: %d passed, %d failed, %d skipped\n", tests_passed, tests_failed, tests_skipped);

    return tests_failed > 0 ? 1 : 0;
}
