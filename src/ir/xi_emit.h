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
 *   - VM operands are 16-bit; 0xffff is reserved internally as the emitter
 *     sentinel, so functions may use registers 0..65534.
 *   - Constants are added to XrProto.constants pool and encoded through
 *     widened 32-bit Bx payloads where applicable.
 *   - Source line info is preserved in XrProto.lineinfo for diagnostics and
 *     native debug mapping.
 */

#ifndef XI_EMIT_H
#define XI_EMIT_H

#include "xi.h"
#include "../base/xcompile_state.h"
#include "../base/xtarget_data_layout.h"

struct XrProto;
struct XrVMRuntime;
struct XrModuleGraph;

typedef enum XiEmitImportMode {
    XI_EMIT_IMPORT_PROGRAM = 0,
    XI_EMIT_IMPORT_RUNTIME_LOCATOR,
} XiEmitImportMode;

/* All inputs are genuine caller-owned facts, borrowed exclusively through the
 * complete recursive emit. The graph must use this exact parser state. Target
 * layout is validated and copied before use. Names retained by a proto are
 * copied or interned by its actual runtime; this descriptor never escapes. */
typedef struct XiEmitInput {
    XrCompileState *state;
    const XrTargetDataLayout *target;
    const struct XrModuleGraph *graph;
    const char *module_identity;
    XiEmitImportMode import_mode;
} XiEmitInput;

/* Emit result status */
typedef enum {
    XI_EMIT_OK = 0,
    XI_EMIT_ERR_TOO_MANY_REGS,   /* encoded register space exhausted */
    XI_EMIT_ERR_TOO_MANY_CONSTS, /* constant pool overflow */
    XI_EMIT_ERR_UNSUPPORTED_OP,  /* unhandled XiOp */
    XI_EMIT_ERR_INTERNAL,        /* unexpected internal error */
    XI_EMIT_ERR_RESOURCE,        /* original CompileState retains the exact first cause */
} XiEmitStatus;

/* Emit Xi IR function to a new XrProto.
 * Returns XI_EMIT_OK on success; on failure, *out_proto is NULL
 * and the status indicates the error kind.
 * Caller owns the returned XrProto (free with xr_instruction_unit_free). */
/* isolate is needed for string constant interning (may be NULL for no-string funcs) */
XR_FUNC XiEmitStatus xi_emit(XiFunc *f, struct XrVMRuntime *isolate,
                              const XiEmitInput *input, struct XrProto **out_proto);

/* Human-readable error string for emit status. */
XR_FUNC const char *xi_emit_status_str(XiEmitStatus s);

/* Attach Xi IR to proto for AOT direct lowering.
 * Transfers XiFunc ownership: proto will free it in xr_instruction_unit_free().
 * Must only be called once per proto. */
XR_FUNC bool xi_emit_attach_ir(struct XrProto *proto, XiFunc *ir);

#endif  // XI_EMIT_H
