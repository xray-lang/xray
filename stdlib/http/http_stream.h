/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_stream.h - HTTP streaming transfer support
 *
 * KEY CONCEPT:
 *   Large file download with chunked reading, progress callbacks,
 *   and resume download support.
 */

#ifndef XR_STDLIB_HTTP_STREAM_H
#define XR_STDLIB_HTTP_STREAM_H

#include "../../src/base/xdefs.h"
#include "http_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ========== Progress Callback ========== */

typedef void (*XrHttpProgressCallback)(size_t downloaded,  // Downloaded bytes
                                       size_t total,       // Total bytes (-1 = unknown)
                                       void *user_data     // User data
);

/* ========== Data Callback ========== */

typedef int (*XrHttpDataCallback)(const char *data,  // Data chunk
                                  size_t len,        // Data length
                                  void *user_data    // User data
);

/* ========== Stream Download Config ========== */

typedef struct XrStreamConfig {
    const char *url;                     // Download URL
    const char *output_path;             // Output file path (optional)
    XrHttpProgressCallback on_progress;  // Progress callback (optional)
    XrHttpDataCallback on_data;          // Data callback (optional)
    void *user_data;                     // Callback user data
    size_t buffer_size;                  // Buffer size (default 64KB)
    int timeout_ms;                      // Timeout (milliseconds)
    size_t range_start;                  // Range start (for resume download)
    size_t range_end;                    // Range end (0 = to end)
    bool follow_redirects;               // Follow redirects
} XrStreamConfig;

/* ========== Stream Download Result ========== */

typedef struct XrStreamResult {
    int status_code;    // HTTP status code
    size_t total_size;  // Total size
    size_t downloaded;  // Downloaded bytes
    XrHttpError error;  // Error code
    char *error_msg;    // Error message
    bool completed;     // Is completed
} XrStreamResult;

/* ========== API ========== */

/*
 * Initialize stream config
 */
XR_FUNC void xr_stream_config_init(XrStreamConfig *config);

/*
 * Stream download to file
 *
 * url: Download URL
 * output_path: Output file path
 * on_progress: Progress callback (optional)
 * user_data: Callback user data
 *
 * Returns: download result
 */
XR_FUNC XrStreamResult xr_http_download(struct XrayIsolate *X, const char *url,
                                        const char *output_path,
                                        XrHttpProgressCallback on_progress, void *user_data);

/*
 * Stream download (using config)
 */
XR_FUNC XrStreamResult xr_http_stream(struct XrayIsolate *X, const XrStreamConfig *config);

/*
 * Resume download
 *
 * Checks local file size and sends Range header to continue download
 */
XR_FUNC XrStreamResult xr_http_resume_download(struct XrayIsolate *X, const char *url,
                                               const char *output_path,
                                               XrHttpProgressCallback on_progress,
                                               void *user_data);

/*
 * Get remote file size (HEAD request)
 * Returns: file size, -1 on failure or unknown
 */
XR_FUNC long long xr_http_get_content_length(XrayIsolate *X, const char *url);

/*
 * Free stream result
 */
XR_FUNC void xr_stream_result_free(XrStreamResult *result);

#endif
