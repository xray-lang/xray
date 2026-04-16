/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_http_parser.c - Unit tests for HTTP parser
 *
 * KEY CONCEPT:
 *   Tests HTTP request/response parsing, method conversion,
 *   header lookup, chunked decoding, and helper functions.
 */

#include "../test_framework.h"
#include <string.h>
#include <stdlib.h>

#include "../../../stdlib/http/http_parser.h"

/* ========== Method Conversion ========== */

TEST(http_method_from_string) {
    ASSERT_EQ_INT(xr_http_method_from_string("GET", 3), XR_HTTP_METHOD_GET);
    ASSERT_EQ_INT(xr_http_method_from_string("POST", 4), XR_HTTP_METHOD_POST);
    ASSERT_EQ_INT(xr_http_method_from_string("PUT", 3), XR_HTTP_METHOD_PUT);
    ASSERT_EQ_INT(xr_http_method_from_string("DELETE", 6), XR_HTTP_METHOD_DELETE);
    ASSERT_EQ_INT(xr_http_method_from_string("HEAD", 4), XR_HTTP_METHOD_HEAD);
    ASSERT_EQ_INT(xr_http_method_from_string("OPTIONS", 7), XR_HTTP_METHOD_OPTIONS);
    ASSERT_EQ_INT(xr_http_method_from_string("PATCH", 5), XR_HTTP_METHOD_PATCH);
    ASSERT_EQ_INT(xr_http_method_from_string("CONNECT", 7), XR_HTTP_METHOD_CONNECT);
    ASSERT_EQ_INT(xr_http_method_from_string("TRACE", 5), XR_HTTP_METHOD_TRACE);
    ASSERT_EQ_INT(xr_http_method_from_string("INVALID", 7), XR_HTTP_METHOD_UNKNOWN);
}

TEST(http_method_to_string) {
    ASSERT_STR_EQ(xr_http_method_to_string(XR_HTTP_METHOD_GET), "GET");
    ASSERT_STR_EQ(xr_http_method_to_string(XR_HTTP_METHOD_POST), "POST");
    ASSERT_STR_EQ(xr_http_method_to_string(XR_HTTP_METHOD_PUT), "PUT");
    ASSERT_STR_EQ(xr_http_method_to_string(XR_HTTP_METHOD_DELETE), "DELETE");
    ASSERT_NOT_NULL(xr_http_method_to_string(XR_HTTP_METHOD_UNKNOWN));
}

/* ========== Request Parsing ========== */

TEST(http_parse_request_get) {
    const char *data = "GET /index.html HTTP/1.1\r\n"
                       "Host: example.com\r\n"
                       "Accept: text/html\r\n"
                       "\r\n";
    size_t len = strlen(data);

    const char *method = NULL, *path = NULL;
    size_t method_len = 0, path_len = 0;
    int minor_ver = 0;
    XrHttpHeader headers[16];
    size_t num_headers = 16;

    int ret = xr_http_parse_request_ex(data, len,
                                        &method, &method_len,
                                        &path, &path_len,
                                        &minor_ver,
                                        headers, &num_headers, 0);
    ASSERT_GT(ret, 0);
    ASSERT_EQ_INT((int)method_len, 3);
    ASSERT_EQ_INT(memcmp(method, "GET", 3), 0);
    ASSERT_EQ_INT(minor_ver, 1);
    ASSERT_EQ_INT((int)num_headers, 2);
}

TEST(http_parse_request_post) {
    const char *data = "POST /api/data HTTP/1.1\r\n"
                       "Host: api.example.com\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: 13\r\n"
                       "\r\n"
                       "{\"key\":\"val\"}";
    size_t len = strlen(data);

    const char *method = NULL, *path = NULL;
    size_t method_len = 0, path_len = 0;
    int minor_ver = 0;
    XrHttpHeader headers[16];
    size_t num_headers = 16;

    int ret = xr_http_parse_request_ex(data, len,
                                        &method, &method_len,
                                        &path, &path_len,
                                        &minor_ver,
                                        headers, &num_headers, 0);
    ASSERT_GT(ret, 0);
    ASSERT_EQ_INT((int)method_len, 4);
    ASSERT_EQ_INT(memcmp(method, "POST", 4), 0);
    ASSERT_EQ_INT((int)num_headers, 3);
}

/* ========== Response Parsing ========== */

TEST(http_parse_response_200) {
    const char *data = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-Length: 5\r\n"
                       "\r\n"
                       "Hello";
    size_t len = strlen(data);

    int minor_ver = 0, status = 0;
    const char *msg = NULL;
    size_t msg_len = 0;
    XrHttpHeader headers[16];
    size_t num_headers = 16;

    int ret = xr_http_parse_response_ex(data, len,
                                         &minor_ver, &status,
                                         &msg, &msg_len,
                                         headers, &num_headers, 0);
    ASSERT_GT(ret, 0);
    ASSERT_EQ_INT(status, 200);
    ASSERT_EQ_INT(minor_ver, 1);
    ASSERT_EQ_INT((int)num_headers, 2);
}

TEST(http_parse_response_404) {
    const char *data = "HTTP/1.1 404 Not Found\r\n"
                       "Content-Length: 0\r\n"
                       "\r\n";
    size_t len = strlen(data);

    int minor_ver = 0, status = 0;
    const char *msg = NULL;
    size_t msg_len = 0;
    XrHttpHeader headers[16];
    size_t num_headers = 16;

    int ret = xr_http_parse_response_ex(data, len,
                                         &minor_ver, &status,
                                         &msg, &msg_len,
                                         headers, &num_headers, 0);
    ASSERT_GT(ret, 0);
    ASSERT_EQ_INT(status, 404);
}

/* ========== Header Lookup ========== */

TEST(http_get_header) {
    XrHttpHeader headers[3];
    headers[0] = (XrHttpHeader){"Host", 4, "example.com", 11};
    headers[1] = (XrHttpHeader){"Content-Type", 12, "text/html", 9};
    headers[2] = (XrHttpHeader){"Accept", 6, "*/*", 3};

    size_t val_len = 0;
    const char *val = xr_http_get_header(headers, 3, "Content-Type", &val_len);
    ASSERT_NOT_NULL(val);
    ASSERT_EQ_INT((int)val_len, 9);
    ASSERT_EQ_INT(memcmp(val, "text/html", 9), 0);
}

TEST(http_get_header_not_found) {
    XrHttpHeader headers[1];
    headers[0] = (XrHttpHeader){"Host", 4, "example.com", 11};

    size_t val_len = 0;
    const char *val = xr_http_get_header(headers, 1, "X-Missing", &val_len);
    ASSERT_NULL(val);
}

/* ========== Helper Functions ========== */

TEST(http_find_header_end) {
    const char *data = "GET / HTTP/1.1\r\nHost: x\r\n\r\nBody";
    const char *end = xr_http_find_header_end(data, strlen(data));
    ASSERT_NOT_NULL(end);
    ASSERT_STR_EQ(end, "Body");
}

TEST(http_find_header_end_not_found) {
    const char *data = "GET / HTTP/1.1\r\nHost: x\r\n";
    const char *end = xr_http_find_header_end(data, strlen(data));
    ASSERT_NULL(end);
}

TEST(http_parse_status_code_helper) {
    ASSERT_EQ_INT(xr_http_parse_status_code("HTTP/1.1 200 OK"), 200);
    ASSERT_EQ_INT(xr_http_parse_status_code("HTTP/1.1 404 Not Found"), 404);
    ASSERT_EQ_INT(xr_http_parse_status_code(NULL), -1);
}

/* ========== Simplified API ========== */

TEST(http_parser_init_reset) {
    XrHttpParser parser;
    xr_http_parser_init(&parser);
    ASSERT_EQ_INT(parser.state, HTTP_STATE_REQUEST_LINE);

    xr_http_parser_reset(&parser);
    ASSERT_EQ_INT(parser.state, HTTP_STATE_REQUEST_LINE);
}

TEST(http_request_init) {
    XrHttpRequest req;
    xr_http_request_init(&req);
    ASSERT_EQ_INT(req.method, XR_HTTP_METHOD_GET);
    ASSERT_EQ_INT((int)req.content_length, -1);
    ASSERT_EQ_INT((int)req.header_count, 0);
}

TEST(http_response_init) {
    XrHttpResponse resp;
    xr_http_response_init(&resp);
    ASSERT_EQ_INT(resp.status_code, 0);
    ASSERT_EQ_INT((int)resp.content_length, -1);
}

/* ========== Incomplete Data ========== */

TEST(http_parse_request_incomplete) {
    const char *data = "GET /index.html HTTP/1.1\r\n"
                       "Host: example.com\r\n";
    size_t len = strlen(data);

    const char *method = NULL, *path = NULL;
    size_t method_len = 0, path_len = 0;
    int minor_ver = 0;
    XrHttpHeader headers[16];
    size_t num_headers = 16;

    int ret = xr_http_parse_request_ex(data, len,
                                        &method, &method_len,
                                        &path, &path_len,
                                        &minor_ver,
                                        headers, &num_headers, 0);
    ASSERT_EQ_INT(ret, -2);  // Incomplete
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("HTTP - Method Conversion");
    RUN_TEST(http_method_from_string);
    RUN_TEST(http_method_to_string);

    RUN_TEST_SUITE("HTTP - Request Parsing");
    RUN_TEST(http_parse_request_get);
    RUN_TEST(http_parse_request_post);

    RUN_TEST_SUITE("HTTP - Response Parsing");
    RUN_TEST(http_parse_response_200);
    RUN_TEST(http_parse_response_404);

    RUN_TEST_SUITE("HTTP - Header Lookup");
    RUN_TEST(http_get_header);
    RUN_TEST(http_get_header_not_found);

    RUN_TEST_SUITE("HTTP - Helpers");
    RUN_TEST(http_find_header_end);
    RUN_TEST(http_find_header_end_not_found);
    RUN_TEST(http_parse_status_code_helper);

    RUN_TEST_SUITE("HTTP - Init/Reset");
    RUN_TEST(http_parser_init_reset);
    RUN_TEST(http_request_init);
    RUN_TEST(http_response_init);

    RUN_TEST_SUITE("HTTP - Edge Cases");
    RUN_TEST(http_parse_request_incomplete);

TEST_MAIN_END()
