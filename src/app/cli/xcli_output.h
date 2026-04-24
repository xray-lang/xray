/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_output.h - Unified output strategy for CLI commands
 *
 * KEY CONCEPT:
 *   Single source of truth for color codes, terminal detection, and
 *   timing.  Commands query xr_cli_color_enabled() instead of each
 *   maintaining their own flag.
 */

#ifndef XCLI_OUTPUT_H
#define XCLI_OUTPUT_H

#include <stdbool.h>
#include "../../base/xdefs.h"

/* ========== ANSI Color Codes ========== */

#define XR_CLR_RESET   "\033[0m"
#define XR_CLR_BOLD    "\033[1m"
#define XR_CLR_DIM     "\033[2m"
#define XR_CLR_RED     "\033[1;31m"
#define XR_CLR_GREEN   "\033[32m"
#define XR_CLR_YELLOW  "\033[33m"
#define XR_CLR_BLUE    "\033[34m"
#define XR_CLR_MAGENTA "\033[35m"
#define XR_CLR_CYAN    "\033[36m"
#define XR_CLR_GRAY    "\033[90m"

/* ========== Color Control ========== */

typedef enum {
    XR_COLOR_AUTO,   /* Detect from terminal (default) */
    XR_COLOR_ON,     /* Force color on */
    XR_COLOR_OFF,    /* Force color off */
} XrCliColorMode;

/* Set global color mode. Called by dispatch after parsing --color/--no-color. */
XR_FUNC void xr_cli_set_color(XrCliColorMode mode);

/* Query whether color is currently enabled. */
XR_FUNC bool xr_cli_color_enabled(void);

/* Return the code if color is enabled, empty string otherwise.
 * Convenience for:  printf("%sPASS%s\n", CLR(XR_CLR_GREEN), CLR(XR_CLR_RESET)); */
XR_FUNC const char *xr_cli_clr(const char *code);

/* ========== Timing ========== */

/* Monotonic time in milliseconds. */
XR_FUNC double xr_cli_get_time_ms(void);

#endif // XCLI_OUTPUT_H
