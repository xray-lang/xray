/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_print_plan.h - Target-neutral plan for one grouped output call
 *
 * CoreIntrinsicRegistry owns public binding identity.  This schema is a typed
 * projection of the output row; it never resolves source spellings or chooses
 * a second semantic operation.
 *
 * One source call is one plan.  The separator and the terminator are facts of
 * the group, not per-operand flags: an executor derives where a separator goes
 * from the operand ordinal and the arity, so no operand carries its own copy
 * of a decision the group already made.  Arity zero is a complete group and
 * still writes the terminator.
 */

#ifndef XR_PRINT_PLAN_H
#define XR_PRINT_PLAN_H

#include "../base/xlocation.h"
#include "xr_core_intrinsic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_PRINT_PLAN_SCHEMA_VERSION UINT32_C(1)

/* The byte a group writes between two rendered operands. */
typedef enum XrPrintSeparator {
    XR_PRINT_SEPARATOR_NONE = 0,
    XR_PRINT_SEPARATOR_SPACE,
    XR_PRINT_SEPARATOR_COUNT,
} XrPrintSeparator;

/* The byte a group writes after its last rendered operand. */
typedef enum XrPrintTerminator {
    XR_PRINT_TERMINATOR_NONE = 0,
    XR_PRINT_TERMINATOR_NEWLINE,
    XR_PRINT_TERMINATOR_COUNT,
} XrPrintTerminator;

typedef enum XrPrintCapability {
    XR_PRINT_CAPABILITY_NONE = 0,
    XR_PRINT_CAPABILITY_OUTPUT_WRITE = 1u << 0,
    XR_PRINT_CAPABILITY_ALL = XR_PRINT_CAPABILITY_OUTPUT_WRITE,
} XrPrintCapability;

/* ATOMIC_GROUP states that the group is indivisible: an executor renders every
 * operand into one buffer and reaches the output capability exactly once, at
 * the end.  A formatter that fails midway therefore publishes nothing, and two
 * concurrent groups cannot interleave within a line.  The buffer has that one
 * exit: dying any other way — a panic unwinding past it, a coroutine collected
 * while suspended inside a formatter — discards it unwritten. */
typedef enum XrPrintPlanFlag {
    XR_PRINT_PLAN_FLAG_NONE = 0,
    XR_PRINT_PLAN_FLAG_ATOMIC_GROUP = 1u << 0,
} XrPrintPlanFlag;

typedef enum XrPrintPlanStatus {
    XR_PRINT_PLAN_OK = 0,
    XR_PRINT_PLAN_INVALID_ARGUMENT,
    XR_PRINT_PLAN_NOT_OUTPUT,
    XR_PRINT_PLAN_INVALID_ARITY,
    XR_PRINT_PLAN_UNSUPPORTED_TARGET,
    XR_PRINT_PLAN_MISSING_CAPABILITY,
    XR_PRINT_PLAN_INVALID_SCHEMA,
} XrPrintPlanStatus;

typedef struct XrPrintPlan {
    uint32_t schema_version;
    XrCoreBuiltinId builtin_id;
    XrLocation source;
    XrPrintSeparator separator;
    XrPrintTerminator terminator;
    XrCoreIntrinsicEffectKind effect;
    uint32_t target;
    uint32_t required_capabilities;
    uint32_t flags;
    uint16_t arity;
} XrPrintPlan;

XR_FUNC XrPrintPlanStatus xr_print_plan_build(XrCoreBuiltinId builtin_id, uint16_t arity,
                                              XrLocation source, uint32_t target,
                                              uint32_t available_capabilities, XrPrintPlan *out);
XR_FUNC bool xr_print_plan_validate(const XrPrintPlan *plan);

/* A separator precedes every operand except the first.  Deriving it here keeps
 * one answer for every executor instead of a bit set per emitted operand. */
static inline bool xr_print_plan_operand_needs_separator(const XrPrintPlan *plan,
                                                         uint16_t ordinal) {
    return plan && plan->separator != XR_PRINT_SEPARATOR_NONE && ordinal > 0 &&
           ordinal < plan->arity;
}

static inline char xr_print_separator_byte(XrPrintSeparator separator) {
    return separator == XR_PRINT_SEPARATOR_SPACE ? ' ' : '\0';
}

static inline char xr_print_terminator_byte(XrPrintTerminator terminator) {
    return terminator == XR_PRINT_TERMINATOR_NEWLINE ? '\n' : '\0';
}

/* Exact byte count a group adds around its rendered operands: one separator
 * between adjacent operands plus one terminator.  Executors size their render
 * buffer with this, so a plan and its output cannot disagree on framing. */
static inline size_t xr_print_plan_framing_bytes(const XrPrintPlan *plan) {
    if (!plan)
        return 0;
    size_t separators = plan->separator != XR_PRINT_SEPARATOR_NONE && plan->arity > 1
                            ? (size_t) (plan->arity - 1u)
                            : 0u;
    size_t terminator = plan->terminator != XR_PRINT_TERMINATOR_NONE ? 1u : 0u;
    return separators + terminator;
}

/* Bytecode projection of one group.
 *
 * A group becomes several instructions because rendering an operand can call
 * back into the interpreter. These bits carry the group facts that each
 * instruction needs, derived from the plan at emission time; nothing upstream
 * of emission encodes them, so the plan stays the only place the group is
 * described.
 *
 * Every bit below means one thing across the whole projection, whichever
 * instruction and whichever operand slot carries it.  Separate and terminate
 * never share an instruction, so they could have shared a bit; they do not,
 * because then reading a flag word would first require knowing which
 * instruction it came from, and the operand field is 16 bits wide with room to
 * spare. */
#define XR_PRINT_BC_SEPARATE 0x01u
/* Unlike the group decisions here, this one projects the operand's static type:
 * a full-width unsigned integer shares its slot representation with a signed
 * one, so the renderer cannot recover the sign domain from the value alone. */
#define XR_PRINT_BC_UNSIGNED 0x02u
/* Rides the flush, the single instruction that reaches the output capability. */
#define XR_PRINT_BC_TERMINATE 0x04u

/* Registers PRINT_GROUP_APPEND reserves from its A operand, in order: the group
 * buffer, the return slot of a user `toString`, and that call's frame base.
 * The buffer sits below the callee, which is what keeps it alive across the
 * call; the two slots above it are why the window is wider than one register. */
#define XI_PRINT_GROUP_WINDOW 3u
#define XI_PRINT_GROUP_CALL_SLOT 2u

#endif /* XR_PRINT_PLAN_H */
