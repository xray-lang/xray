/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xinline.h - Function inlining analyzer
 */

#ifndef XINLINE_H
#define XINLINE_H

#include "../../runtime/value/xchunk.h"
#include <stdbool.h>
#include "../../base/xdefs.h"

#define INLINE_MAX_INSTRUCTIONS 10
#define INLINE_MAX_PARAMS 4
#define INLINE_MAX_LOCALS 8

typedef struct InlineCandidate {
    bool can_inline;
    int instruction_count;
    int param_count;
    int local_count;
    bool has_loops;
    bool has_recursion;
    bool has_closure;
    int call_count;
} InlineCandidate;

/*
** Analyze whether the function is suitable for inlining
**
** Judgment conditions:
**   1.  Instructions <= INLINE_MAX_INSTRUCTIONS
**   2.  No looping (no JMP rebound)
**   3.  No recursive calls
**   4.  No closure creation
**   5.  The number of parameters is reasonable
**
*/
XR_FUNC bool xr_inline_analyze(XrProto *proto, InlineCandidate *candidate);
XR_FUNC int xr_inline_mark_candidates(XrProto *proto);

XR_FUNC bool xr_inline_has_loops(XrProto *proto);
XR_FUNC bool xr_inline_has_recursion(XrProto *proto);
XR_FUNC bool xr_inline_has_closure(XrProto *proto);
XR_FUNC void xr_inline_detect_indirect_recursion(XrProto *root);

// Calculate the "complexity" of a function
// The smaller the return value, the more suitable it is for inlining
XR_FUNC int xr_inline_complexity(XrProto *proto);

typedef struct InlineStats {
    int total_functions;
    int inline_candidates;
    int too_large;
    int has_loops;
    int has_recursion;
} InlineStats;

extern InlineStats g_inline_stats;

XR_FUNC void xr_inline_reset_stats(void);
XR_FUNC void xr_inline_print_stats(void);

#endif  // XINLINE_H
