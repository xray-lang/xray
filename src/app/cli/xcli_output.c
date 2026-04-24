/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_output.c - Unified output strategy for CLI commands
 *
 * KEY CONCEPT:
 *   Centralizes color and timing logic previously duplicated across
 *   subcommands.  Color mode is auto-detected from isatty(STDOUT)
 *   and can be overridden by --color / --no-color.
 */

#include "xcli_output.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <unistd.h>
#include <time.h>

/* ========== Color State ========== */

static XrCliColorMode s_color_mode = XR_COLOR_AUTO;

void xr_cli_set_color(XrCliColorMode mode) {
    s_color_mode = mode;
}

bool xr_cli_color_enabled(void) {
    switch (s_color_mode) {
    case XR_COLOR_ON:  return true;
    case XR_COLOR_OFF: return false;
    case XR_COLOR_AUTO:
    default:
        return isatty(STDOUT_FILENO) != 0;
    }
}

const char *xr_cli_clr(const char *code) {
    XR_DCHECK(code != NULL, "code must not be NULL");
    return xr_cli_color_enabled() ? code : "";
}

/* ========== Timing ========== */

double xr_cli_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}
