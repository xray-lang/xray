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
#include "../../base/xtime.h"
#include <stdio.h>
#include <unistd.h>

/* ========== Color State ========== */

static XrCliColorMode s_color_mode = XR_COLOR_AUTO;

void xr_cli_set_color(XrCliColorMode mode) {
    s_color_mode = mode;
}

bool xr_cli_color_enabled(void) {
    switch (s_color_mode) {
        case XR_COLOR_ON:
            return true;
        case XR_COLOR_OFF:
            return false;
        case XR_COLOR_AUTO:
        default:
            return isatty(STDOUT_FILENO) != 0;
    }
}

const char *xr_cli_clr(const char *code) {
    XR_DCHECK(code != NULL, "code must not be NULL");
    return xr_cli_color_enabled() ? code : "";
}

/* ========== Output Verbosity ========== */

static XrCliOutputLevel s_output_level = XR_OUTPUT_NORMAL;
static bool s_json_output = false;

void xr_cli_set_output_level(XrCliOutputLevel level) {
    s_output_level = level;
}

XrCliOutputLevel xr_cli_output_level(void) {
    return s_output_level;
}

void xr_cli_set_json(bool on) {
    s_json_output = on;
    if (on)
        xr_cli_set_color(XR_COLOR_OFF);
}

bool xr_cli_json_output(void) {
    return s_json_output;
}

/* ========== Timing ========== */

double xr_cli_get_time_ms(void) {
    return (double) xr_time_monotonic_ns() / 1000000.0;
}
