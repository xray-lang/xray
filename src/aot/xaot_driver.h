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
 *   - xcmd_build.c: CLI entry that invokes xaot_build + CC
 */

#ifndef XAOT_DRIVER_H
#define XAOT_DRIVER_H

#include "../base/xchecks.h"
#include "xi_cgen.h"
#include "xaot_link.h"
#include "xaot_prepare.h"
#include <stdbool.h>
#include <stdint.h>

/* ========== Feature Set ========== */

#define XAOT_MAX_STDLIB_SYMBOLS 128
#define XAOT_STDLIB_SYMBOL_NAME_MAX 96
#define XAOT_MAX_EXTERN_DYLIBS 64
#define XAOT_EXTERN_DYLIB_NAME_MAX 512

/* Bitfield of stdlib modules referenced by the compiled bundle.
 * Stdlib calls are recorded at symbol/object granularity; coroutine/runtime
 * dependencies are represented separately as runtime_caps/runtime_objects. */
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
    XAOT_STDLIB_ENCODING = 1 << 16,
    XAOT_STDLIB_URL = 1 << 17,
    XAOT_STDLIB_DATETIME = 1 << 18,
    XAOT_STDLIB_LOG = 1 << 19,
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
    bool need_task;
    bool need_atomic;
    bool need_work_queue;
    bool need_result_group;
    bool need_countdown_latch;
    bool need_semaphore;
    bool need_event_count;
    bool need_generator;
    bool need_sys_thread;
    bool need_objects;
    bool need_deep_copy;
    bool need_exception;
    bool need_reflection;
    bool need_stacktrace;
    bool need_instanceof;
    XaotStdlibSet stdlib;
    char stdlib_symbols[XAOT_MAX_STDLIB_SYMBOLS][XAOT_STDLIB_SYMBOL_NAME_MAX];
    uint16_t n_stdlib_symbols;
    char extern_dylibs[XAOT_MAX_EXTERN_DYLIBS][XAOT_EXTERN_DYLIB_NAME_MAX];
    uint16_t n_extern_dylibs;
} XaotFeatureSet;

/* ========== Build API ========== */

/* One generated C translation unit.  Each becomes an independently compiled
 * object file; `name` is a stable per-module identifier used both for cache
 * addressing (content hash key seed) and diagnostics. */
typedef struct {
    char *name;     /* module name (cache key seed + diagnostics), malloc'd */
    char *c_source; /* generated C for this TU (malloc'd) */
} XaotModuleSource;

/* Result of xaot_build().  Caller must free owned strings via xr_free(). */
typedef struct {
    XaotModuleSource *sources; /* per-module generated C (malloc'd array) */
    int n_sources;             /* number of generated C translation units */
    char *plan_dump;           /* stable AOT prepare plan dump (malloc'd) */
    char *c_export_header;     /* public @c_export C declarations (malloc'd) */
    XaotLinkManifest link_manifest;
    int total_compiled;      /* number of functions successfully transpiled */
    int total_aot;           /* total AOT-eligible functions found */
    int nmodules;            /* number of modules in the bundle */
    XaotFeatureSet features; /* inferred feature set */
    XaotPrepareStats prepare_stats;
    XiCgenStats cgen_stats;
    XiCgenCoroFrameStats coro_frame_stats;
} XaotBuildResult;

typedef enum XaotBuildProfile {
    XAOT_BUILD_PROFILE_HOSTED = 0,
    XAOT_BUILD_PROFILE_FREESTANDING,
} XaotBuildProfile;

/* Full AOT pipeline: Source → AST → Xi IR → C.
 * Supports single and multi-module bundles.
 * Returns 0 on success, non-zero on failure.
 * On success, result->sources holds one generated C translation unit per
 * module (entry module last); each is compiled independently and linked
 * together, enabling per-module object caching.
 * When emit_plan_dump is true, result->plan_dump holds the stable AOT plan
 * text (for --dump-xaot-plan); otherwise it stays NULL and the O(N) dump is
 * skipped (it is pure diagnostics and most builds discard it).
 * Caller frees the result via xaot_build_result_free(). */
XR_FUNC int xaot_build(const char *input_path, bool emit_plan_dump, XaotBuildResult *result);
XR_FUNC int xaot_build_ex(const char *input_path, bool emit_plan_dump, bool emit_program_main,
                          XaotBuildProfile profile, XaotBuildResult *result);
XR_FUNC void xaot_build_result_free(XaotBuildResult *result);

#endif  // XAOT_DRIVER_H
