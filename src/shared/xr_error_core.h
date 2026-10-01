/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_error_core.h - Runtime-neutral user-visible error formatting helpers.
 */

#ifndef XR_ERROR_CORE_H
#define XR_ERROR_CORE_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "xr_error_messages.h"
#include "../base/xplatform.h"
#if defined(XR_OS_WINDOWS)
#include <fcntl.h>
#include <io.h>
#endif

#define XR_ERROR_CORE_INDEX_OOB_BUFSZ 96
#define XR_ERROR_CORE_TYPE_MISMATCH_BUFSZ 160

/* Exit status for spec 8.3.1 rule D3. Distinct from the uncaught-error status
 * (1) so tests and supervisors can tell "the program failed" from "the only
 * deterministic-cleanup mechanism failed". */
#define XR_ERROR_CORE_DEFER_THROW_EXIT 70

/* Spelled locally so this header stays dependency-free: it is included by both
 * runtime layers and by generated C, whose dialect may predate _Noreturn. */
#if defined(__GNUC__) || defined(__clang__)
#define XR_ERROR_CORE_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define XR_ERROR_CORE_NORETURN __declspec(noreturn)
#else
#define XR_ERROR_CORE_NORETURN
#endif

typedef struct XrErrorCoreMessageView {
    int code;
    const char *message;
    size_t message_len;
    bool has_code;
} XrErrorCoreMessageView;

static inline int xr_error_core_format_array_index_oob(char *buf, size_t cap, int64_t index,
                                                       int64_t length) {
    return snprintf(buf, cap, "array index out of range: %" PRId64 " (length %" PRId64 ")", index,
                    length);
}

static inline int xr_error_core_format_fixed_array_index_oob(char *buf, size_t cap, int64_t index,
                                                             int64_t length) {
    return snprintf(buf, cap, "fixed array index out of range: %" PRId64 " (length %" PRId64 ")",
                    index, length);
}

static inline int xr_error_core_format_type_mismatch(char *buf, size_t cap, const char *expected,
                                                     const char *actual) {
    return snprintf(buf, cap, "TypeError: expected '%s', got '%s'", expected ? expected : "unknown",
                    actual ? actual : "unknown");
}

static inline int xr_error_core_format_prefixed(char *buf, size_t cap, int code,
                                                const char *message) {
    return snprintf(buf, cap, "E%04d: %s", code, message ? message : "");
}

/* Spec 8.3.1 rule D3: an error escaped a `defer` body past the E0387 static
 * rule — only reachable across a boundary the analyzer cannot see through (an
 * `extern` call or a `CFn` callback). Cleanup failed, so the resource state is
 * unknown and there is no safe point to resume from: report both errors and
 * terminate. Uncatchable by construction, since this never returns.
 *
 * `code` is XR_ERR_DEFER_THROW; it is passed in rather than included so this
 * header stays dependency-free, matching xr_error_core_format_prefixed. The VM
 * and AOT both route here so their diagnostics and exit status are identical. */
static inline void xr_error_core_write_message(FILE *stream, XrErrorCoreMessageView view) {
    if (view.has_code) fprintf(stream, "E%04d: ", view.code);
    if (view.message) fwrite(view.message, 1, view.message_len, stream);
    else fputs(XR_ERROR_CORE_NO_MESSAGE_MSG, stream);
}

XR_ERROR_CORE_NORETURN static inline void
xr_error_core_defer_throw_abort(int code, XrErrorCoreMessageView escaped,
    const XrErrorCoreMessageView *in_flight) {
    fflush(stdout);
#if defined(XR_OS_WINDOWS)
    /* Fatal reporting never returns; binary mode preserves message LF bytes. */
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    fprintf(stderr, "E%04d: %s: ", code, XR_ERROR_CORE_DEFER_THROW_MSG);
    xr_error_core_write_message(stderr, escaped);
    fputc('\n', stderr);
    if (in_flight) {
        fprintf(stderr, "  %s: ", XR_ERROR_CORE_DEFER_THROW_IN_FLIGHT_MSG);
        xr_error_core_write_message(stderr, *in_flight);
        fputc('\n', stderr);
    }
    fprintf(stderr, "  %s\n", XR_ERROR_CORE_DEFER_THROW_HINT_MSG);
    fflush(stderr);
    exit(XR_ERROR_CORE_DEFER_THROW_EXIT);
}

static inline XrErrorCoreMessageView xr_error_core_parse_prefixed(const char *data, size_t len) {
    XrErrorCoreMessageView view = {0, data, len, false};
    if (!data || len < 7 || data[0] != 'E' || data[5] != ':' || data[6] != ' ')
        return view;

    int code = 0;
    for (size_t i = 1; i < 5; i++) {
        if (data[i] < '0' || data[i] > '9')
            return view;
        code = code * 10 + (data[i] - '0');
    }

    view.code = code;
    view.message = data + 7;
    view.message_len = len - 7;
    view.has_code = true;
    return view;
}

#endif  // XR_ERROR_CORE_H
