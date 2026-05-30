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
 * Each entry maps a helper enum ID to the generated runtime contract and the
 * concrete helper function pointer.
 */
const XmHelperInfo xm_helper_info[XM_HELPER__COUNT] = {
#define XM_HELPER_ENTRY_(name)                                                                     \
    [XM_HELPER_##name] = {                                                                         \
        .func = (void *) xr_jit_##name,                                                            \
        .ret_rep = XM_HELPER_RET_REP_##name,                                                       \
        .nargs = XM_HELPER_NARGS_##name,                                                           \
        .flags = XM_HELPER_FLAGS_##name,                                                           \
        .pointer_trust = XM_HELPER_POINTER_TRUST_##name,                                           \
        .post_call = XM_HELPER_POST_CALL_##name,                                                   \
    },
    XM_HELPER_DEF(XM_HELPER_ENTRY_)
#undef XM_HELPER_ENTRY_
};
