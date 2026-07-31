/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_panic_report.h - Runtime-neutral uncaught-panic report shared by the VM
 * and AOT backends.
 *
 * An uncaught panic must read the same on both backends. The VM interprets on a
 * native stack it fully controls; the AOT native path does not carry unwind
 * state. The only report both can always produce is a single line built from
 * the fault's error code and message, and both source that message from the
 * same xr_error_core formatters, so the rendered body matches byte for byte.
 *
 * Stack traces are deliberately NOT part of this report. They are the VM's free
 * by-product and the AOT path's structural gap; folding them in here would make
 * the two backends disagree by construction. The VM prints its own trace
 * separately, gated behind XRAY_BACKTRACE, as an opt-in hosted diagnostic
 * outside the cross-backend contract.
 */

#ifndef XR_PANIC_REPORT_H
#define XR_PANIC_REPORT_H

#include <stdbool.h>
#include <stdio.h>

#include "xr_error_core.h"

/* "E%04d: <message>" plus the fixed "[Uncaught Panic] " tag and framing; the
 * widest message is the type-mismatch text, so this comfortably bounds it. */
#define XR_PANIC_REPORT_BUFSZ (XR_ERROR_CORE_TYPE_MISMATCH_BUFSZ + 16)

/* Canonical uncaught-panic report, emitted identically by both backends:
 *
 *     \n[Uncaught Panic] E<code>: <message>\n
 *
 * `message` is the bare fault text with no code prefix. `use_color` wraps the
 * "[Uncaught Panic]" tag in bold red; callers decide it from the stream's TTY
 * state so piped output (the differential harness) stays plain on both sides. */
static inline void xr_panic_report_emit(FILE *stream, int code, const char *message,
                                        bool use_color) {
    char body[XR_PANIC_REPORT_BUFSZ];
    xr_error_core_format_prefixed(body, sizeof(body), code, message ? message : "");
    if (use_color)
        fprintf(stream, "\n\033[1;31m[Uncaught Panic]\033[0m %s\n", body);
    else
        fprintf(stream, "\n[Uncaught Panic] %s\n", body);
}

/* XRAY_BACKTRACE gate for the opt-in stack trace on the uncaught-panic path.
 * Both backends read the same switch. Off unless the value is present and is
 * neither empty nor the single character "0". */
static inline bool xr_panic_backtrace_enabled(const char *env_value) {
    return env_value != NULL && env_value[0] != '\0' &&
           !(env_value[0] == '0' && env_value[1] == '\0');
}

#endif  // XR_PANIC_REPORT_H
