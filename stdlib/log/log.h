/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * log.h - Structured logging module
 *
 * KEY CONCEPT:
 *   Multi-level structured logging with JSON/Text output formats.
 *   Supports child loggers that inherit context attributes.
 *
 * EXPORTED SURFACE:
 *   Everything here except the module loader is an implementation detail
 *   living in log.c. The VM reaches the bindings through the module export
 *   table; the GC destroy callback is declared alongside the other per-type
 *   hooks in src/runtime/gc/xgc_internal.h.
 */

#ifndef XR_STDLIB_LOG_H
#define XR_STDLIB_LOG_H

#include "../../src/base/xdefs.h"
#include "../../src/runtime/value/xvalue.h"
#include <stdio.h>
#include <stdbool.h>

struct XrModule;

/* ========== Log Levels ========== */

typedef enum XrLogLevel {
    XR_LOG_DEBUG = 10,
    XR_LOG_INFO = 20,
    XR_LOG_WARN = 30,
    XR_LOG_ERROR = 40,
    XR_LOG_FATAL = 50
} XrLogLevel;

/* ========== Output Formats ========== */

typedef enum XrLogFormat {
    XR_LOG_FORMAT_TEXT,
    XR_LOG_FORMAT_JSON
} XrLogFormat;

/* ========== Logger Structure ========== */

typedef struct XrLogger {
    XrLogLevel level;
    XrLogFormat format;
    FILE *output;  // Default: stderr
    bool add_source;
    bool async_mode;

    // Inherited context (pre-formatted for both output formats)
    char *json_ctx;  // JSON fragment: "key":"val","k2":123
    size_t json_ctx_len;
    char *text_ctx;  // Text fragment: key=val k2=123
    size_t text_ctx_len;

    struct XrLogger *parent;
} XrLogger;

/* ========== Logger Reference (GC-managed wrapper) ========== */

typedef struct XrLoggerRef {
    XrGCHeader gc;     // type = XR_TLOGGER
    XrLogger *logger;  // Points to actual XrLogger (heap-allocated, shared)
} XrLoggerRef;

/* ========== Module Loader ========== */

XR_FUNC struct XrModule *xr_load_module_log(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_LOG_H
