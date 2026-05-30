/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lto.h - Cross-module link-time optimization for Xi IR
 */

#ifndef XI_LTO_H
#define XI_LTO_H

#include "../ir/xi_module.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct XiLtoContext {
    XiModule **modules;
    uint32_t nmodules;
    /* Flat lookup: export name → callee XiFunc* */
    XiFunc **export_funcs;
    const char **export_names;
    uint32_t nexports;
} XiLtoContext;

/* Build cross-module export index from linked modules. */
XR_FUNC bool xi_lto_context_init(XiLtoContext *ctx, XiModule **modules, uint32_t nmodules);

XR_FUNC void xi_lto_context_free(XiLtoContext *ctx);

/* Resolve XI_IMPORT_REF + indirect calls to direct XiFunc targets
 * within the LTO context.  Returns number of edges resolved. */
XR_FUNC uint32_t xi_lto_resolve_calls(XiFunc *f, const XiLtoContext *ctx);

/* Run LTO resolution on every function in every module. */
XR_FUNC uint32_t xi_lto_link_modules(XiLtoContext *ctx);

#endif /* XI_LTO_H */
