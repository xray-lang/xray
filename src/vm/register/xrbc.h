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
#include "shared/xr_math_host_ops.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_module_state.h"
#include "xray_native_values.h"
#include "xray_native_function.h"

#define XR_RBC_REGISTER_COUNT_MAX 255u
#define XR_RBC_SPILL_COUNT_MAX 65535u
/* A direct callee receives at most this many physical scalar parameters in
 * its register window. Further scalar parameters arrive in its spill bank,
 * like stack-passed arguments in a native calling convention. The bound keeps
 * two full windows (its own parameters and an outgoing call) plus temporaries
 * inside the 255 addressable registers of one activation. */
#define XR_RBC_REGISTER_PARAMETER_MAX 64u
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
    X(RET, A)                                                                                      \
    X(RET0, NONE)                                                                                  \
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
    X(ITOF, AB)                                                                                     \
    X(FTOI, AB)                                                                                     \
    X(ADDI, ABS8)                                                                                  \
    X(SUBI, ABS8)                                                                                  \
    X(LTI, ASBX)                                                                                   \
    X(LEI, ASBX)                                                                                   \
    X(GTI, ASBX)                                                                                   \
    X(GEI, ASBX)                                                                                   \
    X(EQI, ASBX)                                                                                   \
    X(MULI, ABS8)                                                                                  \
    X(REMI, ABS8)                                                                                  \
    X(FLEK, ABK8)                                                                                  \
    X(LTU, AB)                                                                                     \
    X(LEU, AB)                                                                                     \
    X(MODULE_BEGIN, ABX)                                                                           \
    X(MODULE_READY, ABX)                                                                           \
    X(STATE_CONSTRUCT, ABX)                                                                        \
    X(STATE_PUBLISH, A)                                                                            \
    X(STATE_INITIALIZE, ABX)                                                                       \
    X(STATE_REFERENCE, ABX)                                                                        \
    X(STATE_READ, ABK8)                                                                            \
    X(STATE_WRITE, ABK8)                                                                            \
    X(AGGREGATE_ACCESS, ABX)                                                                        \
    X(VALUE_ACCESS, ABX)                                                                           \
    X(MAP_READ, ABX) \
    X(STRING_LITERAL_BORROW, ABX) \
    X(STRING_LITERAL_MAP_READ, ABX) \
    X(MAP_EDIT, ABX) \
    X(OWNER_DROP, ABX) \
    X(OWNER_COPY, ABX) \
    X(OWNER_EDGE, ABX) \
    X(OWNER_ARRAY_GET, ABX) \
    X(OWNER_ARRAY_LEN, ABX) \
    X(OWNER_CELL_READ, ABX) \
    X(OWNER_STRING_EQUAL, ABX) \
    X(OWNER_STRING_LENGTH, ABX) \
    X(OWNER_BINDING_MOVE, ABX) \
    X(OWNER_ARRAY_UPDATE, ABX) \
    X(OWNER_CALL, ABX) \
    X(OWNER_RETURN, ABX) \
    X(ARITHMETIC_PANIC, ABX) \
    X(ZEXT, ABS8) \
    X(SEXT, ABS8) \
    X(SPILL_ARG, ABX)

typedef enum {
#define XR_RBC_ENUM(name, format) XR_RBC_##name,
    XR_RBC_OPS(XR_RBC_ENUM)
#undef XR_RBC_ENUM
    XR_RBC_OP_COUNT
} XrRbcOp;

static inline bool xr_rbc_op_is_exit(uint32_t op) {
    return op == XR_RBC_RET || op == XR_RBC_RET0 || op == XR_RBC_ARITHMETIC_PANIC;
}

static inline bool xr_rbc_arithmetic_panic_code(uint64_t code) {
    return code == 420 || code == 421;
}

/* A comparison owns the branch pair that follows it, so every pass that walks
 * bytecode must agree on the set. Signed and unsigned ordered compares are
 * distinct opcodes because the register file holds raw bits: the same bit
 * pattern is a different ordering depending on the source type. An unsigned
 * compare always reads two registers; the encoded-bound forms stay signed
 * because the immediate field is a sign-extended 16-bit value. */
static inline bool xr_rbc_op_is_comparison(uint32_t op) {
    return op == XR_RBC_LT || op == XR_RBC_LE || op == XR_RBC_EQ ||
        op == XR_RBC_LTU || op == XR_RBC_LEU ||
        (op >= XR_RBC_LTI && op <= XR_RBC_EQI);
}

/* The immediate comparisons read one register and one encoded bound; the
 * register comparisons read two registers. Dataflow needs that distinction. */
static inline bool xr_rbc_op_is_immediate_comparison(uint32_t op) {
    return op >= XR_RBC_LTI && op <= XR_RBC_EQI;
}

typedef struct {
    XrRbcOp op;
    uint32_t a, b, c;
    int64_t immediate;
} XrRbcOperands;

typedef enum XrRbcRegisterKind {
    XR_RBC_BITS, XR_RBC_STRING, XR_RBC_STRING_CELL, XR_RBC_CALL_WINDOW,
    XR_RBC_STATE, XR_RBC_STATE_REF,
    XR_RBC_ARRAY_I64, XR_RBC_ARRAY_STRING, XR_RBC_ARRAY_I64_CELL, XR_RBC_ARRAY_STRING_CELL,
    XR_RBC_ARRAY_U8, XR_RBC_ARRAY_U64, XR_RBC_ARRAY_U8_CELL, XR_RBC_ARRAY_U64_CELL,
    XR_RBC_ARRAY_I64_REF, XR_RBC_ARRAY_STRING_REF, XR_RBC_ARRAY_U8_REF, XR_RBC_ARRAY_U64_REF,
    XR_RBC_VIEW_FRAME, XR_RBC_VIEW_CELL, XR_RBC_SLICE_I64, XR_RBC_STRING_REF, XR_RBC_FUNCTION,
    XR_RBC_I64_BINDING, XR_RBC_CAPTURED_CELL, XR_RBC_CAPTURED_CELL_BORROW,
    XR_RBC_MAP_STRING_I64, XR_RBC_MAP_STRING_I64_CELL, XR_RBC_MAP_STRING_I64_REF,
    XR_RBC_SET_I64, XR_RBC_BOXED, XR_RBC_ARRAY_F64, XR_RBC_ARRAY_F64_CELL, XR_RBC_ARRAY_F64_REF
} XrRbcRegisterKind;

typedef enum XrRbcArrayOperation {
    XR_RBC_ARRAY_NEW, XR_RBC_ARRAY_LEN, XR_RBC_ARRAY_GET, XR_RBC_ARRAY_SET, XR_RBC_ARRAY_PUSH,
    XR_RBC_ARRAY_RETAIN, XR_RBC_ARRAY_DROP, XR_RBC_ARRAY_TAKE,
    XR_RBC_ARRAY_CELL_NEW, XR_RBC_ARRAY_CELL_READ, XR_RBC_ARRAY_CELL_WRITE, XR_RBC_ARRAY_CELL_DROP,
    XR_RBC_ARRAY_REPEAT, XR_RBC_ARRAY_BORROW, XR_RBC_ARRAY_REF_READ, XR_RBC_ARRAY_REF_WRITE,
    XR_RBC_ARRAY_REF_SET, XR_RBC_ARRAY_REF_PUSH, XR_RBC_ARRAY_DISPLAY,
    XR_RBC_MAP_CONTAINS, XR_RBC_MAP_REMOVE, XR_RBC_MAP_CLEAR,
    XR_RBC_MAP_REF_REMOVE, XR_RBC_MAP_REF_CLEAR,
    XR_RBC_MAP_STATE_READ, XR_RBC_MAP_STATE_WRITE, XR_RBC_MAP_INDEX
} XrRbcArrayOperation;

/* A location names one activation-owned bank. Variadic operands remain real
 * locations, so a constructor need not fit all payloads in a call window. */
typedef struct XrRbcArrayLocation { uint32_t index; bool spill; } XrRbcArrayLocation;

typedef struct XrRbcArrayAccess {
    XrRbcArrayOperation operation;
    XrRbcRegisterKind kind;
    XrRbcArrayLocation result;
    uint32_t first, count, cell;
} XrRbcArrayAccess;

// Slot metadata supplies the immutable payload type and mutability. A compact
// instruction indexes this row without narrowing the slot's numeric identity.
typedef struct XrRbcStateAccess {
    uint32_t slot, state, value;
} XrRbcStateAccess;

typedef struct XrRbcAggregateValue { uint32_t type, offset; } XrRbcAggregateValue;
typedef enum XrRbcAggregateOperation {
    XR_RBC_AGGREGATE_NEW, XR_RBC_AGGREGATE_GET, XR_RBC_AGGREGATE_COPY,
    XR_RBC_NULLABLE_NONE, XR_RBC_NULLABLE_SOME, XR_RBC_NULLABLE_IS_SOME, XR_RBC_NULLABLE_UNWRAP,
    XR_RBC_AGGREGATE_STATE_READ, XR_RBC_AGGREGATE_STATE_WRITE,
    XR_RBC_AGGREGATE_STATE_CONSTRUCT, XR_RBC_AGGREGATE_STATE_INITIALIZE,
    XR_RBC_AGGREGATE_DISPLAY, XR_RBC_STRING_LIBRARY, XR_RBC_RANGE_CHECK, XR_RBC_AGGREGATE_CALL, XR_RBC_AGGREGATE_RETURN,
    XR_RBC_FUNCTION_REFERENCE, XR_RBC_FUNCTION_CALL, XR_RBC_FUNCTION_RETURN,
    XR_RBC_FUNCTION_OWNED_REFERENCE, XR_RBC_FUNCTION_SCOPE_RETURN,
    XR_RBC_FUNCTION_SCOPE_ENTER, XR_RBC_FUNCTION_SCOPE_LEAVE, XR_RBC_FUNCTION_BIND,
    XR_RBC_FUNCTION_STATE_READ, XR_RBC_FUNCTION_STATE_WRITE,
    XR_RBC_FUNCTION_STATE_CONSTRUCT, XR_RBC_FUNCTION_STATE_INITIALIZE,
    XR_RBC_CAPTURED_CELL_NEW, XR_RBC_CAPTURED_CELL_READ, XR_RBC_CAPTURED_CELL_WRITE, XR_RBC_CAPTURED_CELL_BIND,
    XR_RBC_BOXED_NEW, XR_RBC_BOXED_RETAIN, XR_RBC_BOXED_DROP,
    XR_RBC_BOXED_FIELD, XR_RBC_BOXED_PRESENT, XR_RBC_BOXED_TAKE,
    XR_RBC_BOXED_STATE_READ, XR_RBC_BOXED_STATE_WRITE,
    XR_RBC_BOXED_STATE_CONSTRUCT, XR_RBC_BOXED_STATE_INITIALIZE
} XrRbcAggregateOperation;
typedef struct XrRbcAggregateAccess {
    XrRbcAggregateOperation operation;
    uint32_t destination, source;
    XrRbcArrayLocation result;
    uint32_t first, count, slot, callee, window;
} XrRbcAggregateAccess;

typedef struct XrRbcEnvironment {
    uint32_t callee, offset, producer;
} XrRbcEnvironment;
typedef struct XrRbcFunctionScopes {
    const uint32_t *parents;
    uint32_t count;
} XrRbcFunctionScopes;

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
    // Spills are activation-owned indexed storage. The managed prefix owns
    // references; the remaining slots contain scalar bits or capabilities.
    uint32_t spill_count, reference_spill_count;
    /* Scalar parameters beyond the register window. A caller writes them
     * with SPILL_ARG and the host passes an entry's in its argument array; on
     * entry they occupy the first scalar spill slots, starting at
     * reference_spill_count. Nonzero only for plain functions with exactly
     * XR_RBC_REGISTER_PARAMETER_MAX register parameters. */
    uint32_t spill_parameter_count;
    const XrRbcStateAccess *state_accesses;
    size_t state_access_count;
    /* NULL retains the String-only reference prefix. Otherwise every prefix
     * slot has an immutable exact String or closed Array kind. */
    const XrRbcRegisterKind *reference_spill_kinds;
    size_t reference_spill_kind_count;
    const XrRbcArrayAccess *array_accesses;
    size_t array_access_count;
    const XrRbcArrayLocation *array_operands;
    size_t array_operand_count;
    // Each ordered, nonoverlapping home belongs to this activation. Small
    // values use scalar lanes; larger values use compact byte storage.
    const XrRbcAggregateValue *aggregate_values;
    size_t aggregate_value_count, aggregate_size;
    const XrRbcAggregateAccess *aggregate_accesses;
    size_t aggregate_access_count;
    const XrRbcArrayLocation *aggregate_operands;
    size_t aggregate_operand_count;
    // Aggregate formals name ordered independent homes. A nonzero result type is
    // its owned type ordinal plus one. Scalar arguments retain the usual ABI.
    const uint32_t *aggregate_parameters;
    size_t aggregate_parameter_count;
    uint32_t aggregate_result_type;
    const uint32_t *aggregate_arguments;
    size_t aggregate_argument_count;
    /* Signature ordinals plus one, zero for every non-function register.
     * These rows authenticate transports independently of untagged bits. */
    const uint32_t *register_callables;
    size_t register_callable_count;
    uint32_t callable;
    /* Captures precede invocation parameters. A captured State remains the
     * trailing physical parameter. Environment homes are immutable after their
     * sole construction and retire with the owning activation's spill bank. */
    const struct XrRbcCallableValue *captures;
    uint32_t capture_count;
    bool state_capture;
    /* The sealed code owner authenticates compiler-generated cleanup bodies. */
    bool cleanup_only;
    const XrRbcEnvironment *environments;
    uint32_t environment_count, environment_size;
} XrRbcFunction;

// The native set is closed. Dispatch calls each typed function directly;
// no native function pointer or pointer into a movable frame is stored.
typedef enum XrRbcNativeKind {
    XR_RBC_NATIVE_PRINT_I64,
    XR_RBC_NATIVE_CLOCK_REALTIME,
    XR_RBC_NATIVE_CLOCK_CPU,
    XR_RBC_NATIVE_CLOCK_MONOTONIC,
    XR_RBC_NATIVE_UTC_OFFSET_AT,
    XR_RBC_NATIVE_MATH_LERP,
#define XR_MATH_RBC_NATIVE(tag, name, arity) XR_RBC_NATIVE_MATH_##tag,
    XR_MATH_HOST_OPERATIONS(XR_MATH_RBC_NATIVE)
#undef XR_MATH_RBC_NATIVE
    XR_RBC_NATIVE_MATH_RANDOM,
    XR_RBC_NATIVE_MATH_RANDOM_INT,
    XR_RBC_NATIVE_COUNT
} XrRbcNativeKind;

typedef struct {
    const char *bytes;
    size_t length;
} XrRbcLiteral;

#include "xray_native_pod.h"

typedef enum XrRbcCallableValueKind {
    XR_RBC_CALLABLE_SCALAR, XR_RBC_CALLABLE_REFERENCE,
    XR_RBC_CALLABLE_AGGREGATE, XR_RBC_CALLABLE_FUNCTION, XR_RBC_CALLABLE_BINDING, XR_RBC_CALLABLE_CELL,
    XR_RBC_CALLABLE_HOME
} XrRbcCallableValueKind;
/* Hidden CELL captures and typed HOME signatures name exact ValueImage rows.
 * HOME is used for inline-element Arrays with ordinary typed call homes. */
typedef struct XrRbcCallableValue { XrRbcCallableValueKind kind; uint32_t type; } XrRbcCallableValue;
typedef struct XrRbcCallableParameter {
    XrRbcCallableValue value;
    uint32_t mode;
    bool escaping;
} XrRbcCallableParameter;
typedef struct XrRbcCallableType {
    const XrRbcCallableParameter *parameters;
    uint32_t parameter_count;
    XrRbcCallableValue result;
    uint32_t flags;
} XrRbcCallableType;
typedef struct XrRbcFunctionCallableParameter {
    uint32_t parameter;
    XrRbcCallableParameter contract;
} XrRbcFunctionCallableParameter;
typedef struct XrRbcFunctionCallableParameters {
    const XrRbcFunctionCallableParameter *rows;
    uint32_t count;
} XrRbcFunctionCallableParameters;

static inline XrPodType xr_rbc_fixed_type(uint32_t count, uint32_t element) {
    XrPodLayout scalar;
    uint32_t width = element < XR_POD_SCALAR_COUNT &&
        xr_pod_scalar_layout((XrPodScalar)element, &scalar) ? scalar.size : 0;
    return (XrPodType){XR_POD_FIXED_INT, count, element,
        {count * width, width ? width : 1, count}};
}

typedef struct XrRbcCodeLease {
    void *owner;
    bool (*retain)(void *);
    void (*release)(void *);
} XrRbcCodeLease;

struct XrRbcValueCode;
struct XrRbcCallableEntry;
struct XrtMapLiteral;
typedef struct {
    const XrRbcFunction *functions;
    size_t function_count;
    const XrRbcNativeKind *natives;
    size_t native_count;
    // Immutable bytes share the same borrowed lifetime as code/descriptors.
    const XrRbcLiteral *literals;
    size_t literal_count;
    // Raw physical descriptions share the code lifetime. Preparation receives
    // independent billed plans and storage; code never owns a runtime Layout.
    const XrtModuleLayoutSpec *state_spec;
    uint32_t state_entry;
    // Physical aggregate shapes have module-local ordinals. Programs own this
    // projection; externally supplied modules borrow it with their code.
    const XrPodType *aggregate_types;
    size_t aggregate_type_count;
    const XrRbcCallableType *callable_types;
    size_t callable_type_count;
    // An authentic code owner extends code bytes and immutable descriptors,
    // including raw state_spec. An empty lease preserves borrowed code-buffer lifetimes.
    XrRbcCodeLease code_lease;
    const uint32_t *callable_results;
    size_t callable_result_count;
    // A scope cap projects into this owned directory, outside scalar registers.
    // Parent zero names the activation; other ordinals are one-based here.
    const XrRbcFunctionScopes *function_scopes;
    size_t function_scope_count;
    const XrRbcFunctionCallableParameters *function_callable_parameters;
    size_t function_callable_parameter_count;
    /* Explicit immutable typed descriptions share the raw caller's code
     * lifetime, optionally extended by its lease. Admission accepts only proven
     * Frame/borrow transport; these pointers never authorize an owner slot. */
    const struct XrRbcValueCode *const *borrowed_value_codes;
    size_t borrowed_value_code_count;
    const struct XrtValueImage *borrowed_value_image;
    const struct XrRbcCallableEntry *borrowed_callable_entries;
    const uint32_t *borrowed_type_callables;
    /* Immutable provenance shares the authentic code owner's lifetime. */
    const struct XrtMapLiteral *map_literals;
    size_t map_literal_count;
} XrRbcModule;

typedef enum {
    XR_RBC_OK,
    XR_RBC_INVALID,
    XR_RBC_OUT_OF_MEMORY,
    XR_RBC_TYPED_PANIC,
    XR_RBC_NULL_UNWRAP,
    XR_RBC_OUTPUT_FAILURE,
    XR_RBC_HOST_CLOCK_FAILURE,
    XR_RBC_VALUE_FAILURE,
    XR_RBC_MESSAGE_PANIC,
    XR_RBC_TRY_ASSERT,
    XR_RBC_UNCAUGHT_ERROR,
    XR_RBC_TEMP_FAILURE
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
// immutable until execution_free. A supplied code lease is retained during
// preparation and released by execution_free; otherwise they are borrowed.
// Mutating any of these descriptors violates this
// trusted in-process API. Runtime argument values, arithmetic failures and
// stack allocation failures remain execution-time concerns. Output is empty on entry;
// failure never publishes one.
// CALL A,Bx returns into A; the callee starts at A+1, the caller's top argument
// window. CALLN selects a fixed native kind with Bx. Integer output reads
// value/id at A+1/A+2; clocks read id at A+1 and return I64 bits into A.
// UTC offset reads timestamp/id at A+1/A+2 and also returns I64 bits into A.
// Callee locals may overwrite slots beyond the call window. LT/LE/EQ compare
// A,B as signed 64-bit values, LTU/LEU compare the same bits as unsigned, and
// each executes the following JMP only when the comparison is true.
// FLEK writes A with the F64 comparison B <= constants[C]. Its eight-bit C
// field is a constant index, including 255, rather than a register operand.
// PRINT_LITERAL reserves A as zero and selects immutable bytes by Bx; its
// location is the instruction's source map entry, never a scalar register.
// Registers hold 64-bit lanes, so a narrow integer result is computed at full
// width and then normalized: ZEXT keeps the low C bits of B and clears the
// rest, SEXT sign-extends them. C is the lane width, exactly 8, 16 or 32.
// SPILL_ARG A,Bx copies A into outgoing argument Bx, just above the caller's
// spill bank. A run of them, in order from zero, immediately precedes the CALL
// whose callee has that many spill parameters; only MOV, LOADK, LOADI and
// RELOAD may interleave. CALL then moves them to the callee's scalar spill slots.
XR_FUNC XrRbcStatus xr_rbc_module_prepare(const XrRbcModule *module, uint32_t entry,
    const uint64_t *arguments, size_t argument_count, XrtLocationOwner *locations,
    XrRbcExecution **output);
/* Internal receiving entry for a real Owned Fn outcome. Its Env supplies the
 * exact RBC code owner, raw snapshot and original locations. Arguments are
 * ordinary scalar bits; preparation holds the Env independently of the lender.
 * Failure preserves the empty output and invokes no guest code. */
XR_FUNC XrRbcStatus xr_rbc_owned_function_prepare(const XrtFunction *, const uint64_t *,
    size_t argument_count, XrRbcExecution **empty);
// A valid single-use run transfers its already allocated non-NULL outcome,
// including ordinary VM failure. NULL/consumed misuse invokes no guest.
XR_FUNC XrRbcOutcome *xr_rbc_execution_run(XrRbcExecution *execution);
/* Borrow the host's control only until this single-use owned execution returns. */
XR_FUNC XrRbcOutcome *xr_rbc_execution_run_controlled(XrRbcExecution *, const XrtHostExecutionControl *);
/* A receiving caller accepts responsibility for a temporary-path owner and
 * must take it before discarding a failure outcome. */
XR_FUNC XrRbcOutcome *xr_rbc_execution_run_receiving_temp(XrRbcExecution *);
XR_FUNC void xr_rbc_execution_enable_try_trace(XrRbcExecution *);
/* Success consumes *execution. A failed native close preserves the retired
 * execution for another free attempt and cannot restart the guest. */
XR_FUNC XrOsIoStatus xr_rbc_execution_free(XrRbcExecution **execution);
XR_FUNC XrRbcStatus xr_rbc_outcome_status(const XrRbcOutcome *outcome);
// Query handles must be live. False preserves every output; true copies bits
// or scalar facts and borrows locations only through the outcome's lifetime.
// Failure outcomes keep their owner independently of the executable. Retain
// that owner explicitly before outcome_free to keep it longer.
XR_FUNC bool xr_rbc_outcome_bits(const XrRbcOutcome *outcome, uint64_t *output);
// The outcome owns both the exact shape and compact bytes after all execution
// and program owners retire. The borrowed byte span ends with the outcome.
XR_FUNC bool xr_rbc_outcome_aggregate(const XrRbcOutcome *outcome,
    XrPodType *type, const uint8_t **bytes);
XR_FUNC bool xr_rbc_outcome_aggregate_type(const XrRbcOutcome *,
    uint32_t ordinal, XrPodType *type);
/* Natural owner-free Tuple bytes and their one-based canonical image type
 * remain valid after execution and program release, until outcome_free. */
XR_FUNC bool xr_rbc_outcome_inline(const XrRbcOutcome *, const XrtValueImage **,
    uint32_t *type, const uint8_t **bytes);
XR_FUNC bool xr_rbc_outcome_string(const XrRbcOutcome *outcome, const XrtManagedHeader **borrowed);
XR_FUNC bool xr_rbc_outcome_take_string(XrRbcOutcome *outcome, XrtManagedHeader **output);
XR_FUNC bool xr_rbc_outcome_array(const XrRbcOutcome *, const XrtManagedHeader **borrowed);
XR_FUNC bool xr_rbc_outcome_take_array(XrRbcOutcome *, XrtManagedHeader **empty);
XR_FUNC bool xr_rbc_outcome_map(const XrRbcOutcome *, const XrtManagedHeader **borrowed);
XR_FUNC bool xr_rbc_outcome_take_map(XrRbcOutcome *, XrtManagedHeader **empty);
XR_FUNC bool xr_rbc_outcome_function(const XrRbcOutcome *, const XrtFunction **borrowed);
XR_FUNC bool xr_rbc_outcome_take_function(XrRbcOutcome *, XrtFunction *empty);
XR_FUNC bool xr_rbc_outcome_value_failure(const XrRbcOutcome *outcome,
    XrXirValueStatus *status, uint32_t *source_id, const XrtLocationOwner **borrowed_locations);
XR_FUNC uint32_t xr_rbc_outcome_directory_close_source(const XrRbcOutcome *outcome);
XR_FUNC bool xr_rbc_outcome_output_failure(const XrRbcOutcome *outcome,
    XrtOutputFailure *facts, const XrtLocationOwner **borrowed_locations);
XR_FUNC bool xr_rbc_outcome_arithmetic_panic(const XrRbcOutcome *outcome,
    XrtArithmeticPanic *facts, const XrtLocationOwner **borrowed_locations);
XR_FUNC bool xr_rbc_outcome_message_panic(const XrRbcOutcome *, uint32_t *code,
    const char **bytes, size_t *length, uint32_t *source, const XrtLocationOwner **);
XR_FUNC bool xr_rbc_outcome_null_unwrap(const XrRbcOutcome *outcome,
    uint32_t *source_id, const XrtLocationOwner **borrowed_locations);
XR_FUNC bool xr_rbc_outcome_host_clock_failure(const XrRbcOutcome *outcome,
    XrtHostClockFailure *facts, const XrtLocationOwner **borrowed_locations);
XR_FUNC bool xr_rbc_outcome_take_temp(XrRbcOutcome *, XrtTempFailure *empty);
/* Borrow the original source through the live outcome, even after take. */
XR_FUNC const XrtSourceLocation *xr_rbc_outcome_temp_location(const XrRbcOutcome *);
/* An unclaimed temporary-path owner leaves the entire outcome intact. */
XR_FUNC bool xr_rbc_outcome_free(XrRbcOutcome **outcome);
/* The failure owns its error payload and source through outcome destruction. */
XR_FUNC const XrtTryAssertFailure *xr_rbc_outcome_try_assert(const XrRbcOutcome *);
XR_FUNC const XrtErrorSnapshot *xr_rbc_outcome_uncaught_error(const XrRbcOutcome *);
XR_FUNC size_t xr_rbc_outcome_try_trace_count(const XrRbcOutcome *);
XR_FUNC const XrtSourceLocation *xr_rbc_outcome_try_trace_at(const XrRbcOutcome *,size_t);
XR_FUNC const char *xr_rbc_dispatch_name(void);

#endif  // XRBC_H
