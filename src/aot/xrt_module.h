/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_module.h - AOT module initialization and export tables
 *
 * KEY CONCEPT:
 *   Defines types for module-level export tables and init sequencing.
 *   Each AOT-compiled module exposes:
 *     - An export table (XrtModuleExport[]) for runtime lookup
 *     - An init function (void mod__init(XrtContext)) called once at startup
 *
 *   Module init runs in topological dependency order before main().
 *
 * RELATED MODULES:
 *   - xcgen.c: generates module init/export code
 *   - xrt_compat.h: XrValue, XrtContext types
 */

#ifndef XRT_MODULE_H
#define XRT_MODULE_H

#include "xrt_compat.h"

/* =========================================================================
 * Export Flags
 * ========================================================================= */

#define XRT_EXPORT_FN 0     // function export
#define XRT_EXPORT_CONST 1  // const binding (immutable)
#define XRT_EXPORT_LET 2    // let binding (mutable)

/* =========================================================================
 * Module Export Entry
 * ========================================================================= */

typedef struct XrtModuleExport {
    const char *name;    // export name (e.g. "add", "PI")
    XrValue *value_ptr;  // pointer to the module-level XrValue
    uint8_t flags;       // XRT_EXPORT_FN / XRT_EXPORT_CONST / XRT_EXPORT_LET
} XrtModuleExport;

/* =========================================================================
 * Module Descriptor (one per AOT-compiled module)
 * ========================================================================= */

typedef struct XrtModule {
    const char *name;                // module name (e.g. "math")
    const char *path;                // source path
    void (*init_fn)(XrtContext);     // module init function
    const XrtModuleExport *exports;  // export table
    int nexports;                    // number of exports
    int initialized;                 // 0=pending, 1=done
} XrtModule;

/* =========================================================================
 * Module Init Sequencing
 * ========================================================================= */

// Call all module init functions in dependency order.
// modules[] must be pre-sorted in topological order (leaves first).
static inline void xrt_modules_init(XrtModule *modules, int nmodules, XrtContext ctx) {
    for (int i = 0; i < nmodules; i++) {
        if (!modules[i].initialized && modules[i].init_fn) {
            modules[i].init_fn(ctx);
            modules[i].initialized = 1;
        }
    }
}

// Lookup an export by name across all initialized modules.
// Returns pointer to the value, or NULL if not found.
static inline XrValue *xrt_module_lookup(XrtModule *modules, int nmodules, const char *module_name,
                                         const char *export_name) {
    for (int i = 0; i < nmodules; i++) {
        if (modules[i].name && strcmp(modules[i].name, module_name) == 0) {
            for (int j = 0; j < modules[i].nexports; j++) {
                if (strcmp(modules[i].exports[j].name, export_name) == 0)
                    return modules[i].exports[j].value_ptr;
            }
            return NULL;
        }
    }
    return NULL;
}

#endif  // XRT_MODULE_H
