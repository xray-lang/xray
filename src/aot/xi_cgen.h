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

/* Generate a C function definition for a Xi IR function and all its
 * nested children. Output is appended to `out`.
 * `name_prefix` is prepended to function names (e.g. module name). */
XR_FUNC void xi_cgen_func(FILE *out, XiFunc *f, const char *name_prefix);

/* Generate a complete standalone C file:
 *   - #include "xrt.h"
 *   - Forward declarations
 *   - All function bodies (main_func and its children)
 *   - int main(void) { ... } entry point
 * Suitable for direct compilation with cc -o out file.c */
XR_FUNC void xi_cgen_program(FILE *out, XiFunc *main_func,
                              const char *module_name);

/* ========== Multi-module API ========== */

/* Emit the common C header (includes, defines). Call once per file. */
XR_FUNC void xi_cgen_header(FILE *out);

/* Emit one module: module-scoped shared[], forward declarations, and
 * all function bodies.  Does NOT emit #includes or main().
 * `module_name` is the C-safe module prefix for function names. */
XR_FUNC void xi_cgen_module(FILE *out, XiFunc *module_func,
                             const char *module_name);

/* Emit int main(void) that calls module init functions in order.
 * `module_names` and `module_funcs` are parallel arrays of size `n`.
 * `entry_index` is the index of the entry-point module. */
XR_FUNC void xi_cgen_main(FILE *out, const char **module_names,
                           XiFunc **module_funcs, int n, int entry_index);

/* Resolve cross-module imports from a module graph.  Builds the internal
 * import table by matching each exporter's exports[] against all importers.
 * Must be called after xi_module_populate_exports() on each module and
 * before xi_cgen_module().  Requires nmodules > 1 to have any effect. */
XR_FUNC void xi_cgen_resolve_module_imports(struct XiModule **modules,
                                             int nmodules);

#endif // XI_CGEN_H
