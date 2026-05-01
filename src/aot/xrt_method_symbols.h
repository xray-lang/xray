/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_method_symbols.h - AOT method symbol IDs (constants only).
 *
 * KEY CONCEPT:
 *   This header carries ONLY the XRT_SYM_* numeric IDs. It deliberately
 *   pulls in no AOT runtime headers (no xrt_arc.h, no xrt_coll.h, no
 *   xrt_value.h), so translation units inside xray_core that just need
 *   the symbol numbers can include it without dragging in static-inline
 *   bump-alloc helpers.
 *
 *   Pulling xrt_arc.h into xray_core via xrt_method.h was the root
 *   cause of GNU ld undefined references to xrt_bump_cursor /
 *   xrt_bump_blocks / xrt_bump_end / xrt_bump_enabled: those symbols
 *   are only defined inside an AOT-generated translation unit (the one
 *   that #defines XRT_IMPL), but every TU including xrt_arc.h would
 *   leave a static-inline xrt_arc_alloc copy referencing them.
 *
 *   xrt_method.h itself includes this file, so AOT-generated code that
 *   needs the full method dispatch surface still gets the IDs.
 *
 *   IDs must match SYMBOL_* in src/runtime/symbol/xsymbol_table.h.
 *   xrt_symbol_check.c pairs every XRT_SYM_X with SYMBOL_X via
 *   _Static_assert; drift fails the build before any miscompiled AOT
 *   binary ships.
 */

#ifndef XRT_METHOD_SYMBOLS_H
#define XRT_METHOD_SYMBOLS_H

#define XRT_SYM_LENGTH 1
#define XRT_SYM_SIZE 1 /* alias - compiler always emits LENGTH */
#define XRT_SYM_IS_EMPTY 2
#define XRT_SYM_HAS 3
#define XRT_SYM_GET 4
#define XRT_SYM_SET 5
#define XRT_SYM_DELETE 6
#define XRT_SYM_CLEAR 7
#define XRT_SYM_KEYS 8
#define XRT_SYM_VALUES 9
#define XRT_SYM_CHARAT 17
#define XRT_SYM_SUBSTRING 18
#define XRT_SYM_SLICE 16
#define XRT_SYM_INDEXOF 19
#define XRT_SYM_CONTAINS 20
#define XRT_SYM_STARTSWITH 21
#define XRT_SYM_ENDSWITH 22
#define XRT_SYM_TOLOWER 23
#define XRT_SYM_TOUPPER 24
#define XRT_SYM_TRIM 25
#define XRT_SYM_TRIM_START 35
#define XRT_SYM_TRIM_END 36
#define XRT_SYM_SPLIT 26
#define XRT_SYM_REPLACE 27
#define XRT_SYM_REPLACEALL 28
#define XRT_SYM_REPEAT 29
#define XRT_SYM_CONCAT 30
#define XRT_SYM_BYTE_AT 31
#define XRT_SYM_PAD_START 37
#define XRT_SYM_PAD_END 38
#define XRT_SYM_LASTINDEXOF 39
#define XRT_SYM_TOINT 40
#define XRT_SYM_TOFLOAT 41
#define XRT_SYM_ORD 46
#define XRT_SYM_PUSH 48
#define XRT_SYM_POP 49
#define XRT_SYM_SHIFT 50
#define XRT_SYM_UNSHIFT 51
#define XRT_SYM_JOIN 52
#define XRT_SYM_REVERSE 53
#define XRT_SYM_FLOOR 58
#define XRT_SYM_CEIL 59
#define XRT_SYM_ROUND 60
#define XRT_SYM_ABS 61
#define XRT_SYM_SQRT 62
#define XRT_SYM_POW 63
#define XRT_SYM_TOFIXED 64
#define XRT_SYM_MAX 66
#define XRT_SYM_MIN 67
#define XRT_SYM_TOHEX 68
#define XRT_SYM_TOSTRING 85
#define XRT_SYM_FILL 168
#define XRT_SYM_SORT 169
#define XRT_SYM_INCLUDES 170
#define XRT_SYM_FOREACH 12
#define XRT_SYM_MAP 13
#define XRT_SYM_FILTER 14
#define XRT_SYM_REDUCE 15
#define XRT_SYM_ITERATOR 54
#define XRT_SYM_HAS_NEXT 56
#define XRT_SYM_NEXT 57

#endif  // XRT_METHOD_SYMBOLS_H
