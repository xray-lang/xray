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
 * KEY POINTS:
 *   - The AOT-generated C output is fully self-contained and cannot
 *     #include runtime headers, so xrt_method.h has to declare its
 *     own XRT_SYM_* constants. Without a guard those constants would
 *     drift the moment SYMBOL_* renumbers, and AOT-emitted dispatch
 *     would silently route methods to the wrong slot.
 *
 *   - This translation unit links into xray_core (NOT into the
 *     AOT-generated output). It only contains static_asserts that
 *     pair every XRT_SYM_X with the matching SYMBOL_X. Drift fails
 *     the runtime build immediately rather than producing miscompiled
 *     binaries.
 *
 *   - Aliases (XRT_SYM_TOLOWER == SYMBOL_TOLOWERCASE,
 *     XRT_SYM_TOUPPER == SYMBOL_TOUPPERCASE, XRT_SYM_SIZE ==
 *     SYMBOL_LENGTH) are documented inline because their names do
 *     not match.
 */

/*
 * Use the lightweight constants-only header. Including the full
 * xrt_method.h here would drag in xrt_arc.h's static-inline allocator
 * helpers, which leave unresolved references to AOT-only globals
 * (xrt_bump_cursor / xrt_bump_blocks / xrt_bump_end / xrt_bump_enabled)
 * inside this xray_core translation unit on GNU ld.
 */
#include "xrt_method_symbols.h"
#include "../runtime/symbol/xsymbol_table.h"

_Static_assert(XRT_SYM_LENGTH == SYMBOL_LENGTH,
               "XRT_SYM_LENGTH drifted from runtime SYMBOL_LENGTH");
/* SIZE is an analyzer-side alias; compiler always emits LENGTH. */
_Static_assert(XRT_SYM_SIZE == SYMBOL_LENGTH, "XRT_SYM_SIZE alias drifted from SYMBOL_LENGTH");
_Static_assert(XRT_SYM_IS_EMPTY == SYMBOL_IS_EMPTY, "XRT_SYM_IS_EMPTY drift");
_Static_assert(XRT_SYM_HAS == SYMBOL_HAS, "XRT_SYM_HAS drift");
_Static_assert(XRT_SYM_GET == SYMBOL_GET, "XRT_SYM_GET drift");
_Static_assert(XRT_SYM_SET == SYMBOL_SET, "XRT_SYM_SET drift");
_Static_assert(XRT_SYM_DELETE == SYMBOL_DELETE, "XRT_SYM_DELETE drift");
_Static_assert(XRT_SYM_CLEAR == SYMBOL_CLEAR, "XRT_SYM_CLEAR drift");
_Static_assert(XRT_SYM_KEYS == SYMBOL_KEYS, "XRT_SYM_KEYS drift");
_Static_assert(XRT_SYM_VALUES == SYMBOL_VALUES, "XRT_SYM_VALUES drift");
_Static_assert(XRT_SYM_SLICE == SYMBOL_SLICE, "XRT_SYM_SLICE drift");
_Static_assert(XRT_SYM_INDEXOF == SYMBOL_INDEXOF, "XRT_SYM_INDEXOF drift");
_Static_assert(XRT_SYM_CONTAINS == SYMBOL_CONTAINS, "XRT_SYM_CONTAINS drift");
_Static_assert(XRT_SYM_STARTSWITH == SYMBOL_STARTSWITH, "XRT_SYM_STARTSWITH drift");
_Static_assert(XRT_SYM_ENDSWITH == SYMBOL_ENDSWITH, "XRT_SYM_ENDSWITH drift");
/* Naming differs: AOT uses TOLOWER/TOUPPER, runtime uses TOLOWERCASE/TOUPPERCASE. */
_Static_assert(XRT_SYM_TOLOWER == SYMBOL_TOLOWERCASE, "XRT_SYM_TOLOWER drift");
_Static_assert(XRT_SYM_TOUPPER == SYMBOL_TOUPPERCASE, "XRT_SYM_TOUPPER drift");
_Static_assert(XRT_SYM_TRIM == SYMBOL_TRIM, "XRT_SYM_TRIM drift");
_Static_assert(XRT_SYM_SPLIT == SYMBOL_SPLIT, "XRT_SYM_SPLIT drift");
_Static_assert(XRT_SYM_REPLACE == SYMBOL_REPLACE, "XRT_SYM_REPLACE drift");
_Static_assert(XRT_SYM_REPEAT == SYMBOL_REPEAT, "XRT_SYM_REPEAT drift");
_Static_assert(XRT_SYM_PUSH == SYMBOL_PUSH, "XRT_SYM_PUSH drift");
_Static_assert(XRT_SYM_POP == SYMBOL_POP, "XRT_SYM_POP drift");
_Static_assert(XRT_SYM_JOIN == SYMBOL_JOIN, "XRT_SYM_JOIN drift");
_Static_assert(XRT_SYM_REVERSE == SYMBOL_REVERSE, "XRT_SYM_REVERSE drift");
_Static_assert(XRT_SYM_SORT == SYMBOL_SORT, "XRT_SYM_SORT drift");
_Static_assert(XRT_SYM_INCLUDES == SYMBOL_INCLUDES, "XRT_SYM_INCLUDES drift");
_Static_assert(XRT_SYM_FLOOR == SYMBOL_FLOOR, "XRT_SYM_FLOOR drift");
_Static_assert(XRT_SYM_CEIL == SYMBOL_CEIL, "XRT_SYM_CEIL drift");
_Static_assert(XRT_SYM_ROUND == SYMBOL_ROUND, "XRT_SYM_ROUND drift");
_Static_assert(XRT_SYM_ABS == SYMBOL_ABS, "XRT_SYM_ABS drift");
_Static_assert(XRT_SYM_SQRT == SYMBOL_SQRT, "XRT_SYM_SQRT drift");
_Static_assert(XRT_SYM_POW == SYMBOL_POW, "XRT_SYM_POW drift");
_Static_assert(XRT_SYM_TOFIXED == SYMBOL_TOFIXED, "XRT_SYM_TOFIXED drift");
_Static_assert(XRT_SYM_TOSTRING == SYMBOL_TOSTRING, "XRT_SYM_TOSTRING drift");
