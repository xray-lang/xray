/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_host_cli.h - Hosted module entry and borrowed terminal diagnostics
 *
 * KEY CONCEPT:
 *   The host owner executes; this adapter renders its owned outcome and applies
 *   the ordinary command exit policy without retaining a second execution.
 */
#ifndef XR_XIR_HOST_CLI_H
#define XR_XIR_HOST_CLI_H
#include "xr_xir_host_execution.h"
#include "../shared/xr_value_format_core.h"

typedef enum XrXirHostReportStatus {
    XR_XIR_HOST_REPORT_OK, XR_XIR_HOST_REPORT_BAD_ARGUMENT, XR_XIR_HOST_REPORT_IO
} XrXirHostReportStatus;
/* Borrows a terminal result; does not allocate, retain or drop. Unsupported
 * Error fields return BAD_ARGUMENT. A sink failure returns IO and can leave
 * already-written bytes; callers must not mistake a partial line for success. */
XR_FUNC XrXirHostReportStatus xr_xir_host_report_result(const XrXirCallResult *result,
    XrValueFormatSink sink, bool color);
/* Consumes the supplied Program reference on every return path, including
 * stream/configuration failure. NULL returns internal failure without running.
 * The entry is the admitted module initializer; no new admission ledger is made.
 * Program destruction precedes terminal rendering; the owned result dies last. */
XR_FUNC int xr_xir_host_program_main(XrXirProgram *owned_program, uint32_t entry);
/* A module initializer must return the admitted I64 zero control value.
 * The generated executable supplies its actual immutable ProgramSpec. */
XR_FUNC int xr_xir_host_main(const XrXirProgramSpec *spec);
#endif // XR_XIR_HOST_CLI_H
