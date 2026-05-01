/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_driver.h - AOT native compilation driver (Xi IR pipeline)
 *
 * KEY CONCEPT:
 *   Full pipeline from source to generated C:
 *   1. Bundle discovery (topo-sorted module list)
 *   2. Per-module: parse → analyze → Xi IR lower → optimize
 *   3. Cross-module import resolution via export_names + import table
 *   4. C code generation via xi_cgen
 *   5. Main() generation calling module inits in topo order
 *
 * RELATED MODULES:
 *   - xi_cgen.h: Xi IR → C code generation
 *   - xcmd_build.c: CLI entry that invokes xaot_build_xi + CC
 */

#ifndef XAOT_DRIVER_H
#define XAOT_DRIVER_H

#include "../base/xchecks.h"
#include <stdbool.h>
#include <stdint.h>

/* ========== Feature Set ========== */

/* Bitfield of stdlib modules referenced by the compiled bundle.
 * One bit per module — used to decide which stdlib .o files to link. */
typedef uint32_t XaotStdlibSet;

enum {
    XAOT_STDLIB_JSON = 1 << 0,
    XAOT_STDLIB_REGEX = 1 << 1,
    XAOT_STDLIB_MATH = 1 << 2,
    XAOT_STDLIB_TIME = 1 << 3,
    XAOT_STDLIB_PATH = 1 << 4,
    XAOT_STDLIB_IO = 1 << 5,
    XAOT_STDLIB_OS = 1 << 6,
    XAOT_STDLIB_NET = 1 << 7,
    XAOT_STDLIB_HTTP = 1 << 8,
    XAOT_STDLIB_CRYPTO = 1 << 9,
    XAOT_STDLIB_BASE64 = 1 << 10,
    XAOT_STDLIB_CSV = 1 << 11,
    XAOT_STDLIB_TOML = 1 << 12,
    XAOT_STDLIB_YAML = 1 << 13,
    XAOT_STDLIB_XML = 1 << 14,
    XAOT_STDLIB_COMPRESS = 1 << 15,
};

/* Runtime feature set inferred from analysis.
 * Each flag indicates whether the compiled bundle requires a particular
 * runtime subsystem.  Used to gate #define / link decisions so unused
 * subsystems can be stripped from the final binary. */
typedef struct {
    bool need_coro;
    bool need_channel;
    bool need_scope;
    bool need_timer;
    bool need_netpoll;
    bool need_deep_copy;
    bool need_exception;
    bool need_reflection;
    bool need_stacktrace;
    bool need_instanceof;
    XaotStdlibSet stdlib;
} XaotFeatureSet;

/* ========== Build API ========== */

/* Result of xaot_build_xi().  Caller must free c_source via xr_free(). */
typedef struct {
    char *c_source;          /* generated C program (malloc'd, caller frees) */
    int total_compiled;      /* number of functions successfully transpiled */
    int total_aot;           /* total AOT-eligible functions found */
    int nmodules;            /* number of modules in the bundle */
    XaotFeatureSet features; /* inferred feature set */
} XaotBuildResult;

/* Full AOT pipeline: Source → AST → Xi IR → C.
 * Supports single and multi-module bundles.
 * Returns 0 on success, non-zero on failure.
 * On success, result->c_source is a complete C program.
 * Caller frees result->c_source via xr_free(). */
XR_FUNC int xaot_build_xi(const char *input_path, XaotBuildResult *result);

#endif  // XAOT_DRIVER_H
