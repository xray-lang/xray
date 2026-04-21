/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_parser.h - High-performance zero-copy HTTP parser
 *
 * KEY CONCEPT:
 *   Zero-copy design referencing original buffer directly. Optional SIMD
 *   acceleration with SSE4.2. Full chunked transfer encoding support.
 */

#ifndef XR_STDLIB_HTTP_PARSER_H
#define XR_STDLIB_HTTP_PARSER_H

#include "../../src/base/xdefs.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>

/* ========== Constants ========== */

#define XR_HTTP_MAX_HEADERS     64      // Max header count
#define XR_HTTP_MAX_METHOD_LEN  16      // Max method name length
#define XR_HTTP_MAX_URL_LEN     8192    // Max URL length

/* ========== HTTP Method Enum ========== */

typedef enum {
    XR_HTTP_METHOD_GET = 0,
    XR_HTTP_METHOD_POST,
    XR_HTTP_METHOD_PUT,
    XR_HTTP_METHOD_DELETE,
    XR_HTTP_METHOD_HEAD,
    XR_HTTP_METHOD_OPTIONS,
    XR_HTTP_METHOD_PATCH,
    XR_HTTP_METHOD_CONNECT,
    XR_HTTP_METHOD_TRACE,
    XR_HTTP_METHOD_UNKNOWN
} XrHttpMethod;

/* ========== Parse Result Enum ========== */

typedef enum {
    XR_HTTP_PARSE_OK = 0,           // Parse success, request complete
    XR_HTTP_PARSE_INCOMPLETE = -2,  // Data incomplete, need more data
    XR_HTTP_PARSE_ERROR = -1,       // Parse error
    XR_HTTP_PARSE_HEADER_TOO_LONG,  // Header too long
    XR_HTTP_PARSE_TOO_MANY_HEADERS  // Header count exceeded
} XrHttpParseResult;

/* ========== HTTP Header (Zero-Copy) ========== */

typedef struct {
    const char *name;       // Points to original buffer
    size_t name_len;
    const char *value;      // Points to original buffer
    size_t value_len;
} XrHttpHeader;

/* ========== HTTP Request (Zero-Copy) ========== */

typedef struct {
    // Request line
    XrHttpMethod method;
    const char *method_str;     // Original method string
    size_t method_len;
    const char *path;           // Path (without query string)
    size_t path_len;
    const char *query;          // Query string (without ?)
    size_t query_len;
    int version_major;          // HTTP major version
    int version_minor;          // HTTP minor version
    
    // Headers
    XrHttpHeader headers[XR_HTTP_MAX_HEADERS];
    size_t header_count;
    
    // Special headers (fast access)
    int64_t content_length;     // Content-Length, -1 = not set
    bool keep_alive;            // Connection: keep-alive
    bool chunked;               // Transfer-Encoding: chunked
    const char *content_type;   // Content-Type
    size_t content_type_len;
    const char *host;           // Host
    size_t host_len;
    
    // Body
    const char *body;           // Points to body start
    size_t body_len;            // Received body length
    
    // Parse state
    size_t header_bytes;        // Total header bytes (including \r\n\r\n)
} XrHttpRequest;

/* ========== HTTP Response (Zero-Copy) ========== */

typedef struct {
    // Status line
    int status_code;            // Status code
    const char *status_text;    // Status text
    size_t status_text_len;
    int version_major;
    int version_minor;
    
    // Headers
    XrHttpHeader headers[XR_HTTP_MAX_HEADERS];
    size_t header_count;
    
    // Special headers
    int64_t content_length;
    bool keep_alive;
    bool chunked;
    const char *content_type;
    size_t content_type_len;
    
    // Body
    const char *body;
    size_t body_len;
    
    // Parse state
    size_t header_bytes;
} XrHttpResponse;

/* ========== Chunked Decoder ========== */

/*
 * Chunked decoder state
 * Must memset to 0 before use
 */
typedef struct {
    size_t bytes_left_in_chunk; // Remaining bytes in current chunk
    char consume_trailer;       // Whether to consume trailer headers
    char _hex_count;            // Internal: parsed hex digit count
    char _state;                // Internal: state machine state
    uint64_t _total_read;       // Internal: total bytes read
    uint64_t _total_overhead;   // Internal: encoding overhead bytes
} XrChunkedDecoder;

/* ========== Parser State Machine ========== */

typedef enum {
    HTTP_STATE_REQUEST_LINE,
    HTTP_STATE_RESPONSE_LINE,
    HTTP_STATE_HEADER_NAME,
    HTTP_STATE_HEADER_VALUE,
    HTTP_STATE_BODY,
    HTTP_STATE_CHUNK_SIZE,
    HTTP_STATE_CHUNK_DATA,
    HTTP_STATE_DONE,
    HTTP_STATE_ERROR
} XrHttpParseState;

typedef struct {
    XrHttpParseState state;
    size_t consumed;            // Consumed byte count
    size_t line_start;          // Current line start position
} XrHttpParser;

/* ========== Core Parse API ========== */

/*
 * Parse HTTP request (one-shot header parsing)
 *
 * Returns:
 *   > 0  : Success, returns parsed byte count
 *   -1   : Parse error
 *   -2   : Data incomplete, need more data
 *
 * Parameters:
 *   buf        - Original data buffer
 *   len        - Data length
 *   method     - Output: method string pointer
 *   method_len - Output: method string length
 *   path       - Output: path string pointer
 *   path_len   - Output: path string length
 *   minor_ver  - Output: HTTP minor version
 *   headers    - Output: Header array
 *   num_headers- In/Out: Header array capacity / actual count
 *   last_len   - Last parse position (for incremental parsing), 0 for first call
 */
XR_FUNC int xr_http_parse_request_ex(const char *buf, size_t len,
                              const char **method, size_t *method_len,
                              const char **path, size_t *path_len,
                              int *minor_ver,
                              XrHttpHeader *headers, size_t *num_headers,
                              size_t last_len);

/*
 * Parse HTTP response (one-shot header parsing)
 *
 * Returns same as xr_http_parse_request_ex
 */
XR_FUNC int xr_http_parse_response_ex(const char *buf, size_t len,
                               int *minor_ver, int *status,
                               const char **msg, size_t *msg_len,
                               XrHttpHeader *headers, size_t *num_headers,
                               size_t last_len);

/*
 * Parse headers only (for trailer etc.)
 */
XR_FUNC int xr_http_parse_headers(const char *buf, size_t len,
                           XrHttpHeader *headers, size_t *num_headers,
                           size_t last_len);

/*
 * Decode chunked encoding
 *
 * This function rewrites buf in-place, removing chunked encoding overhead
 *
 * Returns:
 *   >= 0 : Complete, returns remaining undecoded bytes (from *bufsz offset)
 *   -1   : Parse error
 *   -2   : Data incomplete, need more data
 *
 * Parameters:
 *   decoder - Decoder state (must memset to 0 before use)
 *   buf     - Data buffer (will be modified in-place)
 *   bufsz   - In/Out: input is buffer size, output is decoded data size
 */
XR_FUNC ssize_t xr_http_decode_chunked(XrChunkedDecoder *decoder, 
                                char *buf, size_t *bufsz);

/*
 * Check if chunked decoder is in the middle of a data block
 */
XR_FUNC int xr_http_chunked_is_in_data(XrChunkedDecoder *decoder);

/* ========== Simplified API ========== */

/*
 * Initialize parser
 */
XR_FUNC void xr_http_parser_init(XrHttpParser *parser);

/*
 * Parse HTTP request (auto-fill XrHttpRequest struct)
 * Uses xr_http_parse_request_ex internally, and handles special headers
 */
XR_FUNC XrHttpParseResult xr_http_parse_request(XrHttpParser *parser, 
                                         XrHttpRequest *req,
                                         const char *data, 
                                         size_t len);

/*
 * Parse HTTP response (auto-fill XrHttpResponse struct)
 * Uses xr_http_parse_response_ex internally, and handles special headers
 */
XR_FUNC XrHttpParseResult xr_http_parse_response(XrHttpParser *parser,
                                          XrHttpResponse *resp,
                                          const char *data,
                                          size_t len);

/*
 * Reset parser state
 */
XR_FUNC void xr_http_parser_reset(XrHttpParser *parser);

/*
 * Initialize request struct
 */
XR_FUNC void xr_http_request_init(XrHttpRequest *req);

/*
 * Initialize response struct
 */
XR_FUNC void xr_http_response_init(XrHttpResponse *resp);

/*
 * Find header (case-insensitive)
 */
XR_FUNC const char* xr_http_get_header(XrHttpHeader *headers, size_t count,
                                const char *name, size_t *out_len);

/*
 * HTTP method string to enum
 */
XR_FUNC XrHttpMethod xr_http_method_from_string(const char *str, size_t len);

/*
 * HTTP method enum to string
 */
XR_FUNC const char* xr_http_method_to_string(XrHttpMethod method);

/* ========== HTTP Parse Helper Functions ========== */

/*
 * Parse Content-Length header
 * Returns: Content-Length value, -1 if not found
 */
static inline long long xr_http_parse_content_length(const char *headers, size_t len) {
    const char *p = headers;
    const char *end = headers + len;
    while (p < end) {
        if ((end - p >= 15) && 
            (p[0] == 'C' || p[0] == 'c') &&
            (p[7] == '-') &&
            (p[8] == 'L' || p[8] == 'l')) {
            const char *value = p + 16;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            return strtoll(value, NULL, 10);
        }
        while (p < end && *p != '\n') p++;
        p++;
    }
    return -1;
}

/*
 * Find HTTP header end position (\r\n\r\n)
 * Returns: position after header end (body start), NULL if not found
 */
static inline const char* xr_http_find_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            return buf + i + 4;
        }
    }
    return NULL;
}

/*
 * Parse HTTP status code
 * Returns: status code, -1 on parse failure
 */
static inline int xr_http_parse_status_code(const char *response) {
    if (!response) return -1;
    const char *p = response;
    while (*p && *p != ' ') p++;
    if (!*p) return -1;
    p++;
    return atoi(p);
}

#endif
