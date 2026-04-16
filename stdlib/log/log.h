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
 */

#ifndef XR_STDLIB_LOG_H
#define XR_STDLIB_LOG_H

#include "../../src/runtime/value/xvalue.h"
#include <stdio.h>
#include <stdbool.h>

// Forward declaration
struct XrModule;

/* ========== Log Levels ========== */

typedef enum XrLogLevel {
    XR_LOG_DEBUG = 10,
    XR_LOG_INFO  = 20,
    XR_LOG_WARN  = 30,
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
    FILE *output;            // Default: stderr
    bool add_source;
    bool async_mode;
    
    // Inherited context (pre-formatted for both output formats)
    char *json_ctx;         // JSON fragment: "key":"val","k2":123
    int json_ctx_len;
    char *text_ctx;         // Text fragment: key=val k2=123
    int text_ctx_len;
    
    struct XrLogger *parent;
} XrLogger;

/* ========== Logger Reference (GC-managed wrapper) ========== */

typedef struct XrLoggerRef {
    XrGCHeader gc;       // type = XR_TLOGGER
    XrLogger *logger;    // Points to actual XrLogger (malloc-allocated, shared across coroutines)
} XrLoggerRef;

/* ========== Global Logger ========== */

// Get global default logger
XrLogger* xr_log_default(void);

// Reset global logger to default settings
void xr_log_reset(void);

/* ========== Log Output Functions ========== */

// Core log function with source location
// attrs: key-value pairs [key1, val1, key2, val2, ...], nattrs = number of pairs
void xr_log_write_ex(XrLogger *logger, XrLogLevel level, 
                     const char *msg, XrValue *attrs, int nattrs,
                     const char *source_file, int source_line);

// Simplified version without source location
void xr_log_write(XrLogger *logger, XrLogLevel level, 
                  const char *msg, XrValue *attrs, int nattrs);

// log.enableSource(enabled: bool) -> null
XrValue xr_log_enable_source(XrayIsolate *isolate, XrValue *args, int nargs);

// log.enableAsync(enabled: bool) -> null
// Async mode: logs are buffered and flushed by background thread
XrValue xr_log_enable_async(XrayIsolate *isolate, XrValue *args, int nargs);

// log.flush() -> null (blocks until async buffer is flushed)
XrValue xr_log_flush(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== VM Binding Functions ========== */

// log.debug(msg, ...attrs) -> null
XrValue xr_log_debug(XrayIsolate *isolate, XrValue *args, int nargs);

// log.info(msg, ...attrs) -> null
XrValue xr_log_info(XrayIsolate *isolate, XrValue *args, int nargs);

// log.warn(msg, ...attrs) -> null
XrValue xr_log_warn(XrayIsolate *isolate, XrValue *args, int nargs);

// log.error(msg, ...attrs) -> null
XrValue xr_log_error(XrayIsolate *isolate, XrValue *args, int nargs);

// log.fatal(msg, ...attrs) -> null (calls exit(1) after logging)
XrValue xr_log_fatal(XrayIsolate *isolate, XrValue *args, int nargs);

// log.setLevel(level: int) -> null
XrValue xr_log_set_level(XrayIsolate *isolate, XrValue *args, int nargs);

// log.setFormat(format: string) -> null ("json" or "text")
XrValue xr_log_set_format(XrayIsolate *isolate, XrValue *args, int nargs);

// log.setOutput(path: string) -> null (file path, "stdout", or "stderr")
XrValue xr_log_set_output(XrayIsolate *isolate, XrValue *args, int nargs);

// log.child(...attrs) -> Logger (creates child logger with inherited context)
XrValue xr_log_child(XrayIsolate *isolate, XrValue *args, int nargs);

// log.isEnabled(level: int) -> bool
XrValue xr_log_is_enabled(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Child Logger Methods ========== */
XrValue xr_logger_debug(XrayIsolate *isolate, XrValue *args, int nargs);
XrValue xr_logger_info(XrayIsolate *isolate, XrValue *args, int nargs);
XrValue xr_logger_warn(XrayIsolate *isolate, XrValue *args, int nargs);
XrValue xr_logger_error(XrayIsolate *isolate, XrValue *args, int nargs);
XrValue xr_logger_fatal(XrayIsolate *isolate, XrValue *args, int nargs);
XrValue xr_logger_child(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Helper Functions ========== */

// Get log level name string
const char* xr_log_level_name(XrLogLevel level);

// Parse log level from string
XrLogLevel xr_log_level_parse(const char *name);

/* ========== Module Loader ========== */
struct XrModule* xr_load_module_log(XrayIsolate *isolate);

#endif
