/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpeephole.h - Peephole optimizer (instruction-level optimization)
*/

#ifndef XPEEPHOLE_H
#define XPEEPHOLE_H

#include "../../runtime/value/xchunk.h"
#include <stdbool.h>
#include "../../base/xdefs.h"

// ========== Peephole Optimizer ==========

/*
 * Perform peephole optimization on XrProto
 * 
 * Optimizations include:
 *   1. Jump chain elimination: JMP -> JMP -> target => JMP -> target
 *   2. Redundant instruction removal: duplicate LOADK etc.
 *   3. Dead code elimination: unreachable code after JMP
 *   4. Useless MOVE elimination: MOVE R[A], R[A]
 * 
 * Parameters:
 *   proto - function prototype to optimize
 * 
 * Returns:
 *   number of optimizations performed
 */
XR_FUNC int xr_peephole_optimize(XrProto *proto);

// ========== Sub-optimization Functions ==========

/*
 * Jump chain elimination
 * Jump chain optimization technique
 * 
 * Example:
 *   JMP +5
 *   ...
 *   JMP +10    ; pc=5
 *   ...
 *   target     ; pc=15
 * 
 * After optimization:
 *   JMP +15    ; jump directly to final target
 */
XR_FUNC int xr_peep_jump_chain(XrProto *proto);

/*
 * Redundant instruction removal
 * 
 * Example:
 *   LOADK R1, K(10)
 *   LOADK R1, K(20)  ; previous instruction is useless
 * 
 * After optimization:
 *   LOADK R1, K(20)
 */
XR_FUNC int xr_peep_redundant(XrProto *proto);

/*
 * Useless MOVE elimination
 * 
 * Example:
 *   MOVE R1, R1      ; useless
 * 
 * After optimization:
 *   NOP  (or removed)
 */
XR_FUNC int xr_peep_useless_move(XrProto *proto);

/*
 * NOP compression
 * Remove all NOP instructions and recalculate jump offsets
 * This optimization should be executed after all other optimizations
 * 
 * Example:
 *   LOADK R1, K(10)
 *   NOP              ; instruction removed by other optimizations
 *   ADD R2, R1, R3
 * 
 * After optimization:
 *   LOADK R1, K(10)
 *   ADD R2, R1, R3
 */
XR_FUNC int xr_peep_compress_nop(XrProto *proto);

/*
 * Tail call detection
 * Convert CALL+RETURN sequences to TAILCALL
 */
XR_FUNC int xr_peep_tail_call(XrProto *proto);

// ========== Helper Functions ==========

/*
 * Find final target of a jump
 * Follow jump chain until non-JMP instruction
 */
XR_FUNC int xr_peep_finaltarget(XrProto *proto, int pc, int size);

/*
 * Check if instruction is a jump instruction
 */
XR_FUNC bool xr_peep_is_jump(OpCode op);

/*
 * Check if instruction has no side effects
 */
XR_FUNC bool xr_peep_no_side_effect(OpCode op);

// ========== Optimization Statistics ==========

typedef struct PeepholeStats {
    int jump_chain_opt;     // jump chain optimization count
    int redundant_removed;  // redundant instructions removed
    int useless_move_removed; // useless MOVE removed
    int tail_call_opt;      // CALL+RETURN → TAILCALL conversions
    int nop_compressed;     // NOP compressed
    int total_optimizations; // total optimizations
} PeepholeStats;

extern PeepholeStats g_peephole_stats;

XR_FUNC void xr_peephole_reset_stats(void);
XR_FUNC void xr_peephole_print_stats(void);

#endif // XPEEPHOLE_H
