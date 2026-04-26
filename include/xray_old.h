/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_old.h - Legacy public API (deprecated)
 */

#ifndef XRAY_OLD_H
#define XRAY_OLD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "xray_version.h"

/* ========== Opaque Types ========== */

/*
** XrayIsolate - Xray isolated execution environment (v2.0.0)
** Fully opaque type; internal structure not exposed to users
*/
typedef struct XrayIsolate XrayIsolate;

/* ========== Basic Type Definitions ========== */
typedef double xr_Number;
typedef int64_t xr_Integer;

/* ========== Core API ========== */

/*
** Create a new Xray isolate
** @return New isolate instance, or NULL on failure
*/
XrayIsolate *xray_new(void);

/*
** Execute a Xray script file
** @param iso Isolate instance
** @param filename Script file path
** @return 0 on success, non-zero on failure
*/
int xray_dofile(XrayIsolate *iso, const char *filename);

/*
** Execute a Xray code string
** @param iso Isolate instance
** @param source Source code string
** @return 0 on success, non-zero on failure
*/
int xray_dostring(XrayIsolate *iso, const char *source);

/* ========== Error Handling ========== */

/*
** Get the last error message
** @param iso Isolate instance
** @return Error message string, or NULL if no error
*/
const char *xray_get_error(XrayIsolate *iso);

/*
** Check if an error occurred
** @param iso Isolate instance
** @return true if error, false otherwise
*/
bool xray_has_error(XrayIsolate *iso);

/*
** Clear error state
** @param iso Isolate instance
*/
void xray_clear_error(XrayIsolate *iso);

/* ========== Configuration ========== */

/*
** Set VM option
** @param iso Isolate instance
** @param name Option name
** @param value Option value
*/
void xray_set_option(XrayIsolate *iso, const char *name, int value);

/*
** Get VM option
** @param iso Isolate instance
** @param name Option name
** @return Option value
*/
int xray_get_option(XrayIsolate *iso, const char *name);

/* ========== Debug Support ========== */

/*
** Enable/disable execution tracing
** @param iso Isolate instance
** @param enable true to enable, false to disable
*/
void xray_set_trace(XrayIsolate *iso, bool enable);

/*
** Print VM statistics
** @param iso Isolate instance
*/
void xray_print_stats(XrayIsolate *iso);

#endif  // XRAY_OLD_H
