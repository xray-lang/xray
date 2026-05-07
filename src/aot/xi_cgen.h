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
 * bypassing bytecode and the legacy Xm builder entirely.
 * Generated code includes xrt.h for the value representation.
 */

#ifndef XI_CGEN_H
#define XI_CGEN_H

#include "../ir/xi.h"
#include <stdio.h>

/* Opaque codegen context — holds all mutable state for one C-generation
 * session.  Created once by the AOT driver, shared across module calls,
 * then freed.  No file-scope globals. */
typedef struct XiCgenCtx XiCgenCtx;

/* Lifecycle */
XR_FUNC XiCgenCtx *xi_cgen_ctx_new(void);
XR_FUNC void       xi_cgen_ctx_free(XiCgenCtx *ctx);

/* Generate a complete standalone C file (single-module fast path):
 *   #include "xrt.h" + forward decls + bodies + main()
 * Suitable for: cc -o out file.c */
XR_FUNC void xi_cgen_program(XiCgenCtx *ctx, FILE *out,
                              struct XiModule *module);

/* ========== Multi-module API ========== */

/* Emit the common C header (includes, defines). Call once per file. */
XR_FUNC void xi_cgen_header(FILE *out);

/* Resolve cross-module imports.  Populates ctx internal import table
 * from the module graph.  Must be called before xi_cgen_module(). */
XR_FUNC void xi_cgen_resolve_module_imports(XiCgenCtx *ctx,
                                             struct XiModule **modules,
                                             int nmodules);

/* Emit one module: module-scoped shared[], forward decls, function
 * bodies.  Does NOT emit #includes or main(). */
XR_FUNC void xi_cgen_module(XiCgenCtx *ctx, FILE *out,
                             struct XiModule *module);

/* Emit int main(void) calling module inits in topo order. */
XR_FUNC void xi_cgen_main(FILE *out, struct XiModule **modules,
                           int n, int entry_index);

#endif // XI_CGEN_H
