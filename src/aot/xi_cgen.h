/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen.h - Xi IR to C code generation
 *
 * Translates typed SSA IR (XiFunc) directly to C source code,
 * bypassing bytecode and the machine-code Xm builder entirely.
 * Generated code includes xrt.h for the value representation.
 */

#ifndef XI_CGEN_H
#define XI_CGEN_H

#include "../ir/xi.h"
#include "../ir/xi_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Opaque codegen context — holds all mutable state for one C-generation
 * session.  Created once by the AOT driver, shared across module calls,
 * then freed.  No file-scope globals. */
typedef struct XiCgenCtx XiCgenCtx;
typedef struct XaotBundle XaotBundle;

typedef struct XiCgenCoroFrameStats {
    uint32_t coroutine_count;
    size_t total_frame_bytes;
    size_t max_frame_bytes;
    uint32_t total_roots;
    uint32_t total_releases;
    uint32_t max_roots;
    uint32_t max_releases;
} XiCgenCoroFrameStats;

typedef struct XiCgenStats {
    uint32_t functions_total;
    uint32_t functions_native_abi;
    uint32_t functions_tagged_abi;
    uint32_t functions_coro_abi;
    uint32_t boxed_adapters;
    uint32_t sync_go_wrappers;
    uint32_t xi_box_ops;
    uint32_t xi_unbox_ops;
} XiCgenStats;

/* Lifecycle */
XR_FUNC XiCgenCtx *xi_cgen_ctx_new(void);
XR_FUNC void xi_cgen_ctx_free(XiCgenCtx *ctx);
XR_FUNC void xi_cgen_ctx_set_aot_bundle(XiCgenCtx *ctx, const XaotBundle *bundle);
XR_FUNC bool xi_cgen_has_error(const XiCgenCtx *ctx);
XR_FUNC XiCgenCoroFrameStats xi_cgen_coro_frame_stats(const XiCgenCtx *ctx);
XR_FUNC XiCgenStats xi_cgen_stats(const XiCgenCtx *ctx);

/* Generate a complete standalone C file (single-module fast path):
 *   #include "xrt.h" + forward decls + bodies + main()
 * Suitable for: cc -o out file.c */
XR_FUNC void xi_cgen_program(XiCgenCtx *ctx, FILE *out, struct XiModule *module);

/* ========== Multi-module API ========== */

/* Emit the common C header (includes, defines). Call once per file. */
XR_FUNC void xi_cgen_header(FILE *out);

/* Resolve cross-module imports.  Populates ctx internal import table
 * from the module graph.  Must be called before xi_cgen_module(). */
XR_FUNC void xi_cgen_resolve_module_imports(XiCgenCtx *ctx, struct XiModule **modules,
                                            int nmodules);

/* Emit one module: module-scoped shared[], forward decls, function
 * bodies.  Does NOT emit #includes or main(). */
XR_FUNC void xi_cgen_module(XiCgenCtx *ctx, FILE *out, struct XiModule *module);

/* Emit main() calling module inits in topo order. */
XR_FUNC void xi_cgen_main(XiCgenCtx *ctx, FILE *out, struct XiModule **modules, int n,
                          int entry_index);

/* Emit static const xrt_str_t definitions for every string literal
 * interned while emitting module bodies.  Multi-module flow: buffer
 * module/main output in a memstream, emit these defs after the header,
 * then append the buffered bodies (definitions must precede uses). */
XR_FUNC void xi_cgen_emit_str_literal_defs(XiCgenCtx *ctx, FILE *out);

#endif  // XI_CGEN_H
