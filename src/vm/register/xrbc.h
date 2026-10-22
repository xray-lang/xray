/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc.h - Typed scalar register bytecode.
 *
 * KEY CONCEPT:
 *   Instructions occupy one 32-bit word. Registers contain untagged bits;
 *   instruction selection supplies their types. This is an internal API for
 *   trusted, in-process programs, not a serialized bytecode loader.
 */

#ifndef XRBC_H
#define XRBC_H

#include "base/xdefs.h"
#include "runtime/native/xnative_execution.h"

#define XR_RBC_REGISTER_COUNT_MAX 255u
#define XR_RBC_JUMP_MIN (-8388608)
#define XR_RBC_JUMP_MAX 8388607

// A zero operand in a format is reserved and must be encoded as zero.
#define XR_RBC_OPS(X)                                                                              \
    X(MOV, AB)                                                                                     \
    X(LOADK, ABX)                                                                                  \
    X(LOADI, ASBX)                                                                                 \
    X(ADD, ABC)                                                                                    \
    X(SUB, ABC)                                                                                    \
    X(MUL, ABC)                                                                                    \
    X(NEG, AB)                                                                                     \
    X(DIV, ABC)                                                                                    \
    X(REM, ABC)                                                                                    \
    X(AND, ABC)                                                                                    \
    X(OR, ABC)                                                                                     \
    X(XOR, ABC)                                                                                    \
    X(SHL, ABC)                                                                                    \
    X(SHR, ABC)                                                                                    \
    X(USHR, ABC)                                                                                   \
    X(LT, AB)                                                                                      \
    X(LE, AB)                                                                                      \
    X(EQ, AB)                                                                                      \
    X(JMP, SJ)                                                                                     \
    X(CALL, ABX)                                                                                   \
    X(CALLN, ABX)                                                                                  \
    X(RET, A)                                                                                      \
    X(RET0, NONE)

typedef enum {
#define XR_RBC_ENUM(name, format) XR_RBC_##name,
    XR_RBC_OPS(XR_RBC_ENUM)
#undef XR_RBC_ENUM
    XR_RBC_OP_COUNT
} XrRbcOp;

typedef struct {
    XrRbcOp op;
    uint32_t a, b, c;
    int64_t immediate;
} XrRbcOperands;

typedef struct {
    const uint32_t *code;
    size_t code_count;
    const uint64_t *constants;
    size_t constant_count;
    uint32_t register_count;
    uint32_t parameter_count;
} XrRbcFunction;

// Only the registered output leaf is admitted. Both physical arguments are
// passed by value; no native pointer refers into the movable register stack.
typedef void (*XrRbcNativeI64)(int64_t value, uint32_t source_id);

typedef struct {
    const XrRbcFunction *functions;
    size_t function_count;
    const XrRbcNativeI64 *natives;
    size_t native_count;
} XrRbcModule;

typedef enum {
    XR_RBC_OK,
    XR_RBC_INVALID,
    XR_RBC_OUT_OF_MEMORY,
    XR_RBC_DIVIDE_BY_ZERO,
    XR_RBC_OUTPUT_FAILURE
} XrRbcStatus;

// Failure leaves the output untouched. Register 255 is reserved; every
// register operand must fit a frame with at most 255 slots (0 through 254).
XR_FUNC bool xr_rbc_encode(uint32_t *out, XrRbcOperands operands);
// Offsets are relative to the instruction following JMP. Both endpoints must
// identify instructions in the function, including unreachable instructions.
XR_FUNC bool xr_rbc_jump(uint32_t *out, size_t from, size_t to, size_t code_count);

typedef struct XrRbcExecution XrRbcExecution;
typedef struct XrRbcOutcome XrRbcOutcome;

// Prepare copies arguments, owns the initial stack and pending outcome, and
// retains locations before any guest effects. Code/descriptors stay borrowed
// until execution_free. Output must be empty; failure never publishes one.
// CALL A,Bx returns into A; the callee starts at A+1, the caller's top argument
// window. CALLN reads value/id at A+1/A+2, with Bx selecting the actual leaf.
// Callee locals may overwrite slots beyond the call window. LT/LE/EQ compare
// A,B and execute the following JMP only when the comparison is true.
XR_FUNC XrRbcStatus xr_rbc_module_prepare(const XrRbcModule *module, uint32_t entry,
    const uint64_t *arguments, size_t argument_count, XrtLocationOwner *locations,
    XrRbcExecution **output);
// A valid single-use run transfers its already allocated non-NULL outcome,
// including ordinary VM failure. NULL/consumed misuse invokes no guest.
XR_FUNC XrRbcOutcome *xr_rbc_execution_run(XrRbcExecution *execution);
XR_FUNC void xr_rbc_execution_free(XrRbcExecution **execution);
XR_FUNC XrRbcStatus xr_rbc_outcome_status(const XrRbcOutcome *outcome);
// Query handles must be live. False preserves every output; true copies bits
// or scalar facts and borrows locations only through the outcome's lifetime.
XR_FUNC bool xr_rbc_outcome_bits(const XrRbcOutcome *outcome, uint64_t *output);
XR_FUNC bool xr_rbc_outcome_output_failure(const XrRbcOutcome *outcome,
    XrtOutputFailure *facts, const XrtLocationOwner **borrowed_locations);
XR_FUNC void xr_rbc_outcome_free(XrRbcOutcome **outcome);
XR_FUNC const char *xr_rbc_dispatch_name(void);

#endif  // XRBC_H
