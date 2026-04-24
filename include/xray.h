/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray.h - Xray public API
 *
 * KEY CONCEPT:
 *   This is the only header file that external users need to include.
 *   All types are opaque - users don't need to know internal structures.
 */

#ifndef XRAY_H
#define XRAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "xray_version.h"
#include "xray_export.h"

/* ========== Opaque Types ========== */

// XrayIsolate - Isolated execution environment
// Completely opaque type, external users don't need to know internals
typedef struct XrayIsolate XrayIsolate;

/* ========== Basic Type Definitions ========== */
typedef double xr_Number;
typedef int64_t xr_Integer;

/* ========== Core API ========== */

// Create a new Xray isolate
// Returns new isolate instance, or NULL on failure
XRAY_API XrayIsolate* xray_new(void);

// Execute an Xray script file
// @param iso       Isolate instance
// @param filename  Path to script file
// @return 0 on success, non-zero on failure
XRAY_API int xray_dofile(XrayIsolate *iso, const char *filename);

// Execute an Xray code string
// @param iso     Isolate instance
// @param source  Source code string
// @return 0 on success, non-zero on failure
XRAY_API int xray_dostring(XrayIsolate *iso, const char *source);

/* ========== Error Handling ========== */

// Get the last error message
// @param iso  Isolate instance
// @return Error message string, or NULL if no error
XRAY_API const char* xray_get_error(XrayIsolate *iso);

// Check if there is an error
// @param iso  Isolate instance
// @return true if error exists, false otherwise
XRAY_API bool xray_has_error(XrayIsolate *iso);

// Clear error state
// @param iso  Isolate instance
XRAY_API void xray_clear_error(XrayIsolate *iso);

/* ========== Configuration ========== */

// Set VM option
// @param iso    Isolate instance
// @param name   Option name
// @param value  Option value
XRAY_API void xray_set_option(XrayIsolate *iso, const char *name, int value);

// Get VM option
// @param iso   Isolate instance
// @param name  Option name
// @return Option value
XRAY_API int xray_get_option(XrayIsolate *iso, const char *name);

/* ========== Debug Support ========== */

// Enable/disable execution tracing
// @param iso     Isolate instance
// @param enable  true to enable, false to disable
XRAY_API void xray_set_trace(XrayIsolate *iso, bool enable);

// Print VM statistics
// @param iso  Isolate instance
XRAY_API void xray_print_stats(XrayIsolate *iso);

/* ========== Runtime Support ========== */

// Initialize multicore runtime
// @param iso          Isolate instance
// @param num_workers  Number of worker threads, 0 for auto-detect CPU cores
XRAY_API void xr_multicore_init(XrayIsolate *iso, int num_workers);

// Destroy multicore runtime
// @param iso  Isolate instance
XRAY_API void xr_multicore_destroy(XrayIsolate *iso);

// Start coroutine monitor
// @param iso                Isolate instance
// @param watch_interval_ms  Terminal refresh interval in ms, 0 to disable
// @param http_port          HTTP monitor port, 0 to disable
XRAY_API void xr_coro_monitor_start(XrayIsolate *iso, int watch_interval_ms, int http_port);

#endif // XRAY_H

