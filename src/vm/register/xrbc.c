/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc.c - Scalar register interpreter with movable, indexed frames.
 *
 * KEY CONCEPT:
 *   VM calls stay in one C dispatch loop. Frame and register locations are
 *   indices; no pointer into either growing array survives a call.
 */

#include "xrbc_internal.h"
#include "lir/xlir_string.h"
#include "xray_native_string.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include "runtime/native/xnative_array.h"
#include "runtime/native/xnative_map.h"
#include "runtime/native/xnative_boxed.h"
#include "xray_native_view.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena_internal.h"
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_M_FP_FAST) || defined(__FAST_MATH__) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error HARDWARE_F64_REQUIRES_NONFAST_FLOATING_POINT_MODE
#endif
XR_STATIC_ASSERT(CHAR_BIT == 8 && sizeof(double) == 8 && sizeof(uint64_t) == 8 && FLT_RADIX == 2 &&
    DBL_MANT_DIG == 53 && DBL_MIN_EXP == -1021 && DBL_MAX_EXP == 1024,
    "The register interpreter requires binary64 double");
static double xr_rbc_float(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint64_t xr_rbc_float_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

typedef union {
    uint64_t bits;
    XrtString *string;
    XrtModuleState *state;
    XrtModuleAccess *state_ref;
    XrtArray *array;
    XrtMap *map;
} XrRbcRegister;
XR_STATIC_ASSERT(sizeof(XrRbcRegister) == 8, "physical register lanes occupy eight bytes");

#include "xrbc_entry.inc.c"

typedef struct XrRbcClosureScope { size_t depth, first; uint32_t id; } XrRbcClosureScope;
typedef struct XrRbcClosureOwner {
    size_t next;
    uint32_t generation, binding;
} XrRbcClosureOwner;

typedef struct {
    size_t function, pc, base;
    uint32_t destination;
    size_t cell_base;
    uint32_t initialized_cells;
    size_t spill_base;
    size_t aggregate_base;
} XrRbcFrame;

typedef struct {
    XrRbcRegister *slots;
    size_t slot_capacity;
    XrRbcFrame *frames;
    size_t frame_capacity, depth;
    XrRbcRegister *cells;
    size_t cell_capacity, cell_count;
    XrRbcRegister *spills;
    size_t spill_capacity, spill_count;
    XrtModuleAccess *module_accesses;
    void *array_arguments;
    size_t array_argument_capacity;
    // Borrowed from the execution and its pending outcome for one synchronous
    // run. Neither pointer owns another stack or survives execution retirement.
    const XrRbcEntry *entries;
    uint8_t *aggregate_result;
    XrtCallableCode *closure_code;
    XrXirDomain *closure_domain;
    XrXirValue *closures, *closure_arguments;
    size_t closure_count, closure_capacity, closure_argument_capacity;
    XrRbcClosureOwner *closure_owners;
    size_t closure_owner_capacity, closure_free;
    XrRbcClosureScope *closure_scopes;
    size_t closure_scope_count, closure_scope_capacity;
    XrXirValue closure_pending;
} XrRbcStack;

struct XrRbcOutcome {
    XrRbcStatus status;
    XrRbcRegister value;
    XrRbcRegisterKind result_kind;
    XrtExecutionFailure failure;
    XrtLocationOwner *locations;
    XrPodType aggregate_type;
    XrPodType *aggregate_types;
    size_t aggregate_type_count;
    uint8_t *aggregate_bytes;
};

struct XrRbcExecution {
    const XrRbcModule *module;
    XrRbcEntry *entries;
    XrRbcStack stack;
    XrRbcOutcome *pending;
    XrtModuleState *state;
    XrRbcCodeLease code_lease;
    bool consumed;
};

static bool xr_rbc_grow(void **storage, size_t *capacity, size_t count, size_t item_size) {
    if (count <= *capacity)
        return true;
    if (count > SIZE_MAX / item_size)
        return false;
    size_t next = *capacity ? *capacity : 16;
    while (next < count) {
        if (next > SIZE_MAX / 2) {
            next = count;
            break;
        }
        next *= 2;
    }
    if (next > SIZE_MAX / item_size)
        next = count;
    void *grown = xr_realloc(*storage, next * item_size);
    if (!grown)
        return false;
    *storage = grown;
    *capacity = next;
    return true;
}

static bool xr_rbc_reserve(XrRbcStack *stack, size_t slots, size_t frames, size_t cells,
    size_t spills) {
    // Use real void* objects, not a type-punned pointer-to-pointer.
    void *storage = stack->slots;
    if (!xr_rbc_grow(&storage, &stack->slot_capacity, slots, sizeof(*stack->slots)))
        return false;
    stack->slots = storage;
    storage = stack->frames;
    if (!xr_rbc_grow(&storage, &stack->frame_capacity, frames, sizeof(XrRbcFrame)))
        return false;
    stack->frames = storage;
    storage = stack->cells;
    if (!xr_rbc_grow(&storage, &stack->cell_capacity, cells, sizeof(*stack->cells)))
        return false;
    stack->cells = storage;
    storage = stack->spills;
    if (!xr_rbc_grow(&storage, &stack->spill_capacity, spills, sizeof(*stack->spills)))
        return false;
    stack->spills = storage;
    return true;
}

static XrRbcRegisterKind rbc_register_kind(const XrRbcFunction *function, uint32_t slot) {
    return function->register_kinds ? function->register_kinds[slot] : XR_RBC_BITS;
}

#include "xrbc_array_admission.inc.c"

#include "xrbc_state_admission.inc.c"
#include "xrbc_view_admission.inc.c"
#include "xrbc_callable_admission.inc.c"
#include "xrbc_boxed_admission.inc.c"
#include "xrbc_aggregate_admission.inc.c"

static bool xr_rbc_function_valid(const XrRbcFunction *function) {
    if (!(function->code && function->code_count && function->register_count &&
           function->register_count <= XR_RBC_REGISTER_COUNT_MAX &&
           function->parameter_count <= function->register_count &&
           function->cell_count <= 65536 && function->spill_count <= XR_RBC_SPILL_COUNT_MAX &&
           function->reference_spill_count <= function->spill_count &&
           (!function->constant_count || function->constants) &&
           (function->state_accesses ? function->state_access_count <= 65536 :
            function->state_access_count == 0))) return false;
    if ((function->register_kinds ? function->register_kind_count != function->register_count :
         function->register_kind_count != 0) ||
        (function->cell_kinds ? function->cell_kind_count != function->cell_count :
         function->cell_kind_count != 0) ||
        (function->result_kind != XR_RBC_BITS && function->result_kind != XR_RBC_FUNCTION && function->result_kind != XR_RBC_SLICE_I64 &&
         !rbc_managed_kind(function->result_kind))) return false;
    for (uint32_t i = 0; function->register_kinds && i < function->register_count; ++i)
        if ((unsigned)function->register_kinds[i] > XR_RBC_BOXED ||
            (i < function->parameter_count &&
                (function->register_kinds[i] == XR_RBC_CALL_WINDOW ||
                 function->register_kinds[i] == XR_RBC_STATE_REF || function->register_kinds[i] == XR_RBC_VIEW_FRAME ||
                 (function->register_kinds[i] == XR_RBC_STATE && i + 1 != function->parameter_count)))) return false;
    for (uint32_t i = 0; function->cell_kinds && i < function->cell_count; ++i)
        if (function->cell_kinds[i] != XR_RBC_BITS && !rbc_managed_kind(function->cell_kinds[i])) return false;
    return rbc_array_metadata_valid(function) && rbc_view_metadata_valid(function) && rbc_view_function_valid(function);
}

static bool xr_rbc_instruction_valid(const XrRbcModule *module,
    const XrRbcFunction *function, size_t pc) {
    uint32_t word = function->code[pc], op = word & 255;
    uint32_t a = (word >> 8) & 255, b = (word >> 16) & 255, c = word >> 24;
    uint32_t index = word >> 16, registers = function->register_count;
    size_t remaining = function->code_count - pc;
    if (module->closure_entry_count && (op == XR_RBC_RET || op == XR_RBC_RET0)) return false;
    bool immediate_comparison = op >= XR_RBC_LTI && op <= XR_RBC_EQI;
    bool comparison = immediate_comparison || op == XR_RBC_LT || op == XR_RBC_LE || op == XR_RBC_EQ;
    switch (op) {
    case XR_RBC_AGGREGATE_ACCESS:
        if (a || index >= function->aggregate_access_count) return false;
        if (xr_rbc_aggregate_return_word(function->aggregate_accesses, function->aggregate_access_count, word)) return true;
        break;
    case XR_RBC_VIEW_ACCESS:
        if (a || index >= function->view_access_count) return false;
        break;
    case XR_RBC_ARRAY_ACCESS:
        if (a || index >= function->array_access_count) return false;
        break;
    case XR_RBC_MODULE_BEGIN: case XR_RBC_MODULE_READY:
    case XR_RBC_STATE_CONSTRUCT: case XR_RBC_STATE_PUBLISH:
    case XR_RBC_STATE_INITIALIZE: case XR_RBC_STATE_REFERENCE:
    case XR_RBC_STATE_READ: case XR_RBC_STATE_WRITE:
        if (!xr_rbc_state_instruction_valid(module, function, word)) return false;
        break;
    case XR_RBC_FLEK:
        if (a >= registers || b >= registers || c >= function->constant_count) return false;
        break;
    case XR_RBC_ADDI: case XR_RBC_SUBI: case XR_RBC_MULI: case XR_RBC_REMI:
        if (a >= registers || b >= registers) return false;
        break;
    case XR_RBC_LTI: case XR_RBC_LEI: case XR_RBC_GTI: case XR_RBC_GEI: case XR_RBC_EQI:
        if (a >= registers || remaining <= 2 || (function->code[pc + 1] & 255) != XR_RBC_JMP)
            return false;
        break;
    case XR_RBC_MOV: case XR_RBC_NEG: case XR_RBC_FNEG: case XR_RBC_ITOF: case XR_RBC_FTOI:
    case XR_RBC_CELL_READ: case XR_RBC_CELL_WRITE:
    case XR_RBC_STRING_I64: case XR_RBC_STRING_F64: case XR_RBC_STRING_RETAIN: case XR_RBC_STRING_TAKE:
    case XR_RBC_STRING_LEN:
    case XR_RBC_LT: case XR_RBC_LE: case XR_RBC_EQ:
        if (a >= registers || b >= registers || c) return false;
        if (comparison && (remaining <= 2 || (function->code[pc + 1] & 255) != XR_RBC_JMP))
            return false;
        break;
    case XR_RBC_STRING_CONCAT:
    case XR_RBC_ADD: case XR_RBC_SUB: case XR_RBC_MUL:
    case XR_RBC_FADD: case XR_RBC_FSUB: case XR_RBC_FMUL: case XR_RBC_FDIV:
    case XR_RBC_FEQ: case XR_RBC_FNE: case XR_RBC_FLT: case XR_RBC_FLE: case XR_RBC_FGT: case XR_RBC_FGE:
    case XR_RBC_DIV: case XR_RBC_REM: case XR_RBC_AND: case XR_RBC_OR:
    case XR_RBC_XOR: case XR_RBC_SHL: case XR_RBC_SHR: case XR_RBC_USHR:
        if (a >= registers || b >= registers || c >= registers) return false;
        break;
    case XR_RBC_LOADK:
        if (a >= registers || index >= function->constant_count) return false;
        break;
    case XR_RBC_SPILL: case XR_RBC_RELOAD:
        if (a >= registers || index >= function->spill_count) return false;
        break;
    case XR_RBC_LOADI:
        if (a >= registers) return false;
        break;
    case XR_RBC_PRINT_LITERAL:
        if (a || index >= module->literal_count) return false;
        break;
    case XR_RBC_STRING_LITERAL:
        if (a >= registers || index >= module->literal_count) return false;
        break;
    case XR_RBC_STRING_CELL_NEW:
        if (a >= registers || b >= registers || c >= function->cell_count) return false;
        break;
    case XR_RBC_STRING_DROP: case XR_RBC_STRING_PRINT: case XR_RBC_STRING_CELL_DROP:
        if (a >= registers || (word >> 16)) return false;
        break;
    case XR_RBC_CELL_NEW:
        if (a >= registers || index >= function->cell_count) return false;
        break;
    case XR_RBC_JMP: {
        uint32_t bits = word >> 8;
        int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t) bits : (int32_t) bits - 16777216;
        size_t next = pc + 1;
        if (offset < 0) return (size_t) -offset <= next;
        return (size_t) offset < function->code_count - next;
    }
    case XR_RBC_CALL:
        if (a >= registers || index >= module->function_count ||
            module->functions[index].callable ||
            module->functions[index].aggregate_parameter_count || module->functions[index].aggregate_result_type ||
            a + 1 + module->functions[index].parameter_count != registers) return false;
        break;
    case XR_RBC_CALLN:
        if (index >= module->native_count ||
            a + (module->natives[index] == XR_RBC_NATIVE_PRINT_I64 ||
                module->natives[index] == XR_RBC_NATIVE_UTC_OFFSET_AT ? 2u : 1u) >= registers)
            return false;
        break;
    case XR_RBC_RET:
        return !function->callable && !function->aggregate_result_type && a < registers && !(word >> 16) &&
            (function->result_kind == XR_RBC_BITS || function->result_kind == XR_RBC_SLICE_I64 ||
                function->result_kind == XR_RBC_BOXED || rbc_direct_managed_kind(function->result_kind));
    case XR_RBC_RET0:
        return !function->callable && !function->aggregate_result_type && word == XR_RBC_RET0;
    default:
        return false;
    }
    // Every possible successor must remain within the validated function,
    // even when the instruction or comparison's false edge is unreachable.
    return remaining > (comparison ? 2u : 1u);
}

#include "xrbc_string_admission.inc.c"

static XrRbcStatus xr_rbc_module_valid(const XrRbcModule *module) {
    XrRbcCodeLease lease = module->code_lease;
    if (lease.owner ? !lease.retain || !lease.release : lease.retain || lease.release)
        return XR_RBC_INVALID;
    for (size_t t = 0; t < module->aggregate_type_count; ++t) {
        XrRbcAggregateLayout layout;
        if (!xr_rbc_aggregate_type_valid(module, (uint32_t)t) ||
            !xr_rbc_aggregate_layout(module->aggregate_types[t], &layout)) return XR_RBC_INVALID;
    }
    for (size_t i = 0; i < module->literal_count; ++i)
        if (module->literals[i].length == SIZE_MAX ||
            (module->literals[i].length && !module->literals[i].bytes)) return XR_RBC_INVALID;
    // Validate descriptors before using any call target's parameter count.
    for (size_t f = 0; f < module->function_count; ++f)
        if (!xr_rbc_function_valid(&module->functions[f])) return XR_RBC_INVALID;
    if (!rbc_callable_metadata_valid(module)) return XR_RBC_INVALID;
    for (size_t f = 0; f < module->function_count; ++f)
        if (!rbc_boxed_function_valid(module, &module->functions[f]) || !rbc_aggregate_metadata_valid(module, &module->functions[f])) return XR_RBC_INVALID;
    if (!xr_rbc_state_layout_valid(module)) return XR_RBC_INVALID;
    for (size_t f = 0; f < module->function_count; ++f) {
        const XrRbcFunction *function = &module->functions[f];
        if (!rbc_map_module_valid(module, function) || !rbc_aggregate_calls_valid(module, function) || !rbc_view_module_valid(module, function)) return XR_RBC_INVALID;
        for (size_t pc = 0; pc < function->code_count; ++pc)
            if (!xr_rbc_instruction_valid(module, function, pc)) return XR_RBC_INVALID;
        XrRbcStatus status = xr_rbc_strings_valid(module, function);
        if (status != XR_RBC_OK) return status;
    }
    return XR_RBC_OK;
}

static bool xr_rbc_source_maps_valid(const XrRbcModule *module, const XrtLocationOwner *locations) {
    // An unreachable callee must not hide invalid IDs until after guest effects.
    size_t count = xrt_locations_count(locations);
    for (size_t f = 0; f < module->function_count; ++f) {
        const XrRbcFunction *function = &module->functions[f];
        if (!function->source_ids) {
            if (function->source_id_count) return false;
            continue;
        }
        if (!function->source_id_count || function->source_id_count != function->code_count)
            return false;
        for (size_t pc = 0; pc < function->source_id_count; ++pc)
            if (function->source_ids[pc] > count) return false;
    }
    return true;
}

XR_FUNC XrRbcStatus xr_rbc_module_admit(const XrRbcModule *module, const XrtLocationOwner *locations) {
    if (!module || !module->functions || !module->function_count || !locations ||
        (module->native_count && !module->natives) || module->native_count > 65536 ||
        (module->literal_count && !module->literals) || module->literal_count > 65536 ||
        (module->aggregate_type_count && !module->aggregate_types) || module->aggregate_type_count > UINT32_MAX ||
        module->aggregate_type_count > SIZE_MAX / sizeof(*module->aggregate_types))
        return XR_RBC_INVALID;
    XrRbcStatus status = xr_rbc_module_valid(module);
    if (status != XR_RBC_OK) return status;
    if (!xr_rbc_source_maps_valid(module, locations)) return XR_RBC_INVALID;
    for (size_t i = 0; i < module->native_count; ++i)
        if ((unsigned)module->natives[i] >= XR_RBC_NATIVE_COUNT) return XR_RBC_INVALID;
    return XR_RBC_OK;
}

static int64_t xr_rbc_signed(uint64_t bits) {
    // Avoid implementation-defined unsigned-to-signed conversion.
    return bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) ~bits;
}

static uint64_t xr_rbc_shift_right(uint64_t bits, uint64_t amount) {
    unsigned shift = (unsigned) (amount & 63);
    uint64_t result = bits >> shift;
    if (shift && (bits >> 63))
        result |= UINT64_MAX << (64 - shift);
    return result;
}

static bool xr_rbc_call_reserve(XrRbcStack *stack, size_t base, const XrRbcFunction *callee) {
    if (base > SIZE_MAX - callee->register_count ||
        stack->cell_count > SIZE_MAX - callee->cell_count ||
        stack->spill_count > SIZE_MAX - callee->spill_count || stack->depth == SIZE_MAX)
        return false;
    return xr_rbc_reserve(stack, base + callee->register_count, stack->depth + 1,
        stack->cell_count + callee->cell_count, stack->spill_count + callee->spill_count);
}

static XrRbcRegister *xr_rbc_cell(XrRbcStack *stack, const XrRbcModule *module,
    uint64_t handle, XrRbcRegisterKind expected) {
    if (!handle || handle > stack->cell_count) return NULL;
    size_t index = (size_t)handle - 1;
    // Reserved capacity does not establish a cell's lifetime. Array-local
    // slots may leave holes below the active owner's high-water mark; their
    // typed handles additionally require the prepared CFG capability proof.
    for (size_t depth = stack->depth; depth; --depth) {
        const XrRbcFrame *frame = &stack->frames[depth - 1];
        if (index >= frame->cell_base && index - frame->cell_base < frame->initialized_cells) {
            const XrRbcFunction *owner = &module->functions[frame->function];
            XrRbcRegisterKind actual = owner->cell_kinds ? owner->cell_kinds[index - frame->cell_base] : XR_RBC_BITS;
            if (actual != expected) return NULL;
            return &stack->cells[index];
        }
    }
    return NULL;
}

#include "xrbc_state_execute.inc.c"
#include "xrbc_array_execute.inc.c"
#include "xrbc_view_execute.inc.c"
#include "xrbc_aggregate_execute.inc.c"
static bool xr_rbc_captured_cell_value(const XrRbcModule *, const XrRbcStack *,
    XrRbcRegisterKind, uint64_t, XrXirType, XrXirValue *);
#include "xrbc_map_execute.inc.c"
#include "xrbc_aggregate_call.inc.c"

/* Keep computed-goto handlers on a stable cache-line boundary as cold code grows. */
#if defined(__clang__) || defined(__GNUC__)
XR_ALIGN(64)
#endif
static XrRbcStatus xr_rbc_execute(const XrRbcModule *module, XrRbcStack *stack,
    const XrtLocationOwner *locations, XrRbcRegister *result, const XrRbcEntry *entries) {
    const XrRbcFunction *function;
    const uint32_t *code;
    const uint64_t *constants;
    XrRbcFrame *frame;
    XrRbcRegister *registers;
    size_t pc;
    uint32_t instruction, op, a, b, c;
    // Dispatch mechanics follow the existing register interpreter: each
    // handler owns its next fetch on compilers supporting labels as values.
#include "xrbc_dispatch.inc.c"
}

XR_FUNC XrRbcStatus xr_rbc_module_prepare_admitted(const XrRbcModule *module, uint32_t entry,
    const uint64_t *arguments, size_t argument_count, XrtLocationOwner *locations,
    XrRbcExecution **output) {
    if (!output || *output || !locations || !module || !module->functions ||
        entry >= module->function_count || (argument_count && !arguments))
        return XR_RBC_INVALID;
    const XrRbcFunction *function = &module->functions[entry];
    bool inject_state = module->state_layout && entry == module->state_entry;
    if (function->result_kind == XR_RBC_SLICE_I64 || function->result_kind == XR_RBC_FUNCTION || function->result_kind == XR_RBC_BOXED ||
        function->capture_count || function->state_capture ||
        function->aggregate_parameter_count || (inject_state && !function->parameter_count) ||
        argument_count != function->parameter_count - (inject_state ? 1u : 0u) ||
        (!inject_state && function->parameter_count &&
         rbc_register_kind(function, function->parameter_count - 1) == XR_RBC_STATE)) return XR_RBC_INVALID;
    for (size_t p = 0; function->register_kinds && p < argument_count; ++p)
        if (function->register_kinds[p] != XR_RBC_BITS) return XR_RBC_INVALID;
    if (function->callable) {
        XrRbcCallableType type = module->callable_types[function->callable - 1];
        for (size_t p = 0; p < argument_count; ++p)
            if (!xr_rbc_callable_scalar_valid(type.parameters[p].value, arguments[p])) return XR_RBC_INVALID;
    }
    XrRbcExecution *execution = xr_calloc(1, sizeof(*execution));
    if (!execution) return XR_RBC_OUT_OF_MEMORY;
    if (module->code_lease.owner) {
        if (!module->code_lease.retain(module->code_lease.owner)) {
            xr_free(execution);
            return XR_RBC_VALUE_FAILURE;
        }
        execution->code_lease = module->code_lease;
    }
    execution->module = module;
    if (module->closure_entry_count) {
        XrRbcCodeLease lease = module->code_lease;
        XrXirValueStatus status = xrt_callable_code_new(module->closure_types, module->closure_entries,
            (uint32_t)module->closure_entry_count,
            (XrtCallableLease){lease.owner, lease.retain, lease.release}, &execution->stack.closure_code);
        if (status != XR_XIR_VALUE_OK) {
            xr_rbc_execution_free(&execution);
            return status == XR_XIR_VALUE_OOM ? XR_RBC_OUT_OF_MEMORY : XR_RBC_VALUE_FAILURE;
        }
    }
    execution->pending = xr_calloc(1, sizeof(*execution->pending));
    if (module->function_count > SIZE_MAX / sizeof(*execution->entries)) {
        xr_rbc_execution_free(&execution);
        return XR_RBC_OUT_OF_MEMORY;
    }
    execution->entries = xr_malloc(module->function_count * sizeof(*execution->entries));
    if (!execution->pending || !execution->entries || !xr_rbc_entries(module, execution->entries) ||
        !xr_rbc_reserve(&execution->stack, function->register_count, 1, function->cell_count,
            function->spill_count) ||
        !xrt_locations_retain(locations)) {
        xr_rbc_execution_free(&execution);
        return XR_RBC_OUT_OF_MEMORY;
    }
    execution->pending->locations = locations;
    execution->pending->status = XR_RBC_INVALID;
    execution->pending->result_kind = function->result_kind;
    execution->stack.entries = execution->entries;
    if (function->aggregate_result_type) {
        execution->pending->aggregate_type = module->aggregate_types[function->aggregate_result_type - 1];
        execution->pending->aggregate_bytes = xr_malloc(execution->pending->aggregate_type.physical.size);
        execution->pending->aggregate_types = xr_malloc(module->aggregate_type_count * sizeof(*module->aggregate_types));
        if (!execution->pending->aggregate_bytes || !execution->pending->aggregate_types) {
            xr_rbc_execution_free(&execution);
            return XR_RBC_OUT_OF_MEMORY;
        }
        memcpy(execution->pending->aggregate_types, module->aggregate_types,
            module->aggregate_type_count * sizeof(*module->aggregate_types));
        execution->pending->aggregate_type_count = module->aggregate_type_count;
        execution->stack.aggregate_result = execution->pending->aggregate_bytes;
    }
    if (inject_state) {
        XrXirValueStatus status = xrt_module_state_new(module->state_layout, UINT64_MAX, &execution->state);
        if (status == XR_XIR_VALUE_OK)
            status = xr_rbc_state_caches(module->state_layout, execution->state,
                &execution->stack.module_accesses);
        if (status != XR_XIR_VALUE_OK) {
            xr_rbc_execution_free(&execution);
            return status == XR_XIR_VALUE_OOM ? XR_RBC_OUT_OF_MEMORY : XR_RBC_VALUE_FAILURE;
        }
    }
    xr_rbc_entry_clear(execution->stack.slots, &execution->entries[entry]);
    if (argument_count)
        for (size_t i = 0; i < argument_count; ++i) execution->stack.slots[i].bits = arguments[i];
    if (inject_state) execution->stack.slots[argument_count].state = execution->state;
    if (function->reference_spill_count)
        memset(execution->stack.spills, 0, function->reference_spill_count * sizeof(*execution->stack.spills));
    execution->stack.frames[0] = (XrRbcFrame) {entry, 0, 0, 0, 0, 0, 0, 0};
    execution->stack.depth = 1;
    execution->stack.cell_count = function->cell_count;
    execution->stack.spill_count = function->spill_count;
    *output = execution;
    return XR_RBC_OK;
}

XR_FUNC XrRbcStatus xr_rbc_module_prepare(const XrRbcModule *module, uint32_t entry,
    const uint64_t *arguments, size_t argument_count, XrtLocationOwner *locations,
    XrRbcExecution **output) {
    if (!output || *output || !locations || !module || !module->functions ||
        entry >= module->function_count || (argument_count && !arguments))
        return XR_RBC_INVALID;
    XrRbcStatus status = xr_rbc_module_admit(module, locations);
    if (status != XR_RBC_OK) return status;
    return xr_rbc_module_prepare_admitted(module, entry, arguments, argument_count, locations, output);
}

static void xr_rbc_root(void *context) {
    XrRbcExecution *execution = context;
    const XrXirTypeArena *map_types = xrt_callable_map_catalog(execution->module->closure_types);
    if (map_types) {
        XrXirValueStatus status = xrt_map_root_bind_types(map_types);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, 0);
    }
    execution->pending->status = xr_rbc_execute(execution->module, &execution->stack,
        execution->pending->locations, &execution->pending->value, execution->entries);
    if (execution->pending->status == XR_RBC_OK && execution->pending->result_kind == XR_RBC_STRING)
        execution->pending->value.string = xrt_string_root_take(execution->pending->value.string);
    if (execution->pending->status == XR_RBC_OK && execution->pending->result_kind == XR_RBC_MAP_STRING_I64)
        execution->pending->value.map = xrt_map_root_take(execution->pending->value.map);
    if (execution->pending->status == XR_RBC_OK && rbc_array_kind(execution->pending->result_kind))
        execution->pending->value.array = xrt_array_root_take(execution->pending->value.array);
}

static void xr_rbc_stack_free(XrRbcStack *stack) {
    xr_xir_value_drop(&stack->closure_pending);
    while (stack->closure_scope_count)
        xr_rbc_closure_scope_drop(stack, --stack->closure_scope_count);
    for (size_t n = stack->closure_count; n; --n) xr_xir_value_drop(&stack->closures[n - 1]);
    if (stack->closure_domain) {
        XR_CHECK(xr_xir_domain_close(stack->closure_domain) == XR_XIR_VALUE_OK,
            "the actual closure graph owner closes before its final release");
        xr_xir_domain_drop(stack->closure_domain);
    }
    xrt_callable_code_drop(&stack->closure_code);
    xr_free(stack->closures);
    xr_free(stack->closure_owners);
    xr_free(stack->closure_arguments);
    xr_free(stack->closure_scopes);
    xr_free(stack->array_arguments);
    xr_free(stack->spills);
    xr_free(stack->cells);
    xr_free(stack->frames);
    xr_free(stack->slots);
    xr_free(stack->module_accesses);
    *stack = (XrRbcStack) {0};
}

XR_FUNC XrRbcOutcome *xr_rbc_execution_run(XrRbcExecution *execution) {
    if (!execution || execution->consumed) return NULL;
    execution->consumed = true;
    XrRbcOutcome *outcome = execution->pending;
    XrtExecutionStatus status = xrt_run_embedded(outcome->locations, &outcome->failure,
        xr_rbc_root, execution);
    if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_OUTPUT)
        outcome->status = XR_RBC_OUTPUT_FAILURE;
    else if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_TYPED_PANIC)
        outcome->status = XR_RBC_TYPED_PANIC;
    else if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_NULL_UNWRAP)
        outcome->status = XR_RBC_NULL_UNWRAP;
    else if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_HOST_CLOCK)
        outcome->status = XR_RBC_HOST_CLOCK_FAILURE;
    else if (status == XRT_EXECUTION_FAILURE && outcome->failure.kind == XRT_FAILURE_VALUE)
        outcome->status = XR_RBC_VALUE_FAILURE;
    else {
        if (status != XRT_EXECUTION_COMPLETED) outcome->status = XR_RBC_INVALID;
        xrt_locations_release(outcome->locations);
        outcome->locations = NULL;
    }
    // The common boundary has restored its caller before reclaiming the sole
    // heap stack descriptor, including allocations made by skipped VM frames.
    xr_rbc_stack_free(&execution->stack);
    xrt_module_state_drop(&execution->state);
    execution->pending = NULL;
    return outcome;
}

XR_FUNC void xr_rbc_execution_free(XrRbcExecution **execution) {
    if (!execution || !*execution) return;
    XrRbcExecution *owned = *execution;
    *execution = NULL;
    xr_rbc_stack_free(&owned->stack);
    xrt_module_state_drop(&owned->state);
    xr_free(owned->entries);
    xr_rbc_outcome_free(&owned->pending);
    if (owned->code_lease.owner) owned->code_lease.release(owned->code_lease.owner);
    xr_free(owned);
}

XR_FUNC XrRbcStatus xr_rbc_outcome_status(const XrRbcOutcome *outcome) {
    return outcome ? outcome->status : XR_RBC_INVALID;
}

XR_FUNC bool xr_rbc_outcome_bits(const XrRbcOutcome *outcome, uint64_t *output) {
    if (!outcome || outcome->status != XR_RBC_OK || !output || outcome->aggregate_bytes ||
        outcome->result_kind != XR_RBC_BITS) return false;
    *output = outcome->value.bits;
    return true;
}
XR_FUNC bool xr_rbc_outcome_aggregate(const XrRbcOutcome *outcome,
    XrPodType *type, const uint8_t **bytes) {
    if (!outcome || outcome->status != XR_RBC_OK || !outcome->aggregate_bytes || !type || !bytes) return false;
    *type = outcome->aggregate_type;
    *bytes = outcome->aggregate_bytes;
    return true;
}
XR_FUNC bool xr_rbc_outcome_aggregate_type(const XrRbcOutcome *outcome,
    uint32_t ordinal, XrPodType *type) {
    if (!outcome || outcome->status != XR_RBC_OK || !type || ordinal >= outcome->aggregate_type_count) return false;
    *type = outcome->aggregate_types[ordinal];
    return true;
}

XR_FUNC bool xr_rbc_outcome_string(const XrRbcOutcome *outcome, const XrtString **borrowed) {
    if (!outcome || outcome->status != XR_RBC_OK || outcome->result_kind != XR_RBC_STRING ||
        !outcome->value.string || !borrowed) return false;
    *borrowed = outcome->value.string;
    return true;
}
XR_FUNC bool xr_rbc_outcome_take_string(XrRbcOutcome *outcome, XrtString **output) {
    if (!outcome || outcome->status != XR_RBC_OK || outcome->result_kind != XR_RBC_STRING ||
        !outcome->value.string || !output || *output) return false;
    *output = outcome->value.string;
    outcome->value.string = NULL;
    return true;
}
XR_FUNC bool xr_rbc_outcome_array(const XrRbcOutcome *outcome, const XrtArray **borrowed) {
    if (!outcome || outcome->status != XR_RBC_OK || !rbc_array_kind(outcome->result_kind) ||
        !outcome->value.array || !borrowed) return false;
    *borrowed = outcome->value.array;
    return true;
}
XR_FUNC bool xr_rbc_outcome_take_array(XrRbcOutcome *outcome, XrtArray **output) {
    if (!outcome || outcome->status != XR_RBC_OK || !rbc_array_kind(outcome->result_kind) ||
        !outcome->value.array || !output || *output) return false;
    *output = outcome->value.array; outcome->value.array = NULL;
    return true;
}
XR_FUNC bool xr_rbc_outcome_value_failure(const XrRbcOutcome *outcome,
    XrXirValueStatus *status, uint32_t *source_id, const XrtLocationOwner **locations) {
    if (!outcome || outcome->status != XR_RBC_VALUE_FAILURE || !status || !source_id || !locations) return false;
    *status = outcome->failure.detail.value.status;
    *source_id = outcome->failure.detail.value.source_id;
    *locations = outcome->locations;
    return true;
}

XR_FUNC bool xr_rbc_outcome_output_failure(const XrRbcOutcome *outcome,
    XrtOutputFailure *facts, const XrtLocationOwner **borrowed_locations) {
    if (!outcome || outcome->status != XR_RBC_OUTPUT_FAILURE || !facts || !borrowed_locations)
        return false;
    *facts = outcome->failure.detail.output;
    *borrowed_locations = outcome->locations;
    return true;
}

XR_FUNC bool xr_rbc_outcome_arithmetic_panic(const XrRbcOutcome *outcome,
    XrtArithmeticPanic *facts, const XrtLocationOwner **borrowed_locations) {
    if (!outcome || outcome->status != XR_RBC_TYPED_PANIC || !facts || !borrowed_locations)
        return false;
    *facts = outcome->failure.detail.panic;
    *borrowed_locations = outcome->locations;
    return true;
}

XR_FUNC bool xr_rbc_outcome_null_unwrap(const XrRbcOutcome *outcome,
    uint32_t *source_id, const XrtLocationOwner **borrowed_locations) {
    if (!outcome || outcome->status != XR_RBC_NULL_UNWRAP || !source_id || !borrowed_locations) return false;
    *source_id = outcome->failure.detail.panic.source_id;
    *borrowed_locations = outcome->locations;
    return true;
}

XR_FUNC bool xr_rbc_outcome_host_clock_failure(const XrRbcOutcome *outcome,
    XrtHostClockFailure *facts, const XrtLocationOwner **borrowed_locations) {
    if (!outcome || outcome->status != XR_RBC_HOST_CLOCK_FAILURE || !facts || !borrowed_locations)
        return false;
    *facts = outcome->failure.detail.clock;
    *borrowed_locations = outcome->locations;
    return true;
}

XR_FUNC void xr_rbc_outcome_free(XrRbcOutcome **outcome) {
    if (!outcome || !*outcome) return;
    XrRbcOutcome *owned = *outcome;
    *outcome = NULL;
    xr_free(owned->aggregate_bytes);
    xr_free(owned->aggregate_types);
    if (owned->result_kind == XR_RBC_STRING) xrt_string_owned_drop(&owned->value.string);
    if (owned->result_kind == XR_RBC_MAP_STRING_I64) xrt_map_owned_drop(&owned->value.map);
    if (rbc_array_kind(owned->result_kind)) xrt_array_owned_drop(&owned->value.array);
    xrt_locations_release(owned->locations);
    xr_free(owned);
}

XR_FUNC const char *xr_rbc_dispatch_name(void) {
#if defined(__clang__) || defined(__GNUC__)
    return "computed-goto";
#else
    return "switch";
#endif
}
