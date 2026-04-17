/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_helper_table.c - JIT helper metadata table (generated from XIR_HELPER_DEF)
 */

#include "xir_helper_table.h"
#include "xir_jit_runtime.h"

/* Generate the metadata array from the declaration table.
 * Each entry maps: helper enum ID → { function pointer, ret_rep, nargs, flags }
 */
const XirHelperInfo xir_helper_info[XIR_HELPER__COUNT] = {
#define XIR_HELPER_ENTRY_(name, nargs_, ret_rep_, flags_) \
    [XIR_HELPER_##name] = { \
        .func    = (void *)xr_jit_##name, \
        .ret_rep = (ret_rep_), \
        .nargs   = (nargs_), \
        .flags   = (flags_), \
    },
    XIR_HELPER_DEF(XIR_HELPER_ENTRY_)
#undef XIR_HELPER_ENTRY_
};
