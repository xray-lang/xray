/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_driver.h - AOT native compilation driver
 *
 * KEY CONCEPT:
 *   Encapsulates the full AOT pipeline: bundle discovery, per-module
 *   compilation to XrProto, XIR lowering, C code emission, and main()
 *   generation.  The CLI layer calls xaot_build() and receives
 *   a generated C source string ready for the system C compiler.
 *
 *   All bytecode scanning (shared_proto_map, class pre-registration,
 *   export collection) lives here rather than in the CLI.
 *
 * RELATED MODULES:
 *   - xcgen.h: per-function XIR → C lowering
 *   - xcmd_build.c: CLI entry that invokes xaot_build + CC
 */

#ifndef XAOT_DRIVER_H
#define XAOT_DRIVER_H

#include "../base/xchecks.h"

/* Result of xaot_build().  Caller must free c_source via xr_free()
 * and call xaot_result_cleanup() for the rest. */
typedef struct {
    char *c_source;      /* generated C program (malloc'd, caller frees) */
    int total_compiled;  /* number of functions successfully transpiled */
    int total_aot;       /* total AOT-eligible functions found */
    int nmodules;        /* number of modules in the bundle */
} XaotBuildResult;

/* Run the full AOT pipeline for a given source file.
 * Returns 0 on success, non-zero on failure.
 * On success, result->c_source is a complete C program (includes +
 * transpiled functions + main()).  Caller frees result->c_source. */
XR_FUNC int xaot_build(const char *input_path, XaotBuildResult *result);

#endif  // XAOT_DRIVER_H
