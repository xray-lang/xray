/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_fold.h - On-the-fly peephole optimization (FOLD engine)
 *
 * KEY CONCEPT:
 *   Optimizes instructions at emit time, before they enter the IR.
 *   Instead of post-hoc passes, algebraic simplifications are applied
 *   eagerly during IR construction.
 *   This reduces the number of instructions that downstream passes
 *   (DCE, CSE, GVN, regalloc) need to process.
 *
 * RULES IMPLEMENTED:
 *   - Identity elimination:  x+0→x, x*1→x, x|0→x, x&-1→x, x^0→x, x<<0→x
 *   - Annihilation:          x*0→0, x&0→0
 *   - Self-operation:        x-x→0, x^x→0, x&x→x, x|x→x
 *   - Double negation:       NEG(NEG(x))→x, NOT(NOT(x))→x, FNEG(FNEG(x))→x
 *   - Conversion roundtrip:  F2I(I2F(x))→x, I2F(F2I(x))→x (when safe)
 *   - Constant folding:      CONST op CONST → CONST (integer and float)
 *
 * RELATED MODULES:
 *   - xm.h: Xm data structures and xm_emit()
 *   - xi_to_xm.c: Xi IR → Xm lowering (calls fold_emit)
 *   - xm_pass.c: post-hoc optimization passes
 */

#ifndef XM_FOLD_H
#define XM_FOLD_H

#include "xm.h"
#include "../base/xdefs.h"

/*
 * Optimizing emit: attempts peephole optimization before emitting.
 * If the instruction can be simplified, returns the simplified result
 * without emitting a new instruction. Otherwise delegates to xm_emit().
 *
 * Parameters are identical to xm_emit().
 */
XR_FUNC XmRef xm_fold_emit(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a, XmRef b);

/*
 * Optimizing unary emit: wrapper for single-operand instructions.
 */
XR_FUNC XmRef xm_fold_emit_unary(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a);

#endif  // XM_FOLD_H
