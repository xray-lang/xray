/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit.h - Xi IR to VM bytecode emitter
 *
 * KEY CONCEPT:
 *   Translates typed SSA IR (XiFunc) into register-based bytecode (XrProto).
 *   This connects the new IR pipeline to the existing VM runtime.
 *
 *   Pipeline: AST -> xi_lower -> xi_opt -> xi_emit -> XrProto -> VM
 *
 *   The emitter performs:
 *     1. Register allocation (simple greedy assignment)
 *     2. Block linearization in RPO order
 *     3. Phi elimination via MOVE instructions
 *     4. Instruction selection (XiOp -> OpCode)
 *     5. Jump target patching
 *
 * CONSTRAINTS:
 *   - Max 255 registers (VM limit). Functions exceeding this fail.
 *   - Constants are added to XrProto.constants pool.
 *   - Line info is set to 0 (no source mapping in v1).
 */

#ifndef XI_EMIT_H
#define XI_EMIT_H

#include "xi.h"

struct XrProto;

/* Emit result status */
typedef enum {
    XI_EMIT_OK = 0,
    XI_EMIT_ERR_TOO_MANY_REGS,   /* function needs > 255 registers */
    XI_EMIT_ERR_TOO_MANY_CONSTS,  /* constant pool overflow */
    XI_EMIT_ERR_UNSUPPORTED_OP,   /* unhandled XiOp */
    XI_EMIT_ERR_INTERNAL,         /* unexpected internal error */
} XiEmitStatus;

/* Emit Xi IR function to a new XrProto.
 * Returns XI_EMIT_OK on success; on failure, *out_proto is NULL
 * and the status indicates the error kind.
 * Caller owns the returned XrProto (free with xr_vm_proto_free). */
XR_FUNC XiEmitStatus xi_emit(XiFunc *f, struct XrProto **out_proto);

/* Human-readable error string for emit status. */
XR_FUNC const char *xi_emit_status_str(XiEmitStatus s);

#endif  // XI_EMIT_H
