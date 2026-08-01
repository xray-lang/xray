/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_cycle_report.h - runtime cycle findings, surfaced in the editor
 *
 * The development-build cycle detector (task 247 phase D) runs inside the
 * process that leaked and writes what it found to a sidecar file. The editor
 * runs somewhere else entirely, so that file is the handoff: it names the
 * class and field on each candidate edge, which is exactly what a code action
 * needs to put `weak` in front of the right declaration.
 *
 * Reading it is best-effort by design. A stale or absent report degrades to no
 * diagnostics, never to a wrong edit.
 */

#ifndef XLSP_CYCLE_REPORT_H
#define XLSP_CYCLE_REPORT_H

#include "xlsp_server.h"

/* Load (or reload) the sidecar report for `server`'s workspace. Cheap enough
 * to call per diagnostic pass: it stats the file and re-parses only when the
 * mtime moved. */
XR_FUNC void xlsp_cycle_report_refresh(XrLspServer *server);

/* Append one diagnostic per field declaration in `doc` that a reported cycle
 * runs through. No-op when no report has been loaded. */
XR_FUNC void xlsp_cycle_report_diagnostics(XrLspDocument *doc, XrJsonValue *diagnostics);

/* Free the loaded report. Called at server shutdown. */
XR_FUNC void xlsp_cycle_report_clear(XrLspServer *server);

/* Marker the diagnostic message opens with, so the code-action handler can
 * recognise its own diagnostics without re-reading the report. */
#define XLSP_CYCLE_DIAG_PREFIX "reference cycle observed at runtime: "

#endif  // XLSP_CYCLE_REPORT_H
