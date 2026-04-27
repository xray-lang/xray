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
#include <stdbool.h>
#include <stdint.h>

/* ========== Feature Inference ========== */

/* Bitfield of stdlib modules referenced by the compiled bundle.
 * One bit per module — used to decide which stdlib .o files to link. */
typedef uint32_t XaotStdlibSet;

enum {
    XAOT_STDLIB_JSON     = 1 << 0,
    XAOT_STDLIB_REGEX    = 1 << 1,
    XAOT_STDLIB_MATH     = 1 << 2,
    XAOT_STDLIB_TIME     = 1 << 3,
    XAOT_STDLIB_PATH     = 1 << 4,
    XAOT_STDLIB_IO       = 1 << 5,
    XAOT_STDLIB_OS       = 1 << 6,
    XAOT_STDLIB_NET      = 1 << 7,
    XAOT_STDLIB_HTTP     = 1 << 8,
    XAOT_STDLIB_CRYPTO   = 1 << 9,
    XAOT_STDLIB_BASE64   = 1 << 10,
    XAOT_STDLIB_CSV      = 1 << 11,
    XAOT_STDLIB_TOML     = 1 << 12,
    XAOT_STDLIB_YAML     = 1 << 13,
    XAOT_STDLIB_XML      = 1 << 14,
    XAOT_STDLIB_COMPRESS = 1 << 15,
};

/* Runtime feature set inferred from bytecode analysis.
 * Each flag indicates whether the compiled bundle requires a particular
 * runtime subsystem.  Used to gate #define / link decisions so unused
 * subsystems can be stripped from the final binary. */
typedef struct {
    bool need_coro;        /* OP_GO / OP_GO_SCOPE / OP_AWAIT */
    bool need_channel;     /* OP_CHAN_NEW / OP_CHAN_SEND / OP_CHAN_RECV / OP_SELECT */
    bool need_scope;       /* OP_SCOPE_BEGIN */
    bool need_timer;       /* time.sleep / time.after */
    bool need_netpoll;     /* net.* / tcp.* / http.* */
    bool need_deep_copy;   /* need_coro && (need_channel || GO with arguments) */
    bool need_exception;   /* OP_TRY_BEGIN / OP_THROW */
    bool need_reflection;  /* typeof / class.fields() / class.name() */
    bool need_stacktrace;  /* need_exception && stack trace usage */
    bool need_instanceof;  /* OP_IS */
    XaotStdlibSet stdlib;  /* bitset of required stdlib modules */
} XaotFeatureSet;

/* ========== Build API ========== */

/* Result of xaot_build().  Caller must free c_source via xr_free(). */
typedef struct {
    char *c_source;        /* generated C program (malloc'd, caller frees) */
    int total_compiled;    /* number of functions successfully transpiled */
    int total_aot;         /* total AOT-eligible functions found */
    int nmodules;          /* number of modules in the bundle */
    XaotFeatureSet features; /* inferred feature set */
} XaotBuildResult;

/* Run the full AOT pipeline for a given source file.
 * Returns 0 on success, non-zero on failure.
 * On success, result->c_source is a complete C program (includes +
 * transpiled functions + main()).  Caller frees result->c_source. */
XR_FUNC int xaot_build(const char *input_path, XaotBuildResult *result);

#endif  // XAOT_DRIVER_H
