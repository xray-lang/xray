/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_transport.c - Unit tests for MCP stdio transport
 */

#include "../../../src/app/mcp/xmcp_transport_stdio.h"
#include "../../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "../test_win_compat.h"

#ifdef XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#define test_pipe(fds) _pipe((fds), 4096, _O_BINARY)
#define test_close(fd) _close((fd))
#define test_read(fd, buf, len) _read((fd), (buf), (unsigned int) (len))
#define test_write(fd, buf, len) _write((fd), (buf), (unsigned int) (len))
#else
#include <unistd.h>
#define test_pipe(fds) pipe((fds))
#define test_close(fd) close((fd))
#define test_read(fd, buf, len) read((fd), (buf), (len))
#define test_write(fd, buf, len) write((fd), (buf), (len))
#endif

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Testing %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASS\n");                                                                          \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL at line %d: %s\n", __LINE__, #cond);                                      \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

static bool write_all(int fd, const char *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = test_write(fd, data + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        total += (size_t) n;
    }
    return true;
}

static size_t read_all(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = test_read(fd, buf + total, cap - total - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        total += (size_t) n;
    }
    buf[total] = '\0';
    return total;
}

TEST(read_single_lf_message) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);
    const char *input = "{\"jsonrpc\":\"2.0\"}\n";
    ASSERT(write_all(fds[1], input, strlen(input)));
    test_close(fds[1]);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, fds[0], -1, 1024));
    char *message = NULL;
    size_t message_len = 0;
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_OK);
    ASSERT_STR_EQ(message, "{\"jsonrpc\":\"2.0\"}");
    ASSERT(message_len == strlen("{\"jsonrpc\":\"2.0\"}"));
    xr_free(message);
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_EOF);

    xmcp_stdio_destroy(&transport);
    test_close(fds[0]);
}

TEST(read_crlf_message) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);
    const char *input = "{\"method\":\"ping\"}\r\n";
    ASSERT(write_all(fds[1], input, strlen(input)));
    test_close(fds[1]);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, fds[0], -1, 1024));
    char *message = NULL;
    size_t message_len = 0;
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_OK);
    ASSERT_STR_EQ(message, "{\"method\":\"ping\"}");
    xr_free(message);

    xmcp_stdio_destroy(&transport);
    test_close(fds[0]);
}

TEST(read_embedded_nul_preserves_length) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);
    static const char input[] = "{\"id\":1}\0tail\n";
    static const char expected[] = "{\"id\":1}\0tail";
    ASSERT(write_all(fds[1], input, sizeof(input) - 1));
    test_close(fds[1]);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, fds[0], -1, 1024));
    char *message = NULL;
    size_t message_len = 0;
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_OK);
    ASSERT(message_len == sizeof(expected) - 1);
    ASSERT(memcmp(message, expected, message_len) == 0);
    ASSERT(strlen(message) < message_len);
    xr_free(message);

    xmcp_stdio_destroy(&transport);
    test_close(fds[0]);
}

TEST(read_multiple_messages) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);
    const char *input = "{\"id\":1}\n{\"id\":2}\n";
    ASSERT(write_all(fds[1], input, strlen(input)));
    test_close(fds[1]);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, fds[0], -1, 1024));
    char *message = NULL;
    size_t message_len = 0;
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_OK);
    ASSERT_STR_EQ(message, "{\"id\":1}");
    xr_free(message);
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_OK);
    ASSERT_STR_EQ(message, "{\"id\":2}");
    xr_free(message);

    xmcp_stdio_destroy(&transport);
    test_close(fds[0]);
}

TEST(content_length_is_plain_line) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);
    const char *input = "Content-Length: 2\r\n\r\n{}\n";
    ASSERT(write_all(fds[1], input, strlen(input)));
    test_close(fds[1]);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, fds[0], -1, 1024));
    char *message = NULL;
    size_t message_len = 0;
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_OK);
    ASSERT_STR_EQ(message, "Content-Length: 2");
    xr_free(message);
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) == XMCP_STDIO_READ_OK);
    ASSERT_STR_EQ(message, "");
    xr_free(message);

    xmcp_stdio_destroy(&transport);
    test_close(fds[0]);
}

TEST(reject_too_large_line) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);
    const char *input = "abcdef\n";
    ASSERT(write_all(fds[1], input, strlen(input)));
    test_close(fds[1]);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, fds[0], -1, 3));
    char *message = NULL;
    size_t message_len = 0;
    ASSERT(xmcp_stdio_read_message(&transport, &message, &message_len) ==
           XMCP_STDIO_READ_TOO_LARGE);
    ASSERT(message == NULL);

    xmcp_stdio_destroy(&transport);
    test_close(fds[0]);
}

TEST(write_message_appends_newline_only) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, -1, fds[1], 1024));
    const char *json = "{\"result\":{}}";
    ASSERT(xmcp_stdio_write_message(&transport, json, strlen(json)));
    test_close(fds[1]);

    char buf[128];
    size_t n = read_all(fds[0], buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_STR_EQ(buf, "{\"result\":{}}\n");
    ASSERT(strstr(buf, "Content-Length") == NULL);

    xmcp_stdio_destroy(&transport);
    test_close(fds[0]);
}

TEST(write_rejects_embedded_newline) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, -1, fds[1], 1024));
    const char *json = "{\"bad\":true}\n";
    ASSERT(!xmcp_stdio_write_message(&transport, json, strlen(json)));

    xmcp_stdio_destroy(&transport);
    test_close(fds[1]);
    test_close(fds[0]);
}

TEST(write_rejects_embedded_cr) {
    int fds[2];
    ASSERT(test_pipe(fds) == 0);

    XmcpStdioTransport transport;
    ASSERT(xmcp_stdio_init(&transport, -1, fds[1], 1024));
    const char *json = "{\"bad\":true}\r";
    ASSERT(!xmcp_stdio_write_message(&transport, json, strlen(json)));

    xmcp_stdio_destroy(&transport);
    test_close(fds[1]);
    test_close(fds[0]);
}

int main(void) {
    xr_test_suppress_dialogs();
    printf("=== MCP Transport Tests ===\n");

    RUN_TEST(read_single_lf_message);
    RUN_TEST(read_crlf_message);
    RUN_TEST(read_embedded_nul_preserves_length);
    RUN_TEST(read_multiple_messages);
    RUN_TEST(content_length_is_plain_line);
    RUN_TEST(reject_too_large_line);
    RUN_TEST(write_message_appends_newline_only);
    RUN_TEST(write_rejects_embedded_newline);
    RUN_TEST(write_rejects_embedded_cr);

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
