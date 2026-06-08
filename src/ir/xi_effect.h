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
 *   - xi_opt*.c      : query effects and speculation policy
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

static inline uint32_t xi_op_semantic_effects(uint16_t op) {
    return xi_generated_op_effects(op);
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

static inline bool xi_op_allocates(uint16_t op) {
    return (xi_op_semantic_effects(op) & XI_EFFECT_ALLOCATES) != 0;
}

static inline uint8_t xi_op_class(uint16_t op) {
    return xi_generated_op_class(op);
}

static inline bool xi_op_is_comparison(uint16_t op) {
    return xi_op_class(op) == XI_GEN_CLASS_COMPARISON;
}

static inline bool xi_op_can_speculate(uint16_t op) {
    return xi_generated_op_speculation(op) == XI_GEN_SPECULATION_SAFE;
}

static inline uint8_t xi_op_value_numbering_kind(uint16_t op) {
    return xi_generated_op_value_numbering(op);
}

static inline bool xi_op_value_numberable(uint16_t op) {
    return xi_op_value_numbering_kind(op) != XI_GEN_VN_NONE;
}

static inline bool xi_op_value_numbering_reads_mem(uint16_t op) {
    return xi_op_value_numbering_kind(op) == XI_GEN_VN_MEMORY_READ;
}

static inline bool xi_op_is_commutative(uint16_t op) {
    return (xi_generated_op_algebraic_traits(op) & XI_GEN_ALGEBRAIC_COMMUTATIVE) != 0;
}

static inline bool xi_op_is_associative(uint16_t op) {
    return (xi_generated_op_algebraic_traits(op) & XI_GEN_ALGEBRAIC_ASSOCIATIVE) != 0;
}

#endif  // XI_EFFECT_H
