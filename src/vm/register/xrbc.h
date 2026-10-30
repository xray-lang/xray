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
 *   trusted, in-process programs, not a serialized bytecode loader. Code and
 *   descriptors are validated once at preparation, never during dispatch.
 */

#ifndef XRBC_H
#define XRBC_H

#include "base/xdefs.h"
#include "runtime/native/xnative_execution.h"

#define XR_RBC_REGISTER_COUNT_MAX 255u
#define XR_RBC_SPILL_COUNT_MAX 65535u
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
    X(PRINT_LITERAL, ABX)                                                                          \
    X(CELL_NEW, ABX)                                                                               \
    X(CELL_READ, AB)                                                                               \
    X(CELL_WRITE, AB)                                                                              \
    X(STRING_LITERAL, ABX)                                                                         \
    X(STRING_I64, AB)                                                                              \
    X(STRING_CONCAT, ABC)                                                                          \
    X(STRING_RETAIN, AB)                                                                           \
    X(STRING_DROP, A)                                                                              \
    X(STRING_PRINT, A)                                                                             \
    X(STRING_TAKE, AB)                                                                             \
    X(STRING_CELL_NEW, ABC)                                                                        \
    X(STRING_CELL_DROP, A)                                                                         \
    X(RET, A)                                                                                      \
    X(RET0, NONE)                                                                                  \
    X(STRING_LEN, AB)                                                                              \
    X(SPILL, ABX)                                                                                  \
    X(RELOAD, ABX)                                                                                 \
    X(FNEG, AB)                                                                                    \
    X(FADD, ABC)                                                                                   \
    X(FSUB, ABC)                                                                                   \
    X(FMUL, ABC)                                                                                   \
    X(FDIV, ABC)                                                                                   \
    X(FEQ, ABC)                                                                                    \
    X(FNE, ABC)                                                                                    \
    X(FLT, ABC)                                                                                    \
    X(FLE, ABC)                                                                                    \
    X(FGT, ABC)                                                                                    \
    X(FGE, ABC)                                                                                    \
    X(STRING_F64, AB)                                                                               \
    X(ITOF, AB)                                                                                     \
    X(FTOI, AB)

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

typedef enum XrRbcRegisterKind {
    XR_RBC_BITS, XR_RBC_STRING, XR_RBC_STRING_CELL, XR_RBC_CALL_WINDOW
} XrRbcRegisterKind;

typedef struct {
    const uint32_t *code;
    size_t code_count;
    const uint64_t *constants;
    size_t constant_count;
    uint32_t register_count;
    uint32_t parameter_count;
    // NULL/0 is wholly unlocated. Otherwise count equals code_count and each ID
    // is zero or belongs to the execution's location owner.
    const uint32_t *source_ids;
    size_t source_id_count;
    // Cells belong to this activation. Handles are indices in its execution;
    // they may travel only to active callees, never outlive their owner.
    uint32_t cell_count;
    // NULL/0 declares exactly scalar-bit registers/cells. Managed functions
    // supply complete immutable tables; windows are typed at each real call.
    const XrRbcRegisterKind *register_kinds;
    size_t register_kind_count;
    const XrRbcRegisterKind *cell_kinds;
    size_t cell_kind_count;
    XrRbcRegisterKind result_kind;
    // Spills are activation-owned indexed storage. The String prefix is the
    // immutable reference bitmap; the remaining slots contain scalar bits.
    uint32_t spill_count, reference_spill_count;
} XrRbcFunction;

// The native set is closed. Dispatch calls each typed function directly;
// no native function pointer or pointer into a movable frame is stored.
typedef enum XrRbcNativeKind {
    XR_RBC_NATIVE_PRINT_I64,
    XR_RBC_NATIVE_CLOCK_REALTIME,
    XR_RBC_NATIVE_CLOCK_CPU,
    XR_RBC_NATIVE_CLOCK_MONOTONIC,
    XR_RBC_NATIVE_UTC_OFFSET_AT,
    XR_RBC_NATIVE_COUNT
} XrRbcNativeKind;

typedef struct {
    const char *bytes;
    size_t length;
} XrRbcLiteral;

typedef struct {
    const XrRbcFunction *functions;
    size_t function_count;
    const XrRbcNativeKind *natives;
    size_t native_count;
    // Immutable bytes share the same borrowed lifetime as code/descriptors.
    const XrRbcLiteral *literals;
    size_t literal_count;
} XrRbcModule;

typedef enum {
    XR_RBC_OK,
    XR_RBC_INVALID,
    XR_RBC_OUT_OF_MEMORY,
    XR_RBC_ARITHMETIC_PANIC,
    XR_RBC_OUTPUT_FAILURE,
    XR_RBC_HOST_CLOCK_FAILURE,
    XR_RBC_VALUE_FAILURE
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
// retains locations before any guest effects. Every function's source map is
// validated first, together with every instruction in every function, including
// unreachable code. Invalid register, constant, branch, comparison, and call
// operands reject preparation before any execution is published. Nonzero IDs
// belong to locations. Code/descriptors/maps stay
// immutable and borrowed until execution_free; mutating them violates this
// trusted in-process API. Runtime argument values, arithmetic failures and
// stack allocation failures remain execution-time concerns. Output is empty on entry;
// failure never publishes one.
// CALL A,Bx returns into A; the callee starts at A+1, the caller's top argument
// window. CALLN selects a fixed native kind with Bx. Integer output reads
// value/id at A+1/A+2; clocks read id at A+1 and return I64 bits into A.
// UTC offset reads timestamp/id at A+1/A+2 and also returns I64 bits into A.
// Callee locals may overwrite slots beyond the call window. LT/LE/EQ compare
// A,B and execute the following JMP only when the comparison is true.
// PRINT_LITERAL reserves A as zero and selects immutable bytes by Bx; its
// location is the instruction's source map entry, never a scalar register.
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
// Failure outcomes keep their owner independently of the executable. Retain
// that owner explicitly before outcome_free to keep it longer.
XR_FUNC bool xr_rbc_outcome_bits(const XrRbcOutcome *outcome, uint64_t *output);
XR_FUNC bool xr_rbc_outcome_string(const XrRbcOutcome *outcome, const XrtString **borrowed);
XR_FUNC bool xr_rbc_outcome_take_string(XrRbcOutcome *outcome, XrtString **output);
XR_FUNC bool xr_rbc_outcome_value_failure(const XrRbcOutcome *outcome,
    XrXirValueStatus *status, uint32_t *source_id, const XrtLocationOwner **borrowed_locations);
XR_FUNC bool xr_rbc_outcome_output_failure(const XrRbcOutcome *outcome,
    XrtOutputFailure *facts, const XrtLocationOwner **borrowed_locations);
XR_FUNC bool xr_rbc_outcome_arithmetic_panic(const XrRbcOutcome *outcome,
    XrtArithmeticPanic *facts, const XrtLocationOwner **borrowed_locations);
XR_FUNC bool xr_rbc_outcome_host_clock_failure(const XrRbcOutcome *outcome,
    XrtHostClockFailure *facts, const XrtLocationOwner **borrowed_locations);
XR_FUNC void xr_rbc_outcome_free(XrRbcOutcome **outcome);
XR_FUNC const char *xr_rbc_dispatch_name(void);

#endif  // XRBC_H
