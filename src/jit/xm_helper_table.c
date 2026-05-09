/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_helper_table.c - JIT helper metadata table (generated from XM_HELPER_DEF)
 */

#include "xm_helper_table.h"
#include "xm_jit_runtime.h"

/* Generate the metadata array from the declaration table.
 * Each entry maps: helper enum ID → { function pointer, ret_rep, nargs, flags }
 */
const XmHelperInfo xm_helper_info[XM_HELPER__COUNT] = {
#define XM_HELPER_ENTRY_(name, nargs_, ret_rep_, flags_)                                           \
    [XM_HELPER_##name] = {                                                                         \
        .func = (void *) xr_jit_##name,                                                            \
        .ret_rep = (ret_rep_),                                                                     \
        .nargs = (nargs_),                                                                         \
        .flags = (flags_),                                                                         \
    },
    XM_HELPER_DEF(XM_HELPER_ENTRY_)
#undef XM_HELPER_ENTRY_
};
