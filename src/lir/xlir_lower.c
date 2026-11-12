/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower.c - Scalar Cell promotion and source-backed SSA lowering
 *
 * KEY CONCEPT:
 *   Only nonescaping scalar Cells become SSA state. Liveness chooses block
 *   parameters, and each original edge receives its own ordered arguments.
 */
#include "xlir_lower.h"
#include "xlir_builder.h"
#include "xlir_internal.h"
#include "xlir_string_elide.h"
#include "xlir_array.h"
#include "../xir/xxir_source_query.h"
#include "../xir/xxir_generic.h"
#include "../xir/xxir_types.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

typedef struct Lower {
    const XrLirLowerRequest *request;
    const XrXirModule *source;
    XrLirModule *module;
    XrLirType *constructed_types;
    uint32_t *constructed_stack;
    XrLirFunction *functions, print, print_string, clocks[4];
    uint8_t *active;
    uint32_t *queue, queued;
    uint32_t function, block, instruction;
    XrLirStatus status;
    XrLirLowerFailure *failure;
    uint32_t *module_order, *initializer_owner;
    uint8_t *active_modules;
    uint32_t initialization_count;
    XrLirStateLayout state_layout;
    uint32_t *state_slots, **module_cells;
    uint8_t *state_required;
    bool prints, string_prints, arrays;
    uint8_t clock_kinds;
} Lower;
typedef struct LowerArrayCells {
    XrLirValue *owners;
    uint32_t *indices;
    uint8_t *use, *define, *live;
    uint32_t count;
} LowerArrayCells;
typedef struct LowerFunction {
    Lower *owner;
    const XrXirFunction *source;
    XrLirFunction function;
    XrLirBlock *blocks;
    XrLirValue *values, *states, *current, *frame_cells;
    uint8_t *cell_retired, *promoted_arrays, *retired_values;
    uint32_t *array_values, *array_places, *value_blocks;
    XrLirValue state;
    XrLirType *cell_types, *cell_elements;
    uint32_t *cell_for, *phis, *order, *stack, *next;
    uint8_t *reachable, *use, *define, *live;
    uint8_t *literal_uses;
    uint32_t value_count, cell_count, ordered, frame_cell_count, retired_value_count;
    LowerArrayCells array_cells;
} LowerFunction;

static XrLirType lower_array_element(const Lower *c, XrXirType type) {
    if (!xr_xir_type_is_array(c->source->types, type)) return XR_LIR_TYPE_INVALID;
    XrXirType element = xr_xir_array_element(c->source->types, type);
    return element == XR_XIR_I64 ? XR_LIR_I64 :
        element == XR_XIR_STRING ? XR_LIR_STRING :
        element == XR_XIR_U8 ? XR_LIR_U8 : element == XR_XIR_U64 ? XR_LIR_U64 : XR_LIR_TYPE_INVALID;
}
static bool lower_managed_type(XrLirType type) {
    return type == XR_LIR_STRING || type == XR_LIR_ARRAY;
}
static XrLirType lower_type(const Lower *c, XrXirType type) {
    uint32_t id = (uint32_t)type;
    if (c->constructed_types && id >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
        id - XR_XIR_CONSTRUCTED_TYPE_BASE < c->source->types->count &&
        c->constructed_types[id - XR_XIR_CONSTRUCTED_TYPE_BASE])
        return c->constructed_types[id - XR_XIR_CONSTRUCTED_TYPE_BASE];
    if (lower_array_element(c, type)) return XR_LIR_ARRAY;
    if (xr_xir_type_is_cell(c->source->types, type) &&
        lower_array_element(c, xr_xir_cell_element(c->source->types, type)))
        return XR_LIR_ARRAY_CELL;
    if (type == XR_XIR_UNIT)
        return XR_LIR_UNIT;
    if (type == XR_XIR_I64)
        return XR_LIR_I64;
    if (type == XR_XIR_U8)
        return XR_LIR_U8;
    if (type == XR_XIR_U64)
        return XR_LIR_U64;
    if (type == XR_XIR_BOOL)
        return XR_LIR_I1;
    if (type == XR_XIR_F64)
        return XR_LIR_F64;
    if (type == XR_XIR_STRING)
        return XR_LIR_STRING;
    return XR_LIR_TYPE_INVALID;
}

static XrLirSource lower_range_source(const Lower *c, XrXirSourceRange range) {
    XrLirSource result = {0};
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(c->request->snapshot);
    if (!view) return result;
    /* Debug points can retain only a start coordinate. Represent that fact as
     * an empty range rather than dropping the available source location. */
    if (!range.end_line) {
        range.end_line = range.line;
        range.end_column = range.column;
    }
    if (range.module >= view->module_count || range.line <= 0 || range.column <= 0 ||
        range.end_line < range.line || range.end_column <= 0)
        return result;
    const XrXirSourceQueryModule *module = &view->modules[range.module];
    const char *path = module->path ? module->path : module->identity;
    if (!path || !path[0])
        return result;
    result = (XrLirSource) {path,
                            strlen(path),
                            (uint32_t) range.line,
                            (uint32_t) range.column,
                            (uint32_t) range.end_line,
                            (uint32_t) range.end_column};
    return result;
}

static XrLirSource lower_source(const Lower *c) {
    XrLirSource result = {0};
    const XrXirSourceDebugView *debug = xr_xir_compile_source_snapshot_debug(c->request->snapshot);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(c->request->snapshot);
    uint32_t origin = UINT32_MAX;
    if (!debug || !view ||
        xr_xir_compile_specialization_origin(c->request->closed, c->function, &origin) !=
            XR_XIR_OK ||
        origin >= debug->function_count)
        return result;
    const XrXirSourceDebugFunction *function = &debug->functions[origin];
    XR_DCHECK(function->function == origin, "source debug functions use their source ordinal");
    XrXirSourceRange range = {0};
    if (c->instruction < function->instruction_count && function->points)
        range = function->points[c->instruction].range;
    if ((!range.line || !range.column) && function->declaration &&
        function->declaration <= view->declaration_count)
        range = view->declarations[function->declaration - 1].range;
    if ((!range.line || !range.column) && function->points)
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            if (function->points[i].range.line > 0 && function->points[i].range.column > 0) {
                range = function->points[i].range;
                break;
            }
    return lower_range_source(c, range);
}

static XrLirSource lower_slot_source(const Lower *c, uint32_t slot) {
    const XrXirSourceDebugView *debug = xr_xir_compile_source_snapshot_debug(c->request->snapshot);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(c->request->snapshot);
    if (debug && view)
        for (uint32_t i = 0; i < debug->global_count; ++i) {
            const XrXirSourceDebugGlobal *global = &debug->globals[i];
            if (global->slot == slot && global->declaration &&
                global->declaration <= view->declaration_count)
                return lower_range_source(c, view->declarations[global->declaration - 1].range);
        }
    return lower_source(c);
}

static XrLirStatus lower_error_source(Lower *c, XrLirStatus status, const char *message,
                                      const XrLirSource *source) {
    if (c->status != XR_LIR_OK)
        return c->status;
    XR_DCHECK(status != XR_LIR_OK, "lowering failures retain a non-success status");
    c->status = status;
    if (!c->failure)
        return status;
    XrLirDiagnostic *d = &c->failure->diagnostic;
    *d = (XrLirDiagnostic) {.status = status,
                            .function = c->function + 1,
                            .block = c->block + 1,
                            .instruction = c->instruction == UINT32_MAX ? 0 : c->instruction + 1};
    snprintf(d->message, sizeof(d->message), "%s", message);
    if (source && source->path) {
        char *path = source->path_length == SIZE_MAX ? NULL : xr_malloc(source->path_length + 1);
        if (!path) {
            c->status = d->status = XR_LIR_OUT_OF_MEMORY;
            snprintf(d->message, sizeof(d->message), "cannot retain LIR lowering diagnostic path");
            return c->status;
        }
        memcpy(path, source->path, source->path_length);
        path[source->path_length] = 0;
        c->failure->owned_path = path;
        d->source = *source;
        d->source.path = path;
    }
    return c->status;
}
static XrLirStatus lower_error(Lower *c, XrLirStatus status, const char *message) {
    XrLirSource source = lower_source(c);
    return lower_error_source(c, status, message, &source);
}
static bool lower_unsupported(Lower *c, const char *feature) {
    char message[192];
    snprintf(message, sizeof(message), "%s is not supported by LIR lowering yet", feature);
    lower_error(c, XR_LIR_UNSUPPORTED, message);
    return false;
}
static void *lower_alloc(Lower *c, size_t count, size_t size) {
    if (!count)
        return NULL;
    if (!size || count > SIZE_MAX / size) {
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "LIR lowering allocation size overflow");
        return NULL;
    }
    void *memory = xr_malloc(count * size);
    if (!memory)
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "LIR lowering allocation failed");
    else
        memset(memory, 0, count * size);
    return memory;
}
static bool lower_build(Lower *c, XrLirStatus status) {
    if (status == XR_LIR_OK)
        return true;
    XrLirDiagnostic d = {0};
    xr_lir_module_finish(c->module, &d);
    XrLirSource source = d.source.path ? d.source : lower_source(c);
    lower_error_source(c, status, d.message[0] ? d.message : "LIR construction failed", &source);
    return false;
}


/* Only admitted payloads and signatures receive physical type identities.
 * An unused source descriptor cannot change the retained program's frontier. */
static bool lower_receive_type(Lower *c, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(c->source->types, type);
    if (!node || (node->kind != XR_XIR_TYPE_FIXED_ARRAY && node->kind != XR_XIR_TYPE_NULLABLE)) return true;
    if (!c->constructed_types) {
        c->constructed_types = lower_alloc(c, c->source->types->count, sizeof(*c->constructed_types));
        if (!c->constructed_types) return false;
    }
    if (node->kind == XR_XIR_TYPE_NULLABLE && !c->constructed_stack) {
        c->constructed_stack = lower_alloc(c, c->source->types->count, sizeof(*c->constructed_stack));
        if (!c->constructed_stack) return false;
    }
    uint32_t depth = 0;
    while (node && (node->kind == XR_XIR_TYPE_FIXED_ARRAY || node->kind == XR_XIR_TYPE_NULLABLE)) {
        uint32_t index = (uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (c->constructed_types[index]) break;
        if (node->kind == XR_XIR_TYPE_FIXED_ARRAY) {
            if (!lower_build(c, xr_lir_fixed_array_type(c->module, lower_type(c, node->element),
                node->parameter_count, &c->constructed_types[index]))) return false;
            break;
        }
        XR_DCHECK(depth < c->source->types->count, "nullable dependencies follow the owned acyclic type pool");
        c->constructed_stack[depth++] = index;
        type = node->element;
        node = xr_xir_type_node(c->source->types, type);
    }
    XrLirType element = lower_type(c, type);
    if (!element || element == XR_LIR_STRING || element == XR_LIR_ARRAY)
        return lower_unsupported(c, "Nullable payload has no inline POD layout");
    while (depth) {
        uint32_t index = c->constructed_stack[--depth];
        if (!lower_build(c, xr_lir_nullable_type(c->module, element, &element))) return false;
        c->constructed_types[index] = element;
    }
    return true;
}

static bool lower_known(XrXirOp op) {
    switch (op) {
        case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT:
        case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME:
        case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_NULLABLE_UNWRAP:
        case XR_XIR_FIXED_ARRAY_NEW: case XR_XIR_FIXED_ARRAY_GET: case XR_XIR_FIXED_ARRAY_LEN:
        case XR_XIR_ARRAY_NEW:
        case XR_XIR_ARRAY_REPEAT:
        case XR_XIR_ARRAY_LEN:
        case XR_XIR_ARRAY_GET:
        case XR_XIR_ARRAY_SET:
        case XR_XIR_ARRAY_PUSH:
        case XR_XIR_CELL_PLACE:
        case XR_XIR_SLOT_PLACE:
        case XR_XIR_PLACE_READ:
        case XR_XIR_PLACE_WRITE:
        case XR_XIR_SLOT_INIT:
        case XR_XIR_SLOT_LOAD:
        case XR_XIR_CONST_STRING:
        case XR_XIR_CONST_BOOL:
        case XR_XIR_CONST_INT:
        case XR_XIR_CONST_FLOAT:
        case XR_XIR_COPY:
        case XR_XIR_TO_STRING:
        case XR_XIR_CONCAT_STRING:
        case XR_XIR_STRING_LEN:
        case XR_XIR_ADD_INT:
        case XR_XIR_SUB_INT:
        case XR_XIR_MUL_INT:
        case XR_XIR_DIV_INT:
        case XR_XIR_REM_INT:
        case XR_XIR_EQ_INT:
        case XR_XIR_NE_INT:
        case XR_XIR_LT_INT:
        case XR_XIR_LE_INT:
        case XR_XIR_GT_INT:
        case XR_XIR_GE_INT:
        case XR_XIR_EQUAL:
        case XR_XIR_ADD_FLOAT:
        case XR_XIR_SUB_FLOAT:
        case XR_XIR_MUL_FLOAT:
        case XR_XIR_DIV_FLOAT:
        case XR_XIR_NEG_FLOAT:
        case XR_XIR_EQ_FLOAT:
        case XR_XIR_NE_FLOAT:
        case XR_XIR_LT_FLOAT:
        case XR_XIR_LE_FLOAT:
        case XR_XIR_GT_FLOAT:
        case XR_XIR_GE_FLOAT:
        case XR_XIR_CONVERT_NUMBER:
        case XR_XIR_CALL:
        case XR_XIR_PRINT:
        case XR_XIR_PHI:
        case XR_XIR_CELL_NEW:
        case XR_XIR_CELL_READ:
        case XR_XIR_CELL_WRITE:
        case XR_XIR_LOCAL_NEW:
        case XR_XIR_LOCAL_READ:
        case XR_XIR_LOCAL_WRITE:
        case XR_XIR_JUMP:
        case XR_XIR_BRANCH:
        case XR_XIR_RETURN:
            return true;
        default:
            return false;
    }
}

typedef struct LowerStorageCall { uint32_t caller; size_t next; } LowerStorageCall;

static bool lower_storage_call(const Lower *c, const XrXirFunction *f,
    const XrXirInstruction *op, uint32_t *target) {
    if ((f->parameter_count && !f->parameters) ||
        op->immediate < 0 || (uint64_t)op->immediate >= c->source->function_count ||
        op->args[0] > f->operand_count || op->args[1] > f->operand_count - op->args[0] ||
        (op->args[1] && !f->operands)) return false;
    uint32_t callee = (uint32_t)op->immediate;
    const XrXirFunction *to = &c->source->functions[callee];
    if (c->initializer_owner[callee] != UINT32_MAX || op->type != to->result ||
        op->args[1] != to->parameter_count || (to->parameter_count && !to->parameters)) return false;
    for (uint32_t a = 0; a < op->args[1]; ++a) {
        uint32_t value = f->operands[op->args[0] + a];
        if ((uint64_t)value >= (uint64_t)f->parameter_count + f->instruction_count ||
            xr_xir_operand_type(f, value) != to->parameters[a]) return false;
    }
    *target = callee;
    return true;
}

/* One trailing capability follows the reverse direct-call graph. Local SSA
 * Cells do not require module storage, and storage-free helpers keep their ABI. */
static bool lower_storage_closures(Lower *c) {
    uint32_t count = c->source->function_count;
    size_t *heads = lower_alloc(c, count, sizeof(*heads));
    uint32_t *queue = lower_alloc(c, count, sizeof(*queue));
    LowerStorageCall *calls = NULL;
    size_t edges = 0, queued = 0;
    c->state_required = lower_alloc(c, count, 1);
    bool good = heads && queue && c->state_required;
    if (!good) goto done;
    for (uint32_t f = 0; f < count; ++f) {
        heads[f] = SIZE_MAX;
        const XrXirFunction *body = &c->source->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_LOAD ||
                op->op == XR_XIR_SLOT_PLACE)
                c->state_required[f] = 1;
            uint32_t target;
            if (op->op == XR_XIR_CALL && lower_storage_call(c, body, op, &target)) {
                if (edges == SIZE_MAX) {
                    lower_error(c, XR_LIR_OUT_OF_MEMORY, "module call edge count overflow");
                    good = false;
                    goto done;
                }
                ++edges;
            }
        }
    }
    calls = lower_alloc(c, edges, sizeof(*calls));
    if (edges && !calls) { good = false; goto done; }
    size_t at = 0;
    for (uint32_t f = 0; f < count; ++f) {
        const XrXirFunction *body = &c->source->functions[f];
        if (c->state_required[f]) queue[queued++] = f;
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            uint32_t target;
            if (op->op != XR_XIR_CALL || !lower_storage_call(c, body, op, &target)) continue;
            XR_DCHECK(at < edges, "the immutable call graph retains its counted edges");
            calls[at] = (LowerStorageCall){f, heads[target]};
            heads[target] = at++;
        }
    }
    XR_DCHECK(at == edges && queued <= count, "each initial state user is queued once");
    for (size_t cursor = 0; cursor < queued; ++cursor)
        for (size_t edge = heads[queue[cursor]]; edge != SIZE_MAX; edge = calls[edge].next) {
            uint32_t caller = calls[edge].caller;
            if (c->state_required[caller]) continue;
            XR_DCHECK(queued < count, "each function receives its capability once");
            c->state_required[caller] = 1;
            queue[queued++] = caller;
        }
done:
    xr_free(calls);
    xr_free(queue);
    xr_free(heads);
    return good;
}

static uint32_t lower_hidden_state(const Lower *c, uint32_t function) {
    return c->state_layout.slot_count && c->state_required[function] ? 1u : 0u;
}

/* Constructors retain their declaration identity until publication. Readers
 * use the actual reference produced at the source SLOT_LOAD instruction. */
static uint32_t lower_module_cell(const Lower *c, uint32_t function, uint32_t value) {
    if (!c->state_layout.slot_count) return UINT32_MAX;
    const XrXirFunction *f = &c->source->functions[function];
    if (value < f->parameter_count || value - f->parameter_count >= f->instruction_count)
        return UINT32_MAX;
    uint32_t index = value - f->parameter_count;
    if (c->module_cells[function] && c->module_cells[function][index] != UINT32_MAX)
        return c->module_cells[function][index];
    const XrXirInstruction *op = &f->instructions[index];
    const XrXirDeclarations *d = c->source->declarations;
    if (op->op == XR_XIR_SLOT_LOAD && op->immediate >= 0 &&
        (uint64_t)op->immediate < d->slot_count && d->slots[op->immediate].mutable)
        return (uint32_t)op->immediate;
    return UINT32_MAX;
}

static void lower_modules_free(Lower *c) {
    xr_free(c->constructed_types);
    xr_free(c->constructed_stack);
    if (c->module_cells)
        for (uint32_t f = 0; f < c->source->function_count; ++f)
            xr_free(c->module_cells[f]);
    xr_free(c->module_cells);
    xr_free(c->state_slots);
    xr_free((void *)c->state_layout.modules);
    xr_free((void *)c->state_layout.dependencies);
    xr_free((void *)c->state_layout.slots);
    xr_free(c->state_required);
    xr_free(c->module_order);
    xr_free(c->active_modules);
    xr_free(c->initializer_owner);
}
static bool lower_modules(Lower *c) {
    const XrXirDeclarations *d = c->source->declarations;
    c->module_order = lower_alloc(c, d->module_count, sizeof(*c->module_order));
    c->active_modules = lower_alloc(c, d->module_count, 1);
    c->initializer_owner = lower_alloc(c, c->source->function_count, sizeof(*c->initializer_owner));
    if (!c->module_order || !c->active_modules || !c->initializer_owner)
        return false;
    for (uint32_t f = 0; f < c->source->function_count; ++f) c->initializer_owner[f] = UINT32_MAX;
    for (uint32_t m = 0; m < d->module_count; ++m) {
        uint32_t init = d->modules[m].initializer;
        if (init >= c->source->function_count || c->initializer_owner[init] != UINT32_MAX ||
            d->modules[m].storage != XR_XIR_MODULE_FRESH_STORAGE)
            return lower_unsupported(c, "module initializer or receiving storage");
        c->initializer_owner[init] = m;
    }
    XrXirStatus status = xr_xir_compile_declarations_active(
        xr_xir_compile_artifact_context(c->request->closed), d, c->module_order,
        c->active_modules, &c->initialization_count);
    if (status != XR_XIR_OK) {
        lower_error(c, status == XR_XIR_OUT_OF_MEMORY ? XR_LIR_OUT_OF_MEMORY : XR_LIR_UNSUPPORTED,
                    "cannot read Checked module initialization order");
        return false;
    }
    return true;
}
/* The receiving LIR owner copies the layout after declaration slots have
 * been assigned their canonical payload sizes and dense physical ordinals. */
static bool lower_state_layout(Lower *c) {
    const XrXirDeclarations *d = c->source->declarations;
    if (!d->slot_count) return true;
    c->state_slots = lower_alloc(c, d->slot_count, sizeof(*c->state_slots));
    XrLirStateSlot *slots = lower_alloc(c, d->slot_count, sizeof(*slots));
    c->state_layout.slots = slots;
    if (!c->state_slots || !slots) return false;
    uint64_t end = 0;
    uint32_t alignment = 1;
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        c->state_slots[s] = UINT32_MAX;
        const XrXirSlot *slot = &d->slots[s];
        if (!c->active_modules[slot->module]) continue;
        XrXirType type = slot->type;
        if (slot->mutable) {
            if (!xr_xir_type_is_cell(c->source->types, type))
                return lower_unsupported(c, "mutable module slot without a Cell");
            type = xr_xir_cell_element(c->source->types, type);
        }
        if (!lower_receive_type(c, type)) return false;
        XrLirStateSlot physical = {.module = slot->module, .payload = lower_type(c, type),
            .mutable = slot->mutable != 0, .array_element = lower_array_element(c, type)};
        if (!xr_lir_internal_state_payload(c->module, physical.payload, &physical.size, &physical.alignment)) {
            XrLirSource source = lower_slot_source(c, s);
            lower_error_source(c, XR_LIR_UNSUPPORTED,
                "module slot payload type is not supported by LIR lowering yet", &source);
            return false;
        }
        uint64_t padding = (physical.alignment - end % physical.alignment) % physical.alignment;
        if (end > UINT64_MAX - padding || end + padding > UINT64_MAX - physical.size) {
            lower_error(c, XR_LIR_OUT_OF_MEMORY, "module payload size overflow");
            return false;
        }
        physical.offset = end + padding;
        end = physical.offset + physical.size;
        if (physical.alignment > alignment) alignment = physical.alignment;
        c->state_slots[s] = c->state_layout.slot_count;
        slots[c->state_layout.slot_count++] = physical;
    }
    if (!c->state_layout.slot_count) return true;
    uint64_t padding = (alignment - end % alignment) % alignment;
    if (end > UINT64_MAX - padding) {
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "aligned module payload size overflow");
        return false;
    }
    c->state_layout.payload_size = end + padding;
    c->state_layout.payload_alignment = alignment;
    XrLirStateModule *modules = lower_alloc(c, d->module_count, sizeof(*modules));
    c->state_layout.modules = modules;
    c->state_layout.module_count = d->module_count;
    if (!modules) return false;
    uint64_t dependencies = 0;
    for (uint32_t m = 0; m < d->module_count; ++m) {
        modules[m].first_dependency = (uint32_t)dependencies;
        dependencies += d->modules[m].dependency_count;
        if (dependencies > UINT32_MAX) {
            lower_error(c, XR_LIR_OUT_OF_MEMORY, "module dependency count overflow");
            return false;
        }
        modules[m].dependency_count = d->modules[m].dependency_count;
    }
    c->state_layout.dependency_count = (uint32_t)dependencies;
    uint32_t *targets = lower_alloc(c, (size_t)dependencies, sizeof(*targets));
    c->state_layout.dependencies = targets;
    if (dependencies && !targets) return false;
    for (uint32_t m = 0; m < d->module_count; ++m)
        if (modules[m].dependency_count)
            memcpy(targets + modules[m].first_dependency, d->modules[m].dependencies,
                (size_t)modules[m].dependency_count * sizeof(*targets));
    return true;
}

static bool lower_state_constructors(Lower *c) {
    const XrXirDeclarations *d = c->source->declarations;
    if (!c->state_layout.slot_count) return true;
    c->module_cells = lower_alloc(c, c->source->function_count, sizeof(*c->module_cells));
    uint8_t *published = lower_alloc(c, d->slot_count, 1);
    if (!c->module_cells || !published) { xr_free(published); return false; }
    bool good = true;
    for (uint32_t m = 0; m < c->initialization_count && good; ++m) {
        uint32_t module = c->module_order[m], init = d->modules[module].initializer;
        const XrXirFunction *function = &c->source->functions[init];
        uint32_t *constructors = lower_alloc(c, function->instruction_count, sizeof(*constructors));
        c->module_cells[init] = constructors;
        if (!constructors) { good = false; break; }
        for (uint32_t i = 0; i < function->instruction_count; ++i) constructors[i] = UINT32_MAX;
        for (uint32_t i = 0; i < function->instruction_count && good; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_SLOT_INIT) continue;
            c->function = init;
            c->instruction = i;
            if (op->immediate < 0 || (uint64_t)op->immediate >= d->slot_count) {
                good = lower_unsupported(c, "module slot publication ordinal");
                break;
            }
            uint32_t slot = (uint32_t)op->immediate;
            if (d->slots[slot].module != module || published[slot]) {
                good = lower_unsupported(c, "module slot publication owner");
                break;
            }
            published[slot] = 1;
            if (!d->slots[slot].mutable) continue;
            uint32_t value = op->args[0];
            if (value < function->parameter_count ||
                value - function->parameter_count >= function->instruction_count) {
                good = lower_unsupported(c, "module Cell constructor identity");
                break;
            }
            uint32_t at = value - function->parameter_count;
            const XrXirInstruction *constructor = &function->instructions[at];
            if (constructor->op != XR_XIR_CELL_NEW || constructor->type != d->slots[slot].type ||
                constructors[at] != UINT32_MAX) {
                good = lower_unsupported(c, "module Cell publication without its direct constructor");
                break;
            }
            constructors[at] = slot;
        }
    }
    for (uint32_t s = 0; s < d->slot_count && good; ++s)
        if (c->state_slots[s] != UINT32_MAX && !published[s])
            good = lower_unsupported(c, "module slot without publication");
    xr_free(published);
    return good && lower_storage_closures(c);
}

/* Reuse the bounded function worklist without changing the retained root set.
 * The second visit bit belongs only to this structural local-call walk. */
static bool lower_local_pure(Lower *c, uint32_t selected) {
    c->function = selected < c->source->function_count ? selected : c->function;
    c->block = 0;
    c->instruction = UINT32_MAX;
    if (selected >= c->source->function_count || !c->active[selected])
        return lower_unsupported(c, "local function outside the retained program closure");
    c->queued = 1;
    c->queue[0] = selected;
    c->active[selected] |= 2;
    for (uint32_t at = 0; at < c->queued; ++at) {
        c->function = c->queue[at];
        const XrXirFunction *f = &c->source->functions[c->function];
        for (uint32_t b = 0; b < f->block_count; ++b) {
            c->block = b;
            for (uint32_t i = f->blocks[b].first; i < f->blocks[b].first + f->blocks[b].count;
                 ++i) {
                c->instruction = i;
                const XrXirInstruction *op = &f->instructions[i];
                if (op->op == XR_XIR_PRINT || op->op == XR_XIR_CLOCK_NANOS ||
                    op->op == XR_XIR_UTC_OFFSET_AT || !lower_known(op->op))
                    return lower_unsupported(c, "effect in local function closure");
                if (op->op != XR_XIR_CALL)
                    continue;
                if (op->immediate < 0 || (uint64_t) op->immediate >= c->source->function_count ||
                    !(c->active[(uint32_t) op->immediate] & 1))
                    return lower_unsupported(c, "local call outside the retained program closure");
                uint32_t callee = (uint32_t) op->immediate;
                if (!(c->active[callee] & 2)) {
                    c->active[callee] |= 2;
                    c->queue[c->queued++] = callee;
                }
            }
        }
    }
    return true;
}
/* Compiler-only locals, including short-circuit results, obey the same SSA
 * state equations as nonescaping Cells but have no heap identity. */
static bool lower_state_new(XrXirOp op) {
    return op == XR_XIR_CELL_NEW || op == XR_XIR_LOCAL_NEW;
}
static bool lower_state_read(XrXirOp op) {
    return op == XR_XIR_CELL_READ || op == XR_XIR_LOCAL_READ;
}
static bool lower_state_write(XrXirOp op) {
    return op == XR_XIR_CELL_WRITE || op == XR_XIR_LOCAL_WRITE;
}
/* A direct, nonescaping local Cell can use the existing SSA state inventory.
 * Module Cells and aliases keep their physical activation storage. */
static bool lower_promoted_string(const Lower *c, const XrXirFunction *f,
    const XrXirInstruction *op) {
    if (f->block_count == 1) return false;
    uint32_t id;
    if (op->op == XR_XIR_CELL_NEW) id = f->parameter_count + (uint32_t)(op - f->instructions);
    else if (op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_WRITE) id = op->args[0];
    else return false;
    return id >= f->parameter_count && id - f->parameter_count < f->instruction_count &&
        f->instructions[id - f->parameter_count].op == XR_XIR_CELL_NEW &&
        lower_module_cell(c, c->function, id) == UINT32_MAX;
}
static bool lower_managed_storage(const LowerFunction *lower, const XrXirInstruction *op) {
    const Lower *c = lower->owner;
    const XrXirFunction *f = lower->source;
    uint32_t id = op->op == XR_XIR_CELL_NEW ? f->parameter_count + (uint32_t)(op - f->instructions) : op->args[0];
    if (lower->promoted_arrays && id < lower->value_count && lower->promoted_arrays[id]) return false;
    XrXirType type;
    if (op->op == XR_XIR_CELL_NEW) type = op->type;
    else if (op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_WRITE)
        type = xr_xir_operand_type(f, op->args[0]);
    else return false;
    return xr_xir_type_is_cell(c->source->types, type) &&
        (lower_array_element(c, xr_xir_cell_element(c->source->types, type)) ||
         (xr_xir_cell_element(c->source->types, type) == XR_XIR_STRING && !lower_promoted_string(c, f, op)));
}
static const XrXirInstruction *lower_term(Lower *c, const XrXirFunction *f, uint32_t b) {
    if (b >= f->block_count) {
        lower_error(c, XR_LIR_VERIFY_ERROR, "invalid Checked block target");
        return NULL;
    }
    const XrXirBlock *block = &f->blocks[b];
    if (!block->count || block->first >= f->instruction_count ||
        block->count > f->instruction_count - block->first) {
        lower_error(c, XR_LIR_VERIFY_ERROR, "invalid Checked block range");
        return NULL;
    }
    return &f->instructions[block->first + block->count - 1];
}
static uint32_t lower_edge_count(const XrXirInstruction *term) {
    return term->op == XR_XIR_JUMP ? 1u : (term->op == XR_XIR_BRANCH ? 2u : 0u);
}
static bool lower_mark(Lower *c, uint32_t function) {
    if (function >= c->source->function_count) {
        lower_error(c, XR_LIR_VERIFY_ERROR, "invalid Checked callee");
        return false;
    }
    if (!c->active[function]) {
        XR_DCHECK(c->queued < c->source->function_count, "each function is queued once");
        c->active[function] = 1;
        c->queue[c->queued++] = function;
    }
    return true;
}

static void lower_literal_use(const XrXirFunction *f, uint32_t value, bool direct,
    uint8_t *uses) {
    if (value < f->parameter_count || value - f->parameter_count >= f->instruction_count)
        return;
    uint32_t index = value - f->parameter_count;
    if (f->instructions[index].op == XR_XIR_CONST_STRING)
        uses[index] = direct && !uses[index] ? 1 : 2;
}
static uint8_t *lower_literal_uses(Lower *c, const XrXirFunction *f) {
    uint8_t *uses = lower_alloc(c, f->instruction_count, 1);
    if (!uses) return NULL;
    static const uint8_t arity[] = {0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) operands,
#include "../xir/xxir_ops.def"
#undef XR_XIR_OP
    };
    for (uint32_t i = 0; i < f->instruction_count; ++i) {
        const XrXirInstruction *op = &f->instructions[i];
        if (!lower_known(op->op)) continue;
        if (op->op == XR_XIR_CALL || op->op == XR_XIR_PRINT || op->op == XR_XIR_PHI ||
            op->op == XR_XIR_ARRAY_NEW || op->op == XR_XIR_ARRAY_SET ||
            op->op == XR_XIR_FIXED_ARRAY_NEW) {
            if (op->args[0] > f->operand_count || op->args[1] > f->operand_count - op->args[0]) {
                lower_unsupported(c, "literal use range");
                xr_free(uses);
                return NULL;
            }
            uint32_t first = op->op == XR_XIR_PHI ? 1u : 0u;
            uint32_t step = op->op == XR_XIR_PHI ? 2u : 1u;
            bool direct = op->op == XR_XIR_PRINT && op->args[1] == 1;
            for (uint32_t a = first; a < op->args[1]; a += step)
                lower_literal_use(f, f->operands[op->args[0] + a], direct, uses);
        } else {
            uint32_t count = op->op == XR_XIR_RETURN ? f->result != XR_XIR_UNIT : arity[op->op];
            for (uint32_t a = 0; a < count; ++a)
                lower_literal_use(f, op->args[a], false, uses);
        }
    }
    return uses;
}
static bool lower_literal_valid(Lower *c, const XrXirFunction *f, uint32_t index) {
    const XrXirInstruction *literal = &f->instructions[index];
    const XrXirDeclarations *d = c->source->declarations;
    if (literal->type != XR_XIR_STRING || literal->immediate < 0 ||
        (uint64_t)literal->immediate >= d->literal_count || !d->literals ||
        (d->literals[literal->immediate].length && !d->literals[literal->immediate].bytes))
        return lower_unsupported(c, "invalid literal identity");
    return true;
}

static bool lower_admit_instruction(Lower *c, const XrXirFunction *f, const XrXirInstruction *op) {
    if (!lower_known(op->op))
        return lower_unsupported(c, xr_xir_op_name(op->op));
    if (op->op == XR_XIR_SLOT_LOAD || op->op == XR_XIR_SLOT_INIT ||
        op->op == XR_XIR_SLOT_PLACE) {
        const XrXirDeclarations *d = c->source->declarations;
        if (op->immediate < 0 || (uint64_t)op->immediate >= d->slot_count ||
            !c->state_slots || c->state_slots[op->immediate] == UINT32_MAX)
            return lower_unsupported(c, "module slot ordinal");
        const XrXirSlot *slot = &d->slots[op->immediate];
        uint32_t reader = d->functions[c->function].module;
        if (op->op != XR_XIR_SLOT_INIT) {
            if (op->type != slot->type || !xr_xir_module_imports(d, reader, slot->module))
                return lower_unsupported(c, "module slot read authority");
        } else if (c->initializer_owner[c->function] != slot->module)
            return lower_unsupported(c, "module slot publication authority");
        return true;
    }
    XrXirType payload = op->op == XR_XIR_CELL_NEW ?
        xr_xir_cell_element(c->source->types, op->type) : op->type;
    if (!lower_receive_type(c, payload)) return false;
    if (lower_array_element(c, op->type) || lower_type(c, op->type) == XR_LIR_ARRAY_CELL)
        c->arrays = true;
    if (op->op == XR_XIR_CELL_PLACE || op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_PLACE_WRITE) {
        XrXirType type = op->op == XR_XIR_PLACE_WRITE ? xr_xir_operand_type(f, op->args[0]) : op->type;
        if (!lower_array_element(c, type)) return lower_unsupported(c, "non-Array place");
        return true;
    }
    if (op->op == XR_XIR_CONST_STRING)
        return lower_literal_valid(c, f, (uint32_t)(op - f->instructions));
    if (op->op == XR_XIR_TO_STRING && xr_xir_operand_type(f, op->args[0]) != XR_XIR_I64 &&
        xr_xir_operand_type(f, op->args[0]) != XR_XIR_F64 &&
        xr_xir_operand_type(f, op->args[0]) != XR_XIR_STRING)
        return lower_unsupported(c, "String conversion other than i64, f64 or String");
    if (op->op == XR_XIR_CONVERT_NUMBER) {
        XrLirType from = lower_type(c, xr_xir_operand_type(f, op->args[0]));
        XrLirType to = lower_type(c, op->type);
        bool from_integer = from >= XR_LIR_I8 && from <= XR_LIR_U64;
        bool to_integer = to >= XR_LIR_I8 && to <= XR_LIR_U64;
        if (!(from_integer && to_integer) &&
            !((from == XR_LIR_I64 || from == XR_LIR_F64) && (to == XR_LIR_I64 || to == XR_LIR_F64)))
            return lower_unsupported(c, "numeric conversion between these exact types");
    }
    if (op->op == XR_XIR_CLOCK_NANOS) {
        if (op->immediate < 1 || op->immediate > 3)
            return lower_unsupported(c, "clock kind");
        c->clock_kinds |= (uint8_t)(1u << (op->immediate - 1));
        return true;
    }
    if (op->op == XR_XIR_UTC_OFFSET_AT) {
        c->clock_kinds |= 8u;
        return true;
    }
    if (op->op == XR_XIR_CELL_NEW) {
        if (!xr_xir_type_is_cell(c->source->types, op->type) ||
            (lower_type(c, xr_xir_cell_element(c->source->types, op->type)) <= XR_LIR_UNIT &&
             lower_module_cell(c, c->function, f->parameter_count +
                 (uint32_t)(op - f->instructions)) == UINT32_MAX))
            return lower_unsupported(c, "non-scalar Cell");
    } else if (!lower_type(c, op->type))
        return lower_unsupported(c, xr_xir_op_name(op->op));
    if ((op->op == XR_XIR_DIV_INT || op->op == XR_XIR_REM_INT) &&
        op->type != XR_XIR_I64)
        return lower_unsupported(c, xr_xir_op_name(op->op));
    if (op->op == XR_XIR_CALL) {
        if (op->immediate < 0 || (uint64_t) op->immediate >= c->source->function_count)
            return lower_unsupported(c, "unresolved direct call");
        const XrXirDeclarations *d = c->source->declarations;
        uint32_t callee = (uint32_t)op->immediate;
        uint32_t owner = d->functions[c->function].module;
        uint32_t target = d->functions[callee].module;
        if (c->initializer_owner[callee] != UINT32_MAX)
            return lower_unsupported(c, "call back to module initializer");
        if (owner >= d->module_count || target >= d->module_count ||
            !c->active_modules[target] || !xr_xir_module_imports(d, owner, target))
            return lower_unsupported(c, "module call authority");
        return lower_mark(c, callee);
    }
    if (op->op == XR_XIR_PRINT) {
        if (op->args[1] != 1 || op->args[0] >= f->operand_count)
            return lower_unsupported(c, "print other than one supported value");
        uint32_t value = f->operands[op->args[0]];
        XrXirType type = xr_xir_operand_type(f, value);
        if (type == XR_XIR_STRING || type == XR_XIR_F64) {
            c->string_prints = true;
        } else if (type == XR_XIR_I64 || type == XR_XIR_U8) c->prints = true;
        else return lower_unsupported(c, "print other than one supported value");
    }
    return true;
}

static bool lower_managed(const Lower *c, const XrXirFunction *f, const uint8_t *literal_uses) {
    bool managed = lower_managed_type(lower_type(c, f->result));
    for (uint32_t p = 0; p < f->parameter_count; ++p) managed |= lower_managed_type(lower_type(c, f->parameters[p]));
    for (uint32_t i = 0; i < f->instruction_count; ++i) {
        const XrXirInstruction *op = &f->instructions[i];
        managed |= lower_array_element(c, op->type) || (op->type == XR_XIR_STRING &&
            (op->op != XR_XIR_CONST_STRING || literal_uses[i] != 1)) ||
            (xr_xir_type_is_cell(c->source->types, op->type) &&
             xr_xir_cell_element(c->source->types, op->type) == XR_XIR_STRING);
    }
    return managed;
}
static bool lower_admit_function(Lower *c, uint32_t function) {
    c->function = function;
    c->block = 0;
    c->instruction = UINT32_MAX;
    const XrXirFunction *f = &c->source->functions[function];
    XR_DCHECK(c->active[function], "only executable functions enter admission");
    if (!lower_receive_type(c, f->result)) return false;
    if (!lower_type(c, f->result) || lower_type(c, f->result) == XR_LIR_ARRAY_CELL)
        return lower_unsupported(c, "function result type");
    c->arrays |= lower_array_element(c, f->result) != XR_LIR_TYPE_INVALID;
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        if (!lower_receive_type(c, f->parameters[p])) return false;
        c->arrays |= lower_type(c, f->parameters[p]) == XR_LIR_ARRAY ||
            lower_type(c, f->parameters[p]) == XR_LIR_ARRAY_CELL;
        if (lower_type(c, f->parameters[p]) <= XR_LIR_UNIT)
            return lower_unsupported(c, "function parameter type");
    }
    const XrXirSourceDebugView *debug = xr_xir_compile_source_snapshot_debug(c->request->snapshot);
    uint32_t origin = UINT32_MAX;
    if (xr_xir_compile_specialization_origin(c->request->closed, function, &origin) != XR_XIR_OK ||
        !debug || origin >= debug->function_count ||
        debug->functions[origin].parameter_count != f->parameter_count ||
        debug->functions[origin].instruction_count != f->instruction_count) {
        lower_error(c, XR_LIR_VERIFY_ERROR, "Checked specialization source mapping differs");
        return false;
    }
    if (!f->block_count || !f->blocks || !f->instructions)
        return lower_unsupported(c, "function without a scalar body");
    uint8_t *seen = lower_alloc(c, f->block_count, 1);
    uint32_t *queue = lower_alloc(c, f->block_count, sizeof(*queue));
    if (!seen || !queue) {
        xr_free(seen);
        xr_free(queue);
        return false;
    }
    uint32_t count = 1;
    queue[0] = 0;
    seen[0] = 1;
    bool good = true;
    for (uint32_t at = 0; at < count && good; ++at) {
        uint32_t b = queue[at];
        c->block = b;
        const XrXirInstruction *term = lower_term(c, f, b);
        if (!term) {
            good = false;
            break;
        }
        const XrXirBlock *block = &f->blocks[b];
        if (block->panic || block->frontier) {
            good = lower_unsupported(c, "panic or cleanup frontier");
            break;
        }
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            c->instruction = i;
            if (!lower_admit_instruction(c, f, &f->instructions[i])) {
                good = false;
                break;
            }
        }
        for (uint32_t e = 0; e < lower_edge_count(term) && good; ++e) {
            uint32_t target = term->targets[e];
            if (target >= f->block_count) {
                lower_error(c, XR_LIR_VERIFY_ERROR, "invalid Checked branch");
                good = false;
                break;
            }
            if (!seen[target]) {
                seen[target] = 1;
                queue[count++] = target;
            }
        }
    }
    xr_free(seen);
    xr_free(queue);
    return good;
}

static bool lower_declare(Lower *c, uint32_t index) {
    c->function = index;
    c->instruction = UINT32_MAX;
    const XrXirFunction *f = &c->source->functions[index];
    size_t name_length = f->name_length;
    if (name_length > SIZE_MAX - 32) {
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "function name size overflow");
        return false;
    }
    char *name = lower_alloc(c, (size_t) f->name_length + 32, 1);
    uint32_t hidden = lower_hidden_state(c, index);
    if (hidden > UINT32_MAX - f->parameter_count) {
        xr_free(name);
        return lower_unsupported(c, "module parameter count");
    }
    uint32_t count = f->parameter_count + hidden;
    XrLirType *parameters = lower_alloc(c, count, sizeof(*parameters));
    if (!name || (count && !parameters)) {
        xr_free(name);
        xr_free(parameters);
        return false;
    }
    int prefix = snprintf(name, 32, "f%u_", index);
    if (prefix < 0 || prefix >= 32) {
        lower_error(c, XR_LIR_OUTPUT_ERROR, "cannot format LIR function identity");
        xr_free(name);
        xr_free(parameters);
        return false;
    }
    memcpy(name + prefix, f->name, f->name_length);
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        parameters[p] = lower_type(c, f->parameters[p]);
        if (parameters[p] == XR_LIR_ARRAY_CELL) parameters[p] = XR_LIR_ARRAY_REF;
    }
    if (hidden) parameters[f->parameter_count] = XR_LIR_STATE;
    XrLirFunctionSpec spec = {.name = name,
                              .name_length = (size_t) prefix + f->name_length,
                              .parameters = parameters,
                              .parameter_count = count,
                              .result = lower_type(c, f->result)};
    bool good = lower_build(c, xr_lir_declare_function(c->module, &spec, &c->functions[index]));
    if (good && spec.result == XR_LIR_ARRAY) {
        XrLirArrayShape shape = {lower_array_element(c, f->result)};
        good = lower_build(c, xr_lir_set_array_result(c->module, c->functions[index], &shape));
    }
    for (uint32_t p = 0; p < f->parameter_count && good; ++p) {
        XrXirType type = f->parameters[p];
        if (xr_xir_type_is_cell(c->source->types, type)) type = xr_xir_cell_element(c->source->types, type);
        if (lower_array_element(c, type)) {
            XrLirValue value = {0};
            XrLirArrayShape shape = {lower_array_element(c, type)};
            good = lower_build(c, xr_lir_function_parameter(c->module, c->functions[index], p, &value)) &&
                lower_build(c, xr_lir_set_array_shape(c->module, value, &shape));
        }
    }
    xr_free(name);
    xr_free(parameters);
    return good;
}

static void lower_function_free(LowerFunction *f) {
    xr_free(f->array_cells.owners);
    xr_free(f->array_cells.indices);
    xr_free(f->array_cells.use);
    xr_free(f->array_cells.define);
    xr_free(f->array_cells.live);
    xr_free(f->literal_uses);
    xr_free(f->frame_cells);
    xr_free(f->promoted_arrays);
    xr_free(f->array_values);
    xr_free(f->array_places);
    xr_free(f->value_blocks);
    xr_free(f->cell_elements);
    xr_free(f->cell_retired);
    xr_free(f->retired_values);
    xr_free(f->blocks);
    xr_free(f->values);
    xr_free(f->states);
    xr_free(f->current);
    xr_free(f->cell_types);
    xr_free(f->cell_for);
    xr_free(f->phis);
    xr_free(f->order);
    xr_free(f->stack);
    xr_free(f->next);
    xr_free(f->reachable);
    xr_free(f->use);
    xr_free(f->define);
    xr_free(f->live);
}
static bool lower_order(LowerFunction *f) {
    Lower *c = f->owner;
    uint32_t count = f->source->block_count;
    f->order = lower_alloc(c, count, sizeof(*f->order));
    f->stack = lower_alloc(c, count, sizeof(*f->stack));
    f->next = lower_alloc(c, count, sizeof(*f->next));
    f->reachable = lower_alloc(c, count, 1);
    f->blocks = lower_alloc(c, count, sizeof(*f->blocks));
    f->phis = lower_alloc(c, count, sizeof(*f->phis));
    if (!f->order || !f->stack || !f->next || !f->reachable || !f->blocks || !f->phis)
        return false;
    bool managed = lower_managed(c, f->source, f->literal_uses);
    uint32_t depth = 1;
    f->stack[0] = 0;
    f->reachable[0] = 1;
    while (depth) {
        uint32_t b = f->stack[depth - 1];
        const XrXirInstruction *term = lower_term(c, f->source, b);
        if (!term)
            return false;
        if (f->next[b] < lower_edge_count(term)) {
            uint32_t target = term->targets[f->next[b]++];
            XR_DCHECK(target < count, "admission checked every reachable successor");
            if (managed && f->reachable[target] == 1)
                for (uint32_t p = 0; p < f->source->parameter_count; ++p)
                    if (f->source->parameters[p] == XR_XIR_STRING) {
                        c->block = b; c->instruction = (uint32_t)(term - f->source->instructions);
                        return lower_unsupported(c, "String parameter across a loop");
                    }
            if (!f->reachable[target]) {
                f->reachable[target] = 1;
                f->stack[depth++] = target;
            }
        } else {
            f->order[f->ordered++] = b;
            f->reachable[b] = 2;
            --depth;
        }
    }
    for (uint32_t i = 0; i < f->ordered / 2; ++i) {
        uint32_t saved = f->order[i];
        f->order[i] = f->order[f->ordered - i - 1];
        f->order[f->ordered - i - 1] = saved;
    }
    for (uint32_t i = 0; i < f->ordered; ++i) f->next[f->order[i]] = i;
    return true;
}

#include "xlir_lower_array_flow.inc.c"

static bool lower_cell_inventory(LowerFunction *f) {
    Lower *c = f->owner;
    const XrXirFunction *source = f->source;
    uint64_t values = (uint64_t) source->parameter_count + source->instruction_count;
    if (values > UINT32_MAX) {
        lower_error(c, XR_LIR_UNSUPPORTED, "Checked value index exceeds LIR capacity");
        return false;
    }
    f->value_count = (uint32_t) values;
    f->values = lower_alloc(c, f->value_count, sizeof(*f->values));
    f->cell_for = lower_alloc(c, f->value_count, sizeof(*f->cell_for));
    f->cell_types = lower_alloc(c, source->instruction_count, sizeof(*f->cell_types));
    f->cell_elements = lower_alloc(c, source->instruction_count, sizeof(*f->cell_elements));
    if ((f->value_count && (!f->values || !f->cell_for)) || !f->cell_types || !f->cell_elements)
        return false;
    for (uint32_t v = 0; v < f->value_count; ++v)
        f->cell_for[v] = UINT32_MAX;
    if (!lower_array_flow_inventory(f)) return false;
    for (uint32_t b = 0; b < source->block_count; ++b)
        if (f->reachable[b]) {
            const XrXirBlock *block = &source->blocks[b];
            for (uint32_t i = block->first; i < block->first + block->count; ++i) {
                const XrXirInstruction *op = &source->instructions[i];
                if (lower_state_new(op->op) &&
                    !lower_managed_storage(f, op) &&
                    lower_module_cell(c, c->function, source->parameter_count + i) == UINT32_MAX) {
                    XR_DCHECK(f->cell_count < source->instruction_count,
                              "each state has one defining instruction");
                    uint32_t id = source->parameter_count + i;
                    f->cell_for[id] = f->cell_count;
                    XrXirType element = op->op == XR_XIR_CELL_NEW
                                            ? xr_xir_cell_element(c->source->types, op->type)
                                            : op->type;
                    f->cell_elements[f->cell_count] = lower_array_element(c, element);
                    f->cell_types[f->cell_count++] = lower_type(c, element);
                }
            }
        }
    return lower_array_flow_values(f);
}
static bool lower_cell_index(LowerFunction *f, uint32_t value, uint32_t *index) {
    if (value >= f->value_count || f->cell_for[value] == UINT32_MAX)
        return lower_unsupported(f->owner, "Cell escape, alias or nonlocal owner");
    *index = f->cell_for[value];
    XR_DCHECK(*index < f->cell_count, "state identifiers come from the local inventory");
    return true;
}

static bool lower_liveness(LowerFunction *f) {
    Lower *c = f->owner;
    const XrXirFunction *source = f->source;
    if (!f->cell_count)
        return true;
    if (source->block_count > SIZE_MAX / f->cell_count) {
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "Cell state size overflow");
        return false;
    }
    size_t pairs = (size_t) source->block_count * f->cell_count;
    f->use = lower_alloc(c, pairs, 1);
    f->define = lower_alloc(c, pairs, 1);
    f->live = lower_alloc(c, pairs, 1);
    f->states = lower_alloc(c, pairs, sizeof(*f->states));
    f->current = lower_alloc(c, f->cell_count, sizeof(*f->current));
    if (!f->use || !f->define || !f->live || !f->states || !f->current)
        return false;
    for (uint32_t b = 0; b < source->block_count; ++b)
        if (f->reachable[b]) {
            const XrXirBlock *block = &source->blocks[b];
            for (uint32_t i = block->first; i < block->first + block->count; ++i) {
                const XrXirInstruction *op = &source->instructions[i];
                lower_array_flow_liveness(f, b, i);
                if (lower_managed_storage(f, op)) continue;
                if (lower_module_cell(c, c->function, lower_state_new(op->op)
                        ? source->parameter_count + i : op->args[0]) != UINT32_MAX) continue;
                if (!lower_state_new(op->op) && !lower_state_read(op->op) &&
                    !lower_state_write(op->op))
                    continue;
                c->block = b;
                c->instruction = i;
                uint32_t cell = 0;
                if (!lower_cell_index(
                        f, lower_state_new(op->op) ? source->parameter_count + i : op->args[0],
                        &cell))
                    return false;
                size_t at = (size_t) b * f->cell_count + cell;
                if (lower_state_read(op->op)) {
                    if (!f->define[at])
                        f->use[at] = 1;
                } else
                    f->define[at] = 1;
            }
        }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t n = f->ordered; n; --n) {
            uint32_t b = f->order[n - 1];
            const XrXirInstruction *term = lower_term(c, source, b);
            if (!term)
                return false;
            for (uint32_t cell = 0; cell < f->cell_count; ++cell) {
                size_t at = (size_t) b * f->cell_count + cell;
                bool needed = f->use[at] != 0;
                if (!f->define[at])
                    for (uint32_t e = 0; e < lower_edge_count(term); ++e)
                        needed =
                            needed || f->live[(size_t) term->targets[e] * f->cell_count + cell];
                if (needed && !f->live[at]) {
                    f->live[at] = 1;
                    changed = true;
                }
            }
        }
    }
    return true;
}

static bool lower_blocks(LowerFunction *f) {
    Lower *c = f->owner;
    const XrXirFunction *source = f->source;
    for (uint32_t b = 0; b < source->block_count; ++b)
        if (f->reachable[b]) {
            const XrXirBlock *block = &source->blocks[b];
            uint32_t phis = 0, count = 0;
            while (phis < block->count &&
                   source->instructions[block->first + phis].op == XR_XIR_PHI)
                ++phis;
            f->phis[b] = phis;
            count = phis;
            for (uint32_t cell = 0; cell < f->cell_count; ++cell)
                if (f->live[(size_t) b * f->cell_count + cell]) {
                    if (count == UINT32_MAX) {
                        lower_error(c, XR_LIR_UNSUPPORTED, "block parameter capacity exceeded");
                        return false;
                    }
                    ++count;
                }
            XrLirType *types = lower_alloc(c, count, sizeof(*types));
            if (count && !types)
                return false;
            for (uint32_t p = 0; p < phis; ++p)
                types[p] = lower_type(c, source->instructions[block->first + p].type);
            uint32_t position = phis;
            for (uint32_t cell = 0; cell < f->cell_count; ++cell)
                if (f->live[(size_t) b * f->cell_count + cell])
                    types[position++] = f->cell_types[cell];
            XR_DCHECK(position == count, "block parameter order matches the live-in inventory");
            bool good = lower_build(
                c, xr_lir_add_block(c->module, f->function, types, count, &f->blocks[b]));
            xr_free(types);
            if (!good)
                return false;
            for (uint32_t p = 0; p < phis; ++p) {
                XrLirValue *value = &f->values[source->parameter_count + block->first + p];
                if (!lower_build(c, xr_lir_block_parameter(c->module, f->blocks[b], p, value)))
                    return false;
                XrXirType type = source->instructions[block->first + p].type;
                if (lower_array_element(c, type)) {
                    XrLirArrayShape shape = {lower_array_element(c, type)};
                    if (!lower_build(c, xr_lir_set_array_shape(c->module, *value, &shape))) return false;
                }
            }
            position = phis;
            for (uint32_t cell = 0; cell < f->cell_count; ++cell)
                if (f->live[(size_t)b * f->cell_count + cell]) {
                    if (f->cell_types[cell] == XR_LIR_ARRAY) {
                        XrLirValue parameter = {0};
                        XrLirArrayShape shape = {f->cell_elements[cell]};
                        if (!lower_build(c, xr_lir_block_parameter(c->module, f->blocks[b], position, &parameter)) ||
                            !lower_build(c, xr_lir_set_array_shape(c->module, parameter, &shape))) return false;
                    }
                    ++position;
                }
        }
    return lower_build(c, xr_lir_set_entry(c->module, f->function, f->blocks[0]));
}

static XrLirPredicate lower_predicate(const XrXirInstruction *op) {
    switch (op->op) {
        case XR_XIR_EQ_INT:
        case XR_XIR_EQ_FLOAT:
            return XR_LIR_EQ;
        case XR_XIR_EQUAL:
            return op->immediate ? XR_LIR_NE : XR_LIR_EQ;
        case XR_XIR_NE_INT:
        case XR_XIR_NE_FLOAT:
            return XR_LIR_NE;
        case XR_XIR_LT_INT:
        case XR_XIR_LT_FLOAT:
            return XR_LIR_LT;
        case XR_XIR_LE_INT:
        case XR_XIR_LE_FLOAT:
            return XR_LIR_LE;
        case XR_XIR_GT_INT:
        case XR_XIR_GT_FLOAT:
            return XR_LIR_GT;
        case XR_XIR_GE_INT:
        case XR_XIR_GE_FLOAT:
            return XR_LIR_GE;
        default:
            return XR_LIR_PREDICATE_NONE;
    }
}
static XrLirOp lower_operation(const LowerFunction *f, const XrXirInstruction *op) {
    switch (op->op) {
        case XR_XIR_CONST_BOOL:
        case XR_XIR_CONST_INT:
        case XR_XIR_CONST_FLOAT:
            return XR_LIR_CONST;
        case XR_XIR_ADD_INT:
            return XR_LIR_ADD;
        case XR_XIR_SUB_INT:
            return XR_LIR_SUB;
        case XR_XIR_MUL_INT:
            return XR_LIR_MUL;
        case XR_XIR_DIV_INT:
            return XR_LIR_DIV;
        case XR_XIR_REM_INT:
            return XR_LIR_REM;
        case XR_XIR_ADD_FLOAT:
            return XR_LIR_FADD;
        case XR_XIR_SUB_FLOAT:
            return XR_LIR_FSUB;
        case XR_XIR_MUL_FLOAT:
            return XR_LIR_FMUL;
        case XR_XIR_DIV_FLOAT:
            return XR_LIR_FDIV;
        case XR_XIR_NEG_FLOAT:
            return XR_LIR_FNEG;
        case XR_XIR_CONVERT_NUMBER: {
            XrLirType from = lower_type(f->owner, xr_xir_operand_type(f->source, op->args[0]));
            XrLirType to = lower_type(f->owner, op->type);
            if (from >= XR_LIR_I8 && from <= XR_LIR_U64 && to >= XR_LIR_I8 && to <= XR_LIR_U64)
                return XR_LIR_INTCAST;
            if (from == XR_LIR_F64 && to == XR_LIR_I64) return XR_LIR_FTOI;
            if (from == XR_LIR_I64 && to == XR_LIR_F64) return XR_LIR_ITOF;
            return XR_LIR_FCAST;
        }
        default:
            if (lower_predicate(op))
                return xr_xir_operand_type(f->source, op->args[0]) == XR_XIR_F64 ? XR_LIR_FCMP
                                                                                 : XR_LIR_ICMP;
            return XR_LIR_OP_INVALID;
    }
}
static bool lower_value(LowerFunction *f, uint32_t id, XrLirValue *out) {
    if (id >= f->value_count || !f->values[id].id) {
        lower_error(f->owner, XR_LIR_VERIFY_ERROR,
                    "Checked value is unavailable during SSA construction");
        return false;
    }
    *out = f->values[id];
    const XrLirValueData *value = &f->owner->module->functions[f->function.id - 1].values[out->id - 1];
    if (lower_managed_type(value->type) && value->block &&
        value->block != f->blocks[f->owner->block].id) {
        uint32_t cell = f->array_values ? f->array_values[id] : UINT32_MAX;
        if (value->type != XR_LIR_ARRAY || cell == UINT32_MAX || !f->current[cell].id)
            return lower_unsupported(f->owner, value->type == XR_LIR_STRING ?
                "String alias across blocks without owned parameters" :
                "managed alias across blocks without owned parameters");
        *out = f->current[cell];
    }
    XR_DCHECK(out->module == f->owner->module && out->function == f->function.id,
              "SSA values remain in their defining output function");
    return true;
}
static bool lower_fixed_instruction(LowerFunction *f, uint32_t b, uint32_t i) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[i];
    XrLirValue *result = &f->values[f->source->parameter_count + i];
    XrLirValue operands[2] = {{0}};
    if (op->op == XR_XIR_FIXED_ARRAY_LEN) {
        if (!lower_value(f, op->args[0], &operands[0])) return false;
        XrXirType type = xr_xir_operand_type(f->source, op->args[0]);
        XrLirInstruction length = {.op=XR_LIR_CONST, .type=XR_LIR_I64,
            .bits=xr_xir_fixed_array_length(c->source->types, type), .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &length, result));
    }
    if (op->op == XR_XIR_FIXED_ARRAY_GET) {
        if (!lower_value(f, op->args[0], &operands[0]) ||
            !lower_value(f, op->args[1], &operands[1])) return false;
        XrLirInstruction get = {.op=XR_LIR_FIXED_ARRAY_GET, .type=XR_LIR_U8,
            .operands=operands, .operand_count=2, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &get, result));
    }
    if (op->args[0] > f->source->operand_count ||
        op->args[1] > f->source->operand_count - op->args[0]) {
        lower_error(c, XR_LIR_VERIFY_ERROR, "invalid Checked fixed byte operand range");
        return false;
    }
    XrLirValue *elements = lower_alloc(c, op->args[1], sizeof(*elements));
    if (!elements) return false;
    bool good = true;
    for (uint32_t a = 0; a < op->args[1] && good; ++a)
        good = lower_value(f, f->source->operands[op->args[0] + a], &elements[a]);
    XrLirInstruction construct = {.op=XR_LIR_FIXED_ARRAY_NEW, .type=lower_type(c, op->type),
        .operands=elements, .operand_count=op->args[1], .source=lower_source(c)};
    if (good) good = lower_build(c,
        xr_lir_emit_value(c->module, f->blocks[b], &construct, result));
    xr_free(elements);
    return good;
}

static bool lower_nullable_instruction(LowerFunction *f, uint32_t b, uint32_t i) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[i];
    XrLirValue *result = &f->values[f->source->parameter_count + i];
    XrLirValue input = {0};
    XrLirOp code = op->op == XR_XIR_NULLABLE_NONE ? XR_LIR_NULLABLE_NONE :
        op->op == XR_XIR_NULLABLE_SOME ? XR_LIR_NULLABLE_SOME :
        op->op == XR_XIR_NULLABLE_IS_SOME ? XR_LIR_NULLABLE_IS_SOME : XR_LIR_NULLABLE_UNWRAP;
    bool operand = op->op != XR_XIR_NULLABLE_NONE && !(op->op == XR_XIR_NULLABLE_SOME &&
        xr_xir_nullable_element(c->source->types, op->type) == XR_XIR_UNIT);
    if (operand && !lower_value(f, op->args[0], &input)) return false;
    XrLirInstruction instruction = {.op=code, .type=lower_type(c, op->type),
        .operands=operand ? &input : NULL, .operand_count=operand, .source=lower_source(c)};
    bool good = lower_build(c, instruction.type == XR_LIR_UNIT ?
        xr_lir_emit_effect(c->module, f->blocks[b], &instruction) :
        xr_lir_emit_value(c->module, f->blocks[b], &instruction, result));
    if (!good || op->op != XR_XIR_NULLABLE_IS_SOME || !op->immediate) return good;
    XrLirValue args[2] = {*result, {0}};
    XrLirInstruction zero = {.op=XR_LIR_CONST, .type=XR_LIR_I1};
    if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &zero, &args[1]))) return false;
    XrLirInstruction invert = {.op=XR_LIR_ICMP, .type=XR_LIR_I1, .predicate=XR_LIR_EQ,
        .operands=args, .operand_count=2, .source=lower_source(c)};
    return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &invert, result));
}

static bool lower_emit_scalar(LowerFunction *f, uint32_t b, uint32_t i) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[i];
    uint32_t result = f->source->parameter_count + i;
    if (op->op == XR_XIR_NULLABLE_NONE || op->op == XR_XIR_NULLABLE_SOME ||
        op->op == XR_XIR_NULLABLE_IS_SOME || op->op == XR_XIR_NULLABLE_UNWRAP)
        return lower_nullable_instruction(f, b, i);
    if (op->op == XR_XIR_FIXED_ARRAY_NEW || op->op == XR_XIR_FIXED_ARRAY_GET ||
        op->op == XR_XIR_FIXED_ARRAY_LEN) return lower_fixed_instruction(f, b, i);
    if (op->op == XR_XIR_CLOCK_NANOS) {
        XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_I64,
            .callee=c->clocks[op->immediate - 1], .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &call, &f->values[result]));
    }
    if (op->op == XR_XIR_UTC_OFFSET_AT) {
        XrLirValue seconds;
        if (!lower_value(f, op->args[0], &seconds)) return false;
        XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_I64,
            .callee=c->clocks[3], .operands=&seconds, .operand_count=1, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &call, &f->values[result]));
    }
    if (op->op == XR_XIR_CONST_STRING) {
        const XrXirLiteral *input = &c->source->declarations->literals[op->immediate];
        XrLirLiteral literal = {0};
        if (!lower_build(c, xr_lir_add_literal(c->module, input->bytes, input->length, &literal))) return false;
        if (f->literal_uses[i] == 1)
            return lower_build(c, xr_lir_literal_address(c->module, f->blocks[b], literal, &f->values[result]));
        XrLirInstruction owned = {.op=XR_LIR_STRING_LITERAL, .type=XR_LIR_STRING,
            .bits=literal.id, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &owned, &f->values[result]));
    }
    if (op->op == XR_XIR_COPY && !lower_managed_type(lower_type(c, op->type)))
        return lower_value(f, op->args[0], &f->values[result]);
    if (op->op == XR_XIR_STRING_LEN) {
        XrLirValue input = {0};
        if (!lower_value(f, op->args[0], &input)) return false;
        XrLirInstruction length = {.op=XR_LIR_STRING_LEN, .type=XR_LIR_I64,
            .operands=&input, .operand_count=1, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &length, &f->values[result]));
    }
    if ((op->op == XR_XIR_COPY && lower_managed_type(lower_type(c, op->type))) ||
        op->op == XR_XIR_TO_STRING || op->op == XR_XIR_CONCAT_STRING) {
        XrLirValue args[2] = {{0}};
        uint32_t count = op->op == XR_XIR_CONCAT_STRING ? 2u : 1u;
        for (uint32_t a = 0; a < count; ++a) if (!lower_value(f, op->args[a], &args[a])) return false;
        XrLirInstruction owned = {.op = lower_type(c, op->type) == XR_LIR_ARRAY ? XR_LIR_ARRAY_RETAIN :
            op->op == XR_XIR_CONCAT_STRING ? XR_LIR_STRING_CONCAT :
            xr_xir_operand_type(f->source, op->args[0]) == XR_XIR_I64 ? XR_LIR_STRING_I64 :
            xr_xir_operand_type(f->source, op->args[0]) == XR_XIR_F64 ? XR_LIR_STRING_F64 : XR_LIR_STRING_RETAIN,
            .type=lower_type(c, op->type), .operands=args, .operand_count=count, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &owned, &f->values[result]));
    }
    XrLirInstruction instruction = {.op = lower_operation(f, op),
                                    .type = lower_type(c, op->type),
                                    .predicate = lower_predicate(op),
                                    .source = lower_source(c)};
    XrLirValue operands[2] = {{0}};
    if (instruction.op == XR_LIR_CONST)
        instruction.bits = (uint64_t) op->immediate;
    else {
        instruction.operand_count =
            op->op == XR_XIR_NEG_FLOAT || op->op == XR_XIR_CONVERT_NUMBER ? 1u : 2u;
        for (uint32_t a = 0; a < instruction.operand_count; ++a)
            if (!lower_value(f, op->args[a], &operands[a]))
                return false;
        instruction.operands = operands;
    }
    if (!instruction.op)
        return lower_unsupported(c, xr_xir_op_name(op->op));
    return lower_build(
        c, xr_lir_emit_value(c->module, f->blocks[b], &instruction, &f->values[result]));
}

static bool lower_array_borrow(LowerFunction *f, uint32_t b, XrLirValue *value) {
    Lower *c = f->owner;
    XrLirType type = c->module->functions[f->function.id - 1].values[value->id - 1].type;
    if (type == XR_LIR_ARRAY_REF) return true;
    XrLirInstruction borrow = {.op=XR_LIR_ARRAY_BORROW, .type=XR_LIR_ARRAY_REF,
        .operands=value, .operand_count=1, .source=lower_source(c)};
    XrLirValue output = {0};
    if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &borrow, &output))) return false;
    *value = output;
    return true;
}

static bool lower_call(LowerFunction *f, uint32_t b, uint32_t i) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[i];
    if (op->args[0] > f->source->operand_count ||
        op->args[1] > f->source->operand_count - op->args[0]) {
        lower_error(c, XR_LIR_VERIFY_ERROR, "invalid Checked call argument range");
        return false;
    }
    if (op->op == XR_XIR_PRINT && op->args[1] == 1) {
        uint32_t value = f->source->operands[op->args[0]];
        if (xr_xir_operand_type(f->source, value) == XR_XIR_U8) {
            XrLirValue argument = {0}, widened = {0};
            if (!lower_value(f, value, &argument)) return false;
            XrLirInstruction cast = {.op=XR_LIR_INTCAST, .type=XR_LIR_I64,
                .operands=&argument, .operand_count=1, .source=lower_source(c)};
            if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &cast, &widened))) return false;
            XrLirInstruction print = {.op=XR_LIR_CALL, .type=XR_LIR_UNIT, .callee=c->print,
                .operands=&widened, .operand_count=1, .source=lower_source(c)};
            return lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &print));
        }
        if (xr_xir_operand_type(f->source, value) == XR_XIR_F64) {
            XrLirValue argument = {0}, text = {0};
            if (!lower_value(f, value, &argument)) return false;
            XrLirInstruction format = {.op=XR_LIR_STRING_F64, .type=XR_LIR_STRING,
                .operands=&argument, .operand_count=1, .source=lower_source(c)};
            if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &format, &text))) return false;
            XrLirInstruction print = {.op=XR_LIR_STRING_PRINT, .type=XR_LIR_UNIT,
                .operands=&text, .operand_count=1, .source=lower_source(c)};
            return lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &print));
        }
        if (xr_xir_operand_type(f->source, value) == XR_XIR_STRING) {
            XrLirValue argument;
            if (!lower_value(f, value, &argument)) return false;
            bool literal = value >= f->source->parameter_count &&
                f->source->instructions[value - f->source->parameter_count].op == XR_XIR_CONST_STRING &&
                f->literal_uses[value - f->source->parameter_count] == 1;
            if (!literal) {
                XrLirInstruction print = {.op=XR_LIR_STRING_PRINT, .type=XR_LIR_UNIT,
                    .operands=&argument, .operand_count=1, .source=lower_source(c)};
                return lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &print));
            }
            const XrXirInstruction *definition = &f->source->instructions[value - f->source->parameter_count];
            XrLirValue args[2] = {{0}};
            XrLirInstruction length = {.op=XR_LIR_CONST, .type=XR_LIR_U64,
                .bits=c->source->declarations->literals[definition->immediate].length};
            if (!lower_value(f, value, &args[0]) ||
                !lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &length, &args[1]))) return false;
            XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_UNIT, .operands=args,
                .operand_count=2, .callee=c->print_string, .source=lower_source(c)};
            return lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &call));
        }
    }
    uint32_t hidden = op->op == XR_XIR_PRINT ? 0 : lower_hidden_state(c, (uint32_t)op->immediate);
    if (hidden > UINT32_MAX - op->args[1]) return lower_unsupported(c, "module argument count");
    uint32_t count = op->args[1] + hidden;
    XrLirValue *arguments = lower_alloc(c, count, sizeof(*arguments));
    if (count && !arguments)
        return false;
    bool good = true;
    for (uint32_t a = 0; a < op->args[1]; ++a)
        if (!lower_value(f, f->source->operands[op->args[0] + a], &arguments[a])) {
            good = false;
            break;
        }
    if (good && op->op == XR_XIR_CALL) {
        const XrLirFunctionData *callee = &c->module->functions[c->functions[(uint32_t)op->immediate].id - 1];
        for (uint32_t a = 0; a < op->args[1] && good; ++a)
            if (callee->parameter_types[a] == XR_LIR_ARRAY_REF)
                good = lower_array_borrow(f, b, &arguments[a]);
    }
    if (hidden && good) {
        arguments[op->args[1]] = f->state;
        if (!f->state.id) {
            lower_error(c, XR_LIR_VERIFY_ERROR, "stateful call lacks its execution capability");
            good = false;
        }
    }
    XrLirInstruction call = {
        .op = XR_LIR_CALL,
        .type = lower_type(c, op->type),
        .operands = arguments,
        .operand_count = count,
        .source = lower_source(c),
        .callee = op->op == XR_XIR_PRINT ? c->print : c->functions[(uint32_t) op->immediate]};
    if (good)
        good = lower_build(c, call.type == XR_LIR_UNIT
                                  ? xr_lir_emit_effect(c->module, f->blocks[b], &call)
                                  : xr_lir_emit_value(c->module, f->blocks[b], &call,
                                                      &f->values[f->source->parameter_count + i]));
    xr_free(arguments);
    return good;
}

static bool lower_module_operation(LowerFunction *f, uint32_t b, uint32_t i, bool *handled) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[i];
    uint32_t result = f->source->parameter_count + i;
    *handled = false;
    if (op->op == XR_XIR_SLOT_LOAD || op->op == XR_XIR_SLOT_INIT ||
        op->op == XR_XIR_SLOT_PLACE) {
        *handled = true;
        const XrXirSlot *slot = &c->source->declarations->slots[op->immediate];
        uint32_t ordinal = c->state_slots[op->immediate];
        XrLirValue args[2] = {f->state, {0}};
        XrLirInstruction instruction = {.type = XR_LIR_UNIT, .operands = args,
            .operand_count = 1, .source = lower_source(c)};
        if (op->op != XR_XIR_SLOT_INIT) {
            instruction.op = XR_LIR_STATE_REFERENCE;
            instruction.type = XR_LIR_STATE_REF;
            instruction.bits = ordinal;
            XrLirValue reference = {0};
            if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &instruction, &reference)))
                return false;
            if (slot->mutable || op->op == XR_XIR_SLOT_PLACE) { f->values[result] = reference; return true; }
            instruction.op = XR_LIR_STATE_READ;
            instruction.type = lower_type(c, slot->type);
            instruction.bits = 0;
            args[0] = reference;
            return lower_build(c, instruction.type == XR_LIR_UNIT
                ? xr_lir_emit_effect(c->module, f->blocks[b], &instruction)
                : xr_lir_emit_value(c->module, f->blocks[b], &instruction, &f->values[result]));
        }
        if (slot->mutable) {
            instruction.op = XR_LIR_STATE_PUBLISH;
            if (!lower_value(f, op->args[0], &args[0])) return false;
        } else {
            instruction.op = XR_LIR_STATE_INITIALIZE;
            instruction.bits = ordinal;
            if (slot->type != XR_XIR_UNIT) {
                if (!lower_value(f, op->args[0], &args[1])) return false;
                instruction.operand_count = 2;
            }
        }
        return lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &instruction));
    }
    bool create = op->op == XR_XIR_CELL_NEW;
    if (!create && op->op != XR_XIR_CELL_READ && op->op != XR_XIR_CELL_WRITE) return true;
    uint32_t slot = lower_module_cell(c, c->function, create ? result : op->args[0]);
    bool managed = lower_managed_storage(f, op);
    if (slot == UINT32_MAX && !managed) return true;
    *handled = true;
    XrLirValue args[2] = {{0}};
    XrLirInstruction instruction = {.type = lower_type(c, op->type), .operands = args,
        .operand_count = op->op == XR_XIR_CELL_WRITE ? 2u : 1u, .source = lower_source(c)};
    if (slot != UINT32_MAX) {
        XrXirType payload = xr_xir_cell_element(c->source->types,
            c->source->declarations->slots[slot].type);
        if (create) {
            instruction.op = XR_LIR_STATE_CONSTRUCT;
            instruction.type = XR_LIR_STATE_REF;
            instruction.bits = c->state_slots[slot];
            args[0] = f->state;
            if (payload != XR_XIR_UNIT) {
                if (!lower_value(f, op->args[0], &args[1])) return false;
                instruction.operand_count = 2;
            }
        } else {
            instruction.op = op->op == XR_XIR_CELL_READ ? XR_LIR_STATE_READ : XR_LIR_STATE_WRITE;
            if (!lower_value(f, op->args[0], &args[0])) return false;
            if (op->op == XR_XIR_CELL_WRITE && payload != XR_XIR_UNIT &&
                !lower_value(f, op->args[1], &args[1])) return false;
            if (payload == XR_XIR_UNIT) instruction.operand_count = 1;
        }
    } else {
        instruction.op = create ? XR_LIR_CELL_NEW :
            op->op == XR_XIR_CELL_READ ? XR_LIR_CELL_READ : XR_LIR_CELL_WRITE;
        if (create) instruction.type = lower_array_element(c,
            xr_xir_cell_element(c->source->types, op->type)) ? XR_LIR_ARRAY_CELL : XR_LIR_STRING_CELL;
        if (!lower_value(f, op->args[0], &args[0]) ||
            (op->op == XR_XIR_CELL_WRITE && !lower_value(f, op->args[1], &args[1]))) return false;
        if (!create && c->module->functions[f->function.id - 1].values[args[0].id - 1].type == XR_LIR_ARRAY_REF)
            instruction.op = op->op == XR_XIR_CELL_READ ? XR_LIR_ARRAY_REF_READ : XR_LIR_ARRAY_REF_WRITE;
    }
    return lower_build(c, instruction.type == XR_LIR_UNIT
        ? xr_lir_emit_effect(c->module, f->blocks[b], &instruction)
        : xr_lir_emit_value(c->module, f->blocks[b], &instruction, &f->values[result]));
}

static bool lower_managed_copy(LowerFunction *, uint32_t, XrLirValue, XrLirValue *);
#include "xlir_lower_array.inc.c"

static bool lower_body(LowerFunction *f, uint32_t b) {
    Lower *c = f->owner;
    XR_DCHECK(f->reachable[b] && f->blocks[b].id, "only created reachable blocks receive bodies");
    const XrXirBlock *block = &f->source->blocks[b];
    c->block = b;
    uint32_t position = f->phis[b];
    if (f->cell_count)
        memset(f->current, 0, (size_t) f->cell_count * sizeof(*f->current));
    for (uint32_t cell = 0; cell < f->cell_count; ++cell)
        if (f->live[(size_t) b * f->cell_count + cell] &&
            !lower_build(
                c, xr_lir_block_parameter(c->module, f->blocks[b], position++, &f->current[cell])))
            return false;
    for (uint32_t p = 0; p < f->phis[b]; ++p) {
        uint32_t value = f->source->parameter_count + block->first + p;
        if (f->array_values && f->array_values[value] != UINT32_MAX)
            f->current[f->array_values[value]] = f->values[value];
    }
    for (uint32_t i = block->first + f->phis[b]; i < block->first + block->count - 1; ++i) {
        c->instruction = i;
        const XrXirInstruction *op = &f->source->instructions[i];
        uint32_t result = f->source->parameter_count + i;
        bool handled;
        if (!lower_module_operation(f, b, i, &handled)) return false;
        if (!handled && !lower_array_instruction(f, b, i, &handled)) return false;
        if (!handled && (lower_state_new(op->op) || lower_state_read(op->op) || lower_state_write(op->op))) {
            uint32_t cell = 0;
            if (!lower_cell_index(f, lower_state_new(op->op) ? result : op->args[0], &cell))
                return false;
            if (lower_state_read(op->op)) {
                if (!f->current[cell].id) {
                    lower_error(c, XR_LIR_VERIFY_ERROR, "Cell read has no SSA state");
                    return false;
                }
                if (lower_managed_type(f->cell_types[cell])) {
                    if (!lower_managed_copy(f, b, f->current[cell], &f->values[result])) return false;
                } else f->values[result] = f->current[cell];
            } else {
                XrLirValue input = {0};
                if (!lower_value(f, op->args[lower_state_new(op->op) ? 0 : 1], &input)) return false;
                if (f->cell_types[cell] == XR_LIR_ARRAY) {
                    if (!lower_managed_copy(f, b, input, &f->current[cell])) return false;
                } else f->current[cell] = input;
            }
        } else if (!handled && (op->op == XR_XIR_CALL || op->op == XR_XIR_PRINT)) {
            if (!lower_call(f, b, i)) return false;
        } else if (!handled && !lower_emit_scalar(f, b, i)) return false;
        if (f->array_values && f->array_values[result] != UINT32_MAX) {
            uint32_t cell = f->array_values[result];
            f->current[cell] = f->values[result];
        }
    }
    if (f->cell_count)
        memcpy(&f->states[(size_t) b * f->cell_count], f->current,
               (size_t) f->cell_count * sizeof(*f->current));
    return true;
}

static bool lower_edge(LowerFunction *f, uint32_t from, uint32_t target, XrLirEdge *edge) {
    Lower *c = f->owner;
    const XrXirBlock *block = &f->source->blocks[target];
    uint32_t count = f->phis[target];
    for (uint32_t cell = 0; cell < f->cell_count; ++cell)
        if (f->live[(size_t) target * f->cell_count + cell])
            ++count;
    XrLirValue *arguments = lower_alloc(c, count, sizeof(*arguments));
    if (count && !arguments)
        return false;
    *edge = (XrLirEdge) {f->blocks[target], arguments, count};
    for (uint32_t p = 0; p < f->phis[target]; ++p) {
        const XrXirInstruction *phi = &f->source->instructions[block->first + p];
        if (phi->args[0] > f->source->operand_count ||
            phi->args[1] > f->source->operand_count - phi->args[0]) {
            lower_error(c, XR_LIR_VERIFY_ERROR, "invalid Checked PHI range");
            return false;
        }
        bool found = false;
        for (uint32_t a = 0; a + 1 < phi->args[1]; a += 2)
            if (f->source->operands[phi->args[0] + a] == from) {
                if (!lower_value(f, f->source->operands[phi->args[0] + a + 1], &arguments[p]))
                    return false;
                found = true;
                break;
            }
        if (!found) {
            lower_error(c, XR_LIR_VERIFY_ERROR, "Checked PHI has no incoming edge value");
            return false;
        }
    }
    uint32_t position = f->phis[target];
    for (uint32_t cell = 0; cell < f->cell_count; ++cell)
        if (f->live[(size_t) target * f->cell_count + cell]) {
            XrLirValue state = f->states[(size_t) from * f->cell_count + cell];
            if (!state.id) {
                lower_error(c, XR_LIR_VERIFY_ERROR, "Cell edge has no SSA state");
                return false;
            }
            arguments[position++] = state;
        }
    XR_DCHECK(position == count, "edge argument order matches target parameters");
    if (f->next[target] <= f->next[from])
        for (uint32_t a = 0; a < count; ++a) {
            XrLirType type = c->module->functions[f->function.id - 1].values[arguments[a].id - 1].type;
            if (type == XR_LIR_STRING || type == XR_LIR_STRING_CELL || type == XR_LIR_ARRAY_CELL)
                return lower_unsupported(c, "loop-carried managed String transfer");
        }
    return true;
}
#include "xlir_lower_array_cells.inc.c"
#include "xlir_lower_array_edges.inc.c"
static bool lower_managed_copy(LowerFunction *f, uint32_t block, XrLirValue input, XrLirValue *output) {
    XrLirType type = f->owner->module->functions[f->function.id - 1].values[input.id - 1].type;
    XrLirInstruction copy = {.op=type == XR_LIR_ARRAY ? XR_LIR_ARRAY_RETAIN : XR_LIR_STRING_RETAIN, .type=type,
        .operands=&input, .operand_count=1, .source=lower_source(f->owner)};
    return lower_build(f->owner, xr_lir_emit_value(f->owner->module, f->blocks[block], &copy, output));
}
static bool lower_managed_kept(const XrLirInstruction *term, uint32_t id) {
    if (term->op == XR_LIR_RET) return term->operand_count && term->operands[0].id == id;
    for (uint32_t e = 0; e < term->edge_count; ++e)
        for (uint32_t a = 0; a < term->edges[e].argument_count; ++a)
            if (term->edges[e].arguments[a].id == id) return true;
    return false;
}
static bool lower_managed_release(LowerFunction *f, uint32_t block,
    XrLirValue value, const XrLirInstruction *term) {
    XrLirType type = f->owner->module->functions[f->function.id - 1].values[value.id - 1].type;
    if (!lower_managed_type(type) || lower_managed_kept(term, value.id) ||
        (value.id <= f->retired_value_count && f->retired_values && f->retired_values[value.id - 1]))
        return true;
    XrLirInstruction drop = {.op=type == XR_LIR_ARRAY ? XR_LIR_ARRAY_DROP : XR_LIR_STRING_DROP,
        .type=XR_LIR_UNIT, .operands=&value, .operand_count=1, .source=lower_source(f->owner)};
    return lower_build(f->owner, xr_lir_emit_effect(f->owner->module, f->blocks[block], &drop));
}
static bool lower_managed_release_block(LowerFunction *f, uint32_t block, const XrLirInstruction *term) {
    XrLirFunctionData *function = &f->owner->module->functions[f->function.id - 1];
    uint32_t block_id = f->blocks[block].id - 1;
    uint32_t parameters = function->blocks[block_id].parameter_count;
    for (uint32_t p = 0; p < parameters; ++p) {
        XrLirValue value = {f->owner->module, f->function.id, function->blocks[block_id].parameters[p]};
        if (!lower_managed_release(f, block, value, term)) return false;
    }
    uint32_t count = function->blocks[block_id].instruction_count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t result = function->blocks[block_id].instructions[i].view.result;
        if (result) {
            XrLirValue value = {f->owner->module, f->function.id, result};
            if (!lower_managed_release(f, block, value, term)) return false;
        }
    }
    if (term->op == XR_LIR_RET)
        for (uint32_t i = 0; i < f->frame_cell_count; ++i) {
            XrLirInstruction drop = {.op=XR_LIR_STRING_CELL_DROP, .type=XR_LIR_UNIT,
                .operands=&f->frame_cells[i], .operand_count=1, .source=lower_source(f->owner)};
            if (!lower_build(f->owner, xr_lir_emit_effect(f->owner->module, f->blocks[block], &drop)))
                return false;
        }
    return lower_array_cells_release(f, block, term, UINT32_MAX, f->blocks[block]);
}

/* Explicit temporary drops remain local. String Cells keep their frame rule;
 * physical Array Cells retire according to actual successor demand. */
static bool lower_frame_cells(LowerFunction *f) {
    XrLirFunctionData *function = &f->owner->module->functions[f->function.id - 1];
    const XrLirBlockData *entry = &function->blocks[function->entry - 1];
    uint8_t *released = lower_alloc(f->owner, function->value_count, 1);
    f->cell_retired = released;
    if (function->value_count && !released) return false;
    if (f->array_values) {
        f->retired_value_count = function->value_count;
        f->retired_values = lower_alloc(f->owner, f->retired_value_count, 1);
        if (f->retired_value_count && !f->retired_values) return false;
    }
    for (uint32_t b = 0; b < function->block_count; ++b)
        for (uint32_t i = 0; i < function->blocks[b].instruction_count; ++i) {
            const XrLirInstructionView *v = &function->blocks[b].instructions[i].view;
            if (v->op == XR_LIR_ARRAY_CELL_DROP) released[v->operands[0] - 1] = 1;
            if (f->retired_values && (v->op == XR_LIR_ARRAY_DROP || v->op == XR_LIR_STRING_DROP))
                f->retired_values[v->operands[0] - 1] = 1;
        }
    f->frame_cells = lower_alloc(f->owner, entry->instruction_count, sizeof(*f->frame_cells));
    if (entry->instruction_count && !f->frame_cells) return false;
    for (uint32_t i = 0; i < entry->instruction_count; ++i) {
        const XrLirInstructionView *v = &entry->instructions[i].view;
        if (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_STRING_CELL)
            f->frame_cells[f->frame_cell_count++] = (XrLirValue){f->owner->module, f->function.id, v->result};
    }
    return lower_array_cells_inventory(f);
}
static bool lower_terminator(LowerFunction *f, uint32_t b) {
    Lower *c = f->owner;
    const XrXirBlock *block = &f->source->blocks[b];
    c->block = b;
    if (f->cell_count)
        memcpy(f->current, &f->states[(size_t)b * f->cell_count],
            (size_t)f->cell_count * sizeof(*f->current));
    c->instruction = block->first + block->count - 1;
    const XrXirInstruction *term = lower_term(c, f->source, b);
    if (!term)
        return false;
    XR_DCHECK(term->op == XR_XIR_RETURN || term->op == XR_XIR_JUMP || term->op == XR_XIR_BRANCH,
              "admission rejected unsupported terminators");
    XrLirValue value = {0};
    XrLirInstruction instruction = {.type = XR_LIR_UNIT, .source = lower_source(c)};
    bool good = true;
    if (term->op == XR_XIR_RETURN) {
        instruction.op = XR_LIR_RET;
        if (f->source->result != XR_XIR_UNIT) {
            good = lower_value(f, term->args[0], &value);
            if (good && lower_managed_type(lower_type(c, f->source->result))) {
                XrLirValue result = {0};
                good = lower_managed_copy(f, b, value, &result);
                value = result;
            }
            instruction.operands = &value;
            instruction.operand_count = 1;
        }
    } else {
        instruction.op = term->op == XR_XIR_JUMP ? XR_LIR_BR : XR_LIR_CONDBR;
        if (term->op == XR_XIR_BRANCH) {
            good = lower_value(f, term->args[0], &value);
            instruction.operands = &value;
            instruction.operand_count = 1;
        }
        instruction.edge_count = lower_edge_count(term);
        for (uint32_t e = 0; e < instruction.edge_count && good; ++e)
            good = lower_edge(f, b, term->targets[e], &instruction.edges[e]);
    }
    if (good) good = lower_selected_edges(f, b, &instruction);
    if (good) good = lower_managed_release_block(f, b, &instruction);
    if (good)
        good = lower_build(c, xr_lir_set_terminator(c->module, f->blocks[b], &instruction));
    for (uint32_t e = 0; e < 2; ++e)
        xr_free((void *) instruction.edges[e].arguments);
    return good;
}

static bool lower_function(Lower *c, uint32_t index) {
    c->function = index;
    c->block = 0;
    c->instruction = UINT32_MAX;
    LowerFunction f = {
        .owner = c, .source = &c->source->functions[index], .function = c->functions[index]};
    f.literal_uses = lower_literal_uses(c, f.source);
    bool good = f.literal_uses && lower_build(c, xr_lir_define_function(c->module, f.function)) && lower_order(&f) &&
                lower_cell_inventory(&f) && lower_liveness(&f) && lower_blocks(&f);
    for (uint32_t p = 0; p < f.source->parameter_count && good; ++p)
        good = lower_build(c, xr_lir_function_parameter(c->module, f.function, p, &f.values[p]));
    for (uint32_t p = 0; p < f.source->parameter_count && good; ++p)
        if (f.source->parameters[p] == XR_XIR_STRING) {
            XrLirValue retained = {0};
            good = lower_managed_copy(&f, 0, f.values[p], &retained);
            f.values[p] = retained;
        }
    if (lower_hidden_state(c, index) && good)
        good = lower_build(c, xr_lir_function_parameter(c->module, f.function,
            f.source->parameter_count, &f.state));
    for (uint32_t n = 0; n < f.ordered && good; ++n)
        good = lower_body(&f, f.order[n]);
    if (good) good = lower_frame_cells(&f);
    for (uint32_t b = 0; b < f.source->block_count && good; ++b)
        if (f.reachable[b])
            good = lower_terminator(&f, b);
    lower_function_free(&f);
    return good;
}

static bool lower_wrapper(Lower *c, XrLirFunction *output) {
    const XrXirDeclarations *d = c->source->declarations;
    uint32_t entry = d->entry_function;
    XrLirType capability = XR_LIR_STATE;
    bool stateful = c->state_layout.slot_count != 0;
    XrLirFunctionSpec spec = {.name = "main", .name_length = 4,
        .parameters = stateful ? &capability : NULL, .parameter_count = stateful ? 1u : 0u,
        .result = lower_type(c, c->source->functions[entry].result)};
    XrLirFunction wrapper = {0};
    XrLirBlock block = {0};
    XrLirValue result = {0}, state = {0};
    if (!lower_build(c, xr_lir_declare_function(c->module, &spec, &wrapper)) ||
        (stateful && !lower_build(c, xr_lir_set_state_entry(c->module, wrapper))) ||
        !lower_build(c, xr_lir_define_function(c->module, wrapper)) ||
        !lower_build(c, xr_lir_add_block(c->module, wrapper, NULL, 0, &block)) ||
        !lower_build(c, xr_lir_set_entry(c->module, wrapper, block)) ||
        (stateful && !lower_build(c, xr_lir_function_parameter(c->module, wrapper, 0, &state))))
        return false;
    for (uint32_t i = 0; i <= c->initialization_count; ++i) {
        bool initializer = i < c->initialization_count;
        uint32_t module = initializer ? c->module_order[i] : d->root_module;
        uint32_t selected = initializer ? d->modules[module].initializer : entry;
        if (c->source->functions[selected].parameter_count)
            return lower_unsupported(c, "module execution root parameters");
        XrLirInstruction boundary = {.op = XR_LIR_MODULE_BEGIN, .type = XR_LIR_UNIT,
            .operands = &state, .operand_count = 1, .bits = module};
        if (stateful && initializer &&
            !lower_build(c, xr_lir_emit_effect(c->module, block, &boundary))) return false;
        XrLirInstruction call = {.op = XR_LIR_CALL,
            .type = lower_type(c, c->source->functions[selected].result),
            .callee = c->functions[selected], .operands = &state,
            .operand_count = lower_hidden_state(c, selected)};
        if (!lower_build(c, call.type == XR_LIR_UNIT
            ? xr_lir_emit_effect(c->module, block, &call)
            : xr_lir_emit_value(c->module, block, &call, &result))) return false;
        boundary.op = XR_LIR_MODULE_READY;
        if (stateful && initializer &&
            !lower_build(c, xr_lir_emit_effect(c->module, block, &boundary))) return false;
    }
    XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = spec.result == XR_LIR_UNIT ? NULL : &result,
        .operand_count = spec.result == XR_LIR_UNIT ? 0u : 1u};
    if (!lower_build(c, xr_lir_set_terminator(c->module, block, &ret))) return false;
    *output = wrapper;
    return true;
}

/* Copy bindings above retain unconditionally and blocks drop every remaining
 * owner. The published module shares one reference wherever a copy or its
 * source would otherwise only be dropped; the entry handle follows it. */
static bool lower_elide(Lower *c, XrLirFunction *entry) {
    XrLirModule *elided = NULL;
    XrLirStatus status = xr_lir_string_elide(c->module, &elided);
    if (status != XR_LIR_OK) {
        c->instruction = UINT32_MAX;
        lower_error(c, status, "String reference elision failed");
        return false;
    }
    XR_DCHECK(entry->module == c->module, "the entry belongs to the elided module");
    xr_lir_module_destroy(c->module);
    c->module = elided;
    entry->module = elided;
    return true;
}

XR_FUNC void xr_lir_lowered_free(XrLirLowered *result) {
    if (!result)
        return;
    xr_lir_module_destroy(result->module);
    *result = (XrLirLowered) {0};
}
XR_FUNC void xr_lir_lower_failure_free(XrLirLowerFailure *failure) {
    if (!failure)
        return;
    xr_free(failure->owned_path);
    *failure = (XrLirLowerFailure) {0};
}

static XrLirStatus lower_checked(const XrLirLowerRequest *request, uint32_t selected, bool local,
                               XrLirLowered *output, XrLirLowerFailure *failure) {
    if (!request || !request->closed || !request->snapshot || !output || output->module ||
        output->entry.module || output->entry.id || (failure && failure->owned_path))
        return XR_LIR_INVALID_ARGUMENT;
    if (failure)
        *failure = (XrLirLowerFailure) {0};
    Lower c = {.request = request,
               .source = xr_xir_compile_artifact_module(request->closed),
               .failure = failure,
               .instruction = UINT32_MAX};
    const XrXirModule *source = c.source;
    if (!source || source->stage != XR_XIR_CHECKED || source->linkage_kind != XR_XIR_PROGRAM ||
        !source->declarations)
        return lower_error(&c, XR_LIR_INVALID_ARGUMENT,
                           "source-backed Closed Checked program required");
    const XrXirDeclarations *d = source->declarations;
    c.function = d->modules && d->module_count ? d->modules[0].initializer : d->entry_function;
    if (request->overflow_checks)
        return lower_unsupported(&c, "overflow-checks"), c.status;
    if (!d->module_count || !d->modules || d->root_module >= d->module_count ||
        (local && (d->module_count != 1 || d->root_module != 0 || d->slot_count ||
                   d->modules[0].dependency_count)))
        return lower_unsupported(&c, "module imports, slots or receiving storage"), c.status;
    if (d->entry_function >= source->function_count ||
        d->modules[0].initializer >= source->function_count)
        return lower_error(&c, XR_LIR_VERIFY_ERROR, "invalid Checked program entry");
    if (!lower_build(&c, xr_lir_module_create(NULL, &c.module))) return c.status;
    if (!lower_modules(&c) || !lower_state_layout(&c) || !lower_state_constructors(&c)) {
        lower_modules_free(&c);
        xr_lir_module_destroy(c.module);
        return c.status;
    }
    c.functions = lower_alloc(&c, source->function_count, sizeof(*c.functions));
    c.active = lower_alloc(&c, source->function_count, 1);
    c.queue = lower_alloc(&c, source->function_count, sizeof(*c.queue));
    bool good = c.functions && c.active && c.queue;
    for (uint32_t m = 0; m < c.initialization_count && good; ++m)
        good = lower_mark(&c, d->modules[c.module_order[m]].initializer);
    if (good) good = lower_mark(&c, d->entry_function);
    for (uint32_t at = 0; at < c.queued && good; ++at)
        good = lower_admit_function(&c, c.queue[at]);
    if (good && local)
        good = lower_local_pure(&c, selected);
    for (uint32_t s = 0; s < c.state_layout.slot_count; ++s)
        c.arrays |= c.state_layout.slots[s].payload == XR_LIR_ARRAY;
    if (good && c.arrays)
        good = lower_build(&c, xr_lir_array_catalog_install(c.module));
    if (good && c.state_layout.slot_count)
        good = lower_build(&c, xr_lir_set_state_layout(c.module, &c.state_layout));
    if (good && c.prints) {
        XrLirType integer = XR_LIR_I64;
        XrLirFunctionSpec spec = {.name = "xrt_print_i64",
                                  .name_length = 13,
                                  .parameters = &integer,
                                  .parameter_count = 1,
                                  .result = XR_LIR_UNIT,
                                  .kind = XR_LIR_FUNCTION_EXTERNAL,
                                  .extern_kind = XR_LIR_EXTERN_PRINT_I64};
        good = lower_build(&c, xr_lir_declare_function(c.module, &spec, &c.print));
    }
    if (good && c.string_prints) {
        XrLirType parameters[] = {XR_LIR_PTR, XR_LIR_U64};
        XrLirFunctionSpec spec = {.name="xrt_print_string", .name_length=16, .parameters=parameters,
            .parameter_count=2, .result=XR_LIR_UNIT, .kind=XR_LIR_FUNCTION_EXTERNAL,
            .extern_kind=XR_LIR_EXTERN_PRINT_STRING};
        good = lower_build(&c, xr_lir_declare_function(c.module, &spec, &c.print_string));
    }
    static const char *clock_names[] = {"xrt_clock_realtime_nanos",
        "xrt_clock_process_cpu_nanos", "xrt_clock_monotonic_nanos", "xrt_clock_utc_offset_at"};
    for (uint32_t kind=0; kind<4 && good; ++kind) if (c.clock_kinds & (1u << kind)) {
        const XrLirType seconds = XR_LIR_I64;
        XrLirFunctionSpec spec = {.name=clock_names[kind], .name_length=strlen(clock_names[kind]),
            .parameters=kind == 3 ? &seconds : NULL, .parameter_count=kind == 3 ? 1u : 0u,
            .result=XR_LIR_I64, .kind=XR_LIR_FUNCTION_EXTERNAL,
            .extern_kind=(XrLirExternKind)(XR_LIR_EXTERN_CLOCK_REALTIME + kind)};
        good = lower_build(&c, xr_lir_declare_function(c.module, &spec, &c.clocks[kind]));
    }
    for (uint32_t f = 0; f < source->function_count && good; ++f)
        if (c.active[f])
            good = lower_declare(&c, f);
    for (uint32_t f = 0; f < source->function_count && good; ++f)
        if (c.active[f])
            good = lower_function(&c, f);
    XrLirFunction entry = {0};
    if (good && local)
        entry = c.functions[selected];
    else if (good)
        good = lower_wrapper(&c, &entry);
    if (good)
        good = lower_build(&c, xr_lir_module_finish(c.module, NULL));
    if (good)
        good = lower_elide(&c, &entry);
    if (good) {
        XR_DCHECK(c.status == XR_LIR_OK, "only a complete successful module is published");
        *output = (XrLirLowered) {c.module, entry};
        c.module = NULL;
    }
    xr_lir_module_destroy(c.module);
    xr_free(c.functions);
    xr_free(c.active);
    xr_free(c.queue);
    lower_modules_free(&c);
    return good ? XR_LIR_OK : (c.status == XR_LIR_OK ? XR_LIR_VERIFY_ERROR : c.status);
}

XR_FUNC XrLirStatus xr_lir_lower_program_checked(const XrLirLowerRequest *request,
                                                 XrLirLowered *output, XrLirLowerFailure *failure) {
    return lower_checked(request, 0, false, output, failure);
}
XR_FUNC XrLirStatus xr_lir_lower_local_checked(const XrLirLowerRequest *request, uint32_t selected,
                                               XrLirLowered *output, XrLirLowerFailure *failure) {
    return lower_checked(request, selected, true, output, failure);
}
