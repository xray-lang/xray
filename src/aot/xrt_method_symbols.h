/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_method_symbols.h - AOT method symbol IDs (constants only).
 *
 * Generated from src/ir/xi_method_sym.def (single source of truth).
 *
 * This header carries ONLY the XRT_SYM_* numeric IDs.  No AOT runtime
 * headers are pulled in, so translation units inside xray_core can
 * include it without dragging in static-inline bump-alloc helpers.
 *
 * IDs must match SYMBOL_* in src/runtime/symbol/xsymbol_table.h.
 * xrt_symbol_check.c auto-generates _Static_assert guards from the
 * same .def file; drift fails the build immediately.
 */

#ifndef XRT_METHOD_SYMBOLS_H
#define XRT_METHOD_SYMBOLS_H

/* Generated enum from xi_method_sym.def */
enum {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) \
    XRT_SYM_##aot_name = id,
#include "../ir/xi_method_sym.def"
#undef XI_METHOD_SYM
    XRT_SYM_COUNT_
};

/* Alias: analyzer emits LENGTH for both .length and .size */
#define XRT_SYM_SIZE XRT_SYM_LENGTH

#endif  // XRT_METHOD_SYMBOLS_H
