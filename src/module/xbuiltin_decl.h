/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuiltin_decl.h - Type declaration macros for gen_stdlib_types.py
 *
 * KEY CONCEPT:
 *   These macros are no-ops at compile time. They serve as structured
 *   annotations that gen_stdlib_types.py parses to generate:
 *   - xanalyzer_builtins_generated.h (--embed mode, for stdlib)
 *   - .xrd declaration files (--xrd mode, for third-party modules)
 *
 * USAGE:
 *   // @module net
 *   // @handle Connection { const fd: int, const type: string, const tls: bool }
 *   XR_DEFINE_BUILTIN(net_dial, "dial",
 *       "(host: string, port: int, timeout?: int): Connection?",
 *       "Dial a TCP connection")
 */

#ifndef XBUILTIN_DECL_H
#define XBUILTIN_DECL_H

/*
 * XR_DEFINE_BUILTIN(cfunc, name, signature, doc)
 *
 * Declares a function's type signature for the analyzer/LSP.
 * Expands to nothing at compile time - parsed only by gen_stdlib_types.py.
 *
 * @param cfunc     C function name (unused at compile time)
 * @param name      Exported function name as seen by xray scripts
 * @param signature Type signature, e.g. "(host: string, port: int): Json?"
 * @param doc       Short documentation string
 */
#define XR_DEFINE_BUILTIN(cfunc, name, signature, doc)

/*
 * Common module export helpers.
 * Eliminates per-module extern declarations and EXPORT_CFUNC macro duplication.
 */
struct XrCFunction;
struct XrayIsolate;
typedef XrValue (*XrCFunctionPtr)(struct XrayIsolate *, XrValue *, int);
extern struct XrCFunction *xr_vm_cfunction_new(struct XrayIsolate *isolate, XrCFunctionPtr func,
                                               const char *name);
extern XrValue xr_value_from_cfunction(struct XrCFunction *cfunc);

#define XR_MODULE_EXPORT(mod, isolate, name_str, func_ptr)                                         \
    do {                                                                                           \
        struct XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str);              \
        XrValue fn_val = xr_value_from_cfunction(cfunc);                                           \
        xr_module_add_export(isolate, mod, name_str, fn_val);                                      \
    } while (0)

#endif  // XBUILTIN_DECL_H
