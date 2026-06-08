/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_backend.h - Backend-stage op classification
 *
 * The Xi op descriptions define which opcodes are legal at XI_STAGE_BACKEND
 * and which pre-backend rewrites must run before code generation.
 *
 * Ops with verifier-only lowering are never backend-legal. Ops with a
 * generated backend rewrite must be rewritten by xi_backend_lower().
 */

#ifndef XI_BACKEND_H
#define XI_BACKEND_H

#include "xi_ops_gen.h"
#include <stdbool.h>
#include <stdint.h>

/* Returns true if the opcode is legal at STAGE_BACKEND. */
static inline bool xi_op_is_backend_legal(uint16_t op) {
    return xi_generated_op_backend_legal(op);
}

/* Returns the generated backend rewrite kind for non-legal ops. */
static inline uint8_t xi_op_backend_rewrite(uint16_t op) {
    return xi_generated_op_backend_rewrite(op);
}

/* Returns the rewrite target name when the generated rewrite needs one. */
static inline const char *xi_op_backend_rewrite_name(uint16_t op) {
    return xi_generated_op_backend_rewrite_name(op);
}

#endif /* XI_BACKEND_H */
