/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_symbol_check.c -- compile-time guard tying AOT-side method
 * symbol IDs (XRT_SYM_*) to the runtime SYMBOL_* enum.
 *
 * Auto-generated from xi_method_sym.def.  Each XI_METHOD_SYM entry
 * produces a _Static_assert pairing XRT_SYM_<aot_name> with
 * SYMBOL_<rt_name>.  Name differences (TOLOWER vs TOLOWERCASE,
 * HAS_NEXT vs HASNEXT) are handled by the rt_name column in the .def.
 *
 * This TU links into xray_core.  Drift fails the build immediately.
 */

#include "xrt_method_symbols.h"
#include "../runtime/symbol/xsymbol_table.h"

/* Auto-generated from xi_method_sym.def — one assert per entry */
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name)                                         \
    _Static_assert(XRT_SYM_##aot_name == SYMBOL_##rt_name,                                         \
                   "XRT_SYM_" #aot_name " drifted from SYMBOL_" #rt_name);
#include "../ir/xi_method_sym.def"
#undef XI_METHOD_SYM

/* SIZE is an alias for LENGTH */
_Static_assert(XRT_SYM_SIZE == SYMBOL_LENGTH, "XRT_SYM_SIZE alias drifted from SYMBOL_LENGTH");
