/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_effect.h - Opcode-to-effect table for Xi IR
 *
 * Each Xi opcode has a set of default effects that describe its
 * observable behavior.  The lowerer ORs additional flags as needed
 * (e.g. MAY_THROW on integer division), but the opcode defaults
 * provide a sound baseline.
 *
 * Consumers:
 *   - xi_lower*.c    : seed value->flags from the table
 *   - xi_verify.c    : assert flags are superset of opcode defaults
 *   - xi_opt*.c      : query whether an op reads/writes memory
 *   - xi_pipeline.c  : compute per-function effect summary
 */

#ifndef XI_EFFECT_H
#define XI_EFFECT_H

#include "xi.h"
#include "xi_ops_gen.h"

/* Return the default effect flags for a given XiOp.
 * These flags represent the *minimum* effects any instance of the op
 * must carry.  The lowerer may set additional flags (e.g. MAY_THROW
 * on a call that can throw). */
static inline uint8_t xi_op_default_effects(uint16_t op) {
    return xi_generated_op_default_flags(op);
}

/* Query helpers for optimization passes */

static inline bool xi_op_may_suspend(uint16_t op) {
    return (xi_op_default_effects(op) & XI_FLAG_MAY_SUSPEND) != 0;
}

static inline bool xi_op_reads_mem(uint16_t op) {
    return (xi_op_default_effects(op) & XI_FLAG_READS_MEM) != 0;
}

static inline bool xi_op_writes_mem(uint16_t op) {
    return (xi_op_default_effects(op) & XI_FLAG_WRITES_MEM) != 0;
}

static inline bool xi_op_is_pure(uint16_t op) {
    return xi_op_default_effects(op) == 0;
}

#endif  // XI_EFFECT_H
