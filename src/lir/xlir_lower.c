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
    XrLirFunction *functions, print, print_string, clocks[4];
    uint8_t *active;
    uint32_t *queue, queued;
    uint32_t function, block, instruction;
    XrLirStatus status;
    XrLirLowerFailure *failure;
    uint32_t *module_order, *initializer_owner, *last_publication;
    uint8_t *active_modules;
    uint32_t initialization_count;
    struct LowerLiteral *literals;
    uint32_t *module_slot_for_value, *module_publications;
    uint8_t *storage_bad;
    uint32_t module_value_count;
    bool prints, string_prints, mutable_slots, storage_ready;
    uint8_t clock_kinds;
} Lower;
typedef struct LowerLiteral {
    uint64_t bits;
    uint32_t publication;
} LowerLiteral;
typedef struct LowerFunction {
    Lower *owner;
    const XrXirFunction *source;
    XrLirFunction function;
    XrLirBlock *blocks;
    XrLirValue *values, *states, *current, *slots, *frame_cells;
    XrLirType *cell_types;
    uint32_t *cell_for, *phis, *order, *stack, *next;
    uint8_t *reachable, *use, *define, *live;
    uint8_t *literal_uses;
    uint32_t value_count, cell_count, ordered, frame_cell_count;
} LowerFunction;

static XrLirType lower_type(XrXirType type) {
    if (type == XR_XIR_UNIT)
        return XR_LIR_UNIT;
    if (type == XR_XIR_I64)
        return XR_LIR_I64;
    if (type == XR_XIR_BOOL)
        return XR_LIR_I1;
    if (type == XR_XIR_F64)
        return XR_LIR_F64;
    if (type == XR_XIR_STRING)
        return XR_LIR_STRING;
    return XR_LIR_TYPE_INVALID;
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

static bool lower_known(XrXirOp op) {
    switch (op) {
        case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT:
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

static bool lower_storage_body(const XrXirFunction *f) {
    if (!lower_type(f->result) || !f->block_count || !f->blocks ||
        !f->instruction_count || !f->instructions ||
        (f->parameter_count && !f->parameters) || (f->operand_count && !f->operands)) return false;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (lower_type(f->parameters[p]) <= XR_LIR_UNIT) return false;
    for (uint32_t b = 0; b < f->block_count; ++b)
        if (f->blocks[b].first > f->instruction_count ||
            f->blocks[b].count > f->instruction_count - f->blocks[b].first) return false;
    return true;
}

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

/* BAD is reachability to a storage or unknown body, not impurity. A complete
 * recursive component without a BAD successor is storage-free. Reverse edges
 * visit each call once; the cache becomes readable only after convergence. */
static bool lower_storage_closures(Lower *c) {
    uint32_t count = c->source->function_count;
    size_t *heads = lower_alloc(c, count, sizeof(*heads));
    uint32_t *queue = lower_alloc(c, count, sizeof(*queue));
    LowerStorageCall *calls = NULL;
    size_t edges = 0, queued = 0;
    c->storage_bad = lower_alloc(c, count, 1);
    bool good = heads && queue && c->storage_bad;
    if (!good) goto done;
    for (uint32_t f = 0; f < count; ++f) {
        heads[f] = SIZE_MAX;
        const XrXirFunction *body = &c->source->functions[f];
        c->storage_bad[f] = !lower_storage_body(body);
        if (!body->instructions) continue;
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            bool bad = !lower_known(op->op) || !lower_type(op->type);
            switch (op->op) {
            case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_LOAD:
            case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE:
            case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_LOCAL_WRITE:
                bad = true; break;
            case XR_XIR_CALL: {
                uint32_t target;
                if (!lower_storage_call(c, body, op, &target)) bad = true;
                else if (edges == SIZE_MAX) {
                    lower_error(c, XR_LIR_OUT_OF_MEMORY, "module call edge count overflow");
                    good = false; goto done;
                } else ++edges;
                break;
            }
            default: break;
            }
            c->storage_bad[f] |= bad;
        }
    }
    calls = lower_alloc(c, edges, sizeof(*calls));
    if (edges && !calls) { good = false; goto done; }
    size_t at = 0;
    for (uint32_t f = 0; f < count; ++f) {
        const XrXirFunction *body = &c->source->functions[f];
        if (c->storage_bad[f]) queue[queued++] = f;
        if (!body->instructions) continue;
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            uint32_t target;
            if (op->op != XR_XIR_CALL || !lower_storage_call(c, body, op, &target)) continue;
            if (at >= edges) {
                lower_error(c, XR_LIR_VERIFY_ERROR, "module call graph changed during reception");
                good = false; goto done;
            }
            calls[at] = (LowerStorageCall){f, heads[target]};
            heads[target] = at++;
        }
    }
    XR_DCHECK(at == edges && queued <= count, "complete graph and unique initial BAD functions");
    for (size_t cursor = 0; cursor < queued; ++cursor)
        for (size_t edge = heads[queue[cursor]]; edge != SIZE_MAX; edge = calls[edge].next) {
            uint32_t caller = calls[edge].caller;
            if (c->storage_bad[caller]) continue;
            XR_DCHECK(queued < count, "each function becomes BAD once");
            c->storage_bad[caller] = 1;
            queue[queued++] = caller;
        }
    c->storage_ready = true;
done:
    xr_free(calls); xr_free(queue); xr_free(heads);
    return good;
}

static bool lower_storage_free(const Lower *c, uint32_t index) {
    return c->storage_ready && c->storage_bad && index < c->source->function_count &&
        !c->storage_bad[index];
}
/* Explicit storage parameters keep their original physical signature unless
 * the complete current direct-call closure proves no storage requirement. */
static uint32_t lower_hidden_cells(const Lower *c, uint32_t function) {
    return c->mutable_slots && function != c->source->declarations->modules[0].initializer &&
        !lower_storage_free(c, function) ? c->source->declarations->slot_count : 0;
}

/* Only actual owned module Cells become activation-local physical storage.
 * Slot ordinals retain their original declaration identity throughout lowering. */
static uint32_t lower_module_cell(const Lower *c, uint32_t function, uint32_t value) {
    if (!c->mutable_slots) return UINT32_MAX;
    const XrXirDeclarations *d = c->source->declarations;
    const XrXirFunction *f = &c->source->functions[function];
    if (function == d->modules[0].initializer && value < c->module_value_count &&
        c->module_slot_for_value[value] != UINT32_MAX)
        return c->module_slot_for_value[value];
    if (value >= f->parameter_count && value - f->parameter_count < f->instruction_count) {
        const XrXirInstruction *op = &f->instructions[value - f->parameter_count];
        if (op->op == XR_XIR_SLOT_LOAD && op->immediate >= 0 &&
            (uint64_t)op->immediate < d->slot_count) return (uint32_t)op->immediate;
    }
    return UINT32_MAX;
}
static bool lower_mutable_slots(Lower *c) {
    const XrXirDeclarations *d = c->source->declarations;
    uint32_t init = d->modules[0].initializer;
    if (!d->functions || d->entry_function == init || c->source->functions[init].parameter_count ||
        c->source->functions[d->entry_function].parameter_count)
        return lower_unsupported(c, "module execution roots");
    for (uint32_t s = 0; s < d->slot_count; ++s)
        if (d->slots[s].module != 0 || !d->slots[s].mutable ||
            !xr_xir_type_is_cell(c->source->types, d->slots[s].type) ||
            (xr_xir_cell_element(c->source->types, d->slots[s].type) != XR_XIR_I64 &&
             xr_xir_cell_element(c->source->types, d->slots[s].type) != XR_XIR_STRING))
            return lower_unsupported(c, "module slots other than mutable i64 or String Cells");
    if (!lower_storage_closures(c)) return false;
    c->module_value_count = c->source->functions[init].instruction_count;
    c->module_slot_for_value = lower_alloc(c, c->module_value_count,
                                           sizeof(*c->module_slot_for_value));
    c->module_publications = lower_alloc(c, d->slot_count, sizeof(*c->module_publications));
    if (!c->module_slot_for_value || !c->module_publications) return false;
    for (uint32_t v = 0; v < c->module_value_count; ++v)
        c->module_slot_for_value[v] = UINT32_MAX;
    for (uint32_t s = 0; s < d->slot_count; ++s)
        c->module_publications[s] = UINT32_MAX;
    uint32_t last = 0;
    for (uint32_t f = 0; f < c->source->function_count; ++f) {
        const XrXirFunction *function = &c->source->functions[f];
        for (uint32_t b = 0; b < function->block_count; ++b) {
            const XrXirBlock *block = &function->blocks[b];
            for (uint32_t i = block->first; i < block->first + block->count; ++i) {
                const XrXirInstruction *op = &function->instructions[i];
                c->function=f; c->block=b; c->instruction=i;
                if (op->op == XR_XIR_CALL && op->immediate == (int64_t)init)
                    return lower_unsupported(c, "call back to module initializer");
                if (op->op == XR_XIR_SLOT_STORE || op->op == XR_XIR_SLOT_PLACE ||
                    op->op == XR_XIR_SLOT_GROUP_INIT)
                    return lower_unsupported(c, "module slot replacement, address or group");
                if (op->op != XR_XIR_SLOT_INIT && op->op != XR_XIR_SLOT_LOAD) continue;
                if (op->immediate < 0 || (uint64_t)op->immediate >= d->slot_count ||
                    d->functions[f].module != d->slots[op->immediate].module)
                    return lower_unsupported(c, "module slot authority");
                uint32_t s = (uint32_t)op->immediate;
                if (op->op == XR_XIR_SLOT_LOAD) {
                    if (op->type != d->slots[s].type)
                        return lower_unsupported(c, "module slot Cell type");
                    continue;
                }
                if (f != init || b != 0 || c->module_publications[s] != UINT32_MAX ||
                    op->args[0] < function->parameter_count)
                    return lower_unsupported(c, "module Cell publication outside initializer entry");
                uint32_t at = op->args[0] - function->parameter_count;
                if (at < block->first || at >= i ||
                    function->instructions[at].op != XR_XIR_CELL_NEW ||
                    function->instructions[at].type != d->slots[s].type)
                    return lower_unsupported(c, "module Cell publication without its direct constructor");
                if (c->module_slot_for_value[at] != UINT32_MAX)
                    return lower_unsupported(c, "aliased module Cell publication");
                c->module_slot_for_value[at] = s; c->module_publications[s] = i;
                if (i > last) last = i;
            }
        }
    }
    for (uint32_t s = 0; s < d->slot_count; ++s)
        if (c->module_publications[s] == UINT32_MAX)
            return lower_unsupported(c, "module Cell without publication");
    const XrXirFunction *f = &c->source->functions[init];
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            const XrXirInstruction *op = &f->instructions[i];
            c->function=init; c->block=b; c->instruction=i;
            if (!b && op->op == XR_XIR_CALL && i <= last &&
                (op->immediate < 0 || (uint64_t)op->immediate >= c->source->function_count ||
                 !lower_storage_free(c, (uint32_t)op->immediate)))
                return lower_unsupported(c, "module initialization call before complete publication");
            if (!b && op->op == XR_XIR_SLOT_LOAD &&
                i <= c->module_publications[op->immediate])
                return lower_unsupported(c, "module Cell read before publication");
        }
    }
    c->mutable_slots = true;
    return true;
}

static void lower_modules_free(Lower *c) {
    xr_free(c->module_order); xr_free(c->active_modules);
    xr_free(c->initializer_owner); xr_free(c->last_publication); xr_free(c->literals);
    xr_free(c->storage_bad);
}
static bool lower_modules(Lower *c) {
    const XrXirDeclarations *d = c->source->declarations;
    c->module_order = lower_alloc(c, d->module_count, sizeof(*c->module_order));
    c->active_modules = lower_alloc(c, d->module_count, 1);
    c->initializer_owner = lower_alloc(c, c->source->function_count, sizeof(*c->initializer_owner));
    c->last_publication = lower_alloc(c, d->module_count, sizeof(*c->last_publication));
    if (!c->module_order || !c->active_modules || !c->initializer_owner || !c->last_publication)
        return false;
    for (uint32_t f = 0; f < c->source->function_count; ++f) c->initializer_owner[f] = UINT32_MAX;
    for (uint32_t m = 0; m < d->module_count; ++m) {
        uint32_t init = d->modules[m].initializer;
        if (init >= c->source->function_count || c->initializer_owner[init] != UINT32_MAX ||
            d->modules[m].storage != XR_XIR_MODULE_FRESH_STORAGE)
            return lower_unsupported(c, "module initializer or receiving storage");
        c->initializer_owner[init] = m;
        c->last_publication[m] = UINT32_MAX;
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
/* Inspect every original function, including bodies outside the retained
 * roots. Erasing storage must not hide a reader, address, or publication. */
static bool lower_literal(Lower *c) {
    const XrXirDeclarations *d = c->source->declarations;
    if (!d->slot_count) return true;
    if (!d->slots || !d->functions) return lower_unsupported(c, "module slot declarations");
    if (xr_xir_type_is_cell(c->source->types, d->slots[0].type)) {
        if (d->module_count != 1 || d->root_module != 0 || d->modules[0].dependency_count)
            return lower_unsupported(c, "imported mutable module slots");
        return lower_mutable_slots(c);
    }
    c->literals = lower_alloc(c, d->slot_count, sizeof(*c->literals));
    if (!c->literals) return false;
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        c->literals[s].publication = UINT32_MAX;
        if (d->slots[s].module >= d->module_count ||
            (c->active_modules[d->slots[s].module] &&
             (d->slots[s].type != XR_XIR_I64 || d->slots[s].mutable)))
            return lower_unsupported(c, "module slots other than immutable i64 literals");
    }
    for (uint32_t f = 0; f < c->source->function_count; ++f) {
        const XrXirFunction *function = &c->source->functions[f];
        for (uint32_t b = 0; b < function->block_count; ++b) {
            const XrXirBlock *block = &function->blocks[b];
            for (uint32_t i = block->first; i < block->first + block->count; ++i) {
                const XrXirInstruction *op = &function->instructions[i];
                c->function = f; c->block = b; c->instruction = i;
                if (op->op == XR_XIR_CALL && op->immediate >= 0 &&
                    (uint64_t)op->immediate < c->source->function_count &&
                    c->initializer_owner[op->immediate] != UINT32_MAX)
                    return lower_unsupported(c, "call back to module initializer");
                if (op->op == XR_XIR_SLOT_STORE || op->op == XR_XIR_SLOT_PLACE ||
                    op->op == XR_XIR_SLOT_GROUP_INIT)
                    return lower_unsupported(c, "module slot write, address or group");
                if (op->op != XR_XIR_SLOT_INIT) continue;
                if (op->immediate < 0 || (uint64_t)op->immediate >= d->slot_count)
                    return lower_unsupported(c, "module slot initializer");
                uint32_t s = (uint32_t)op->immediate, m = d->slots[s].module;
                if (!c->active_modules[m]) continue;
                if (f != d->modules[m].initializer || b != 0 ||
                    c->literals[s].publication != UINT32_MAX || op->args[0] < function->parameter_count)
                    return lower_unsupported(c, "module slot initializer");
                uint32_t value = op->args[0] - function->parameter_count;
                if (value < block->first || value >= i ||
                    function->instructions[value].op != XR_XIR_CONST_INT ||
                    function->instructions[value].type != XR_XIR_I64)
                    return lower_unsupported(c, "module slot without a direct same-block i64 literal");
                c->literals[s].publication = i;
                c->literals[s].bits = (uint64_t)function->instructions[value].immediate;
                c->last_publication[m] = i;
            }
        }
    }
    for (uint32_t s = 0; s < d->slot_count; ++s)
        if (c->active_modules[d->slots[s].module] && c->literals[s].publication == UINT32_MAX)
            return lower_unsupported(c, "module slot without initialization");
    /* Calls only descend through declared imports or stay in the current
     * module. Once its entry publications finish, every such callee can read
     * only this module or already initialized dependencies. No runtime slot
     * state or inferred ownership is needed for immutable scalar literals. */
    for (uint32_t f = 0; f < c->source->function_count; ++f) {
        const XrXirFunction *function = &c->source->functions[f];
        for (uint32_t b = 0; b < function->block_count; ++b) {
            const XrXirBlock *block = &function->blocks[b];
            for (uint32_t i = block->first; i < block->first + block->count; ++i) {
                const XrXirInstruction *op = &function->instructions[i];
                if (op->op != XR_XIR_SLOT_LOAD) continue;
                c->function = f; c->block = b; c->instruction = i;
                if (op->immediate < 0 || (uint64_t)op->immediate >= d->slot_count ||
                    op->type != XR_XIR_I64 || d->functions[f].module >= d->module_count)
                    return lower_unsupported(c, "module slot read authority");
                uint32_t s = (uint32_t)op->immediate, m = d->slots[s].module;
                uint32_t reader = d->functions[f].module;
                if (!c->active_modules[reader]) continue;
                if (!c->active_modules[m] || !xr_xir_module_imports(d, reader, m))
                    return lower_unsupported(c, "module slot read authority");
                if (f == d->modules[m].initializer &&
                    (b != 0 || i <= c->literals[s].publication))
                    return lower_unsupported(c, "slot read outside its initialized block");
            }
        }
    }
    return true;
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
            for (uint32_t i = f->blocks[b].first; i < f->blocks[b].first + f->blocks[b].count; ++i) {
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
static bool lower_string_storage(const Lower *c, const XrXirFunction *f,
    const XrXirInstruction *op) {
    XrXirType type;
    if (op->op == XR_XIR_CELL_NEW) type = op->type;
    else if (op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_WRITE)
        type = xr_xir_operand_type(f, op->args[0]);
    else return false;
    return xr_xir_type_is_cell(c->source->types, type) &&
        xr_xir_cell_element(c->source->types, type) == XR_XIR_STRING && !lower_promoted_string(c, f, op);
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
        if (op->op == XR_XIR_CALL || op->op == XR_XIR_PRINT || op->op == XR_XIR_PHI) {
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
    if (c->mutable_slots && op->op == XR_XIR_SLOT_LOAD)
        return true;
    if (op->op == XR_XIR_CONST_STRING)
        return lower_literal_valid(c, f, (uint32_t)(op - f->instructions));
    if (op->op == XR_XIR_TO_STRING && xr_xir_operand_type(f, op->args[0]) != XR_XIR_I64 &&
        xr_xir_operand_type(f, op->args[0]) != XR_XIR_F64 &&
        xr_xir_operand_type(f, op->args[0]) != XR_XIR_STRING)
        return lower_unsupported(c, "String conversion other than i64, f64 or String");
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
            lower_type(xr_xir_cell_element(c->source->types, op->type)) <= XR_LIR_UNIT)
            return lower_unsupported(c, "non-scalar Cell");
    } else if (!lower_type(op->type))
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
        if (!c->mutable_slots && c->initializer_owner[c->function] != UINT32_MAX &&
            c->last_publication[owner] != UINT32_MAX &&
            c->instruction <= c->last_publication[owner])
            return lower_unsupported(c, "module call before complete publication");
        return lower_mark(c, callee);
    }
    if (op->op == XR_XIR_PRINT) {
        if (op->args[1] != 1 || op->args[0] >= f->operand_count)
            return lower_unsupported(c, "print other than one supported value");
        uint32_t value = f->operands[op->args[0]];
        XrXirType type = xr_xir_operand_type(f, value);
        if (type == XR_XIR_STRING || type == XR_XIR_F64) {
            c->string_prints = true;
        } else if (type == XR_XIR_I64) c->prints = true;
        else return lower_unsupported(c, "print other than one supported value");
    }
    if (op->op == XR_XIR_CONVERT_NUMBER && xr_xir_operand_type(f, op->args[0]) == XR_XIR_F64 &&
        op->type == XR_XIR_I64)
        return lower_unsupported(c, "floating to integer conversion");
    return true;
}

static bool lower_managed(const Lower *c, const XrXirFunction *f, const uint8_t *literal_uses) {
    bool managed = f->result == XR_XIR_STRING;
    for (uint32_t p = 0; p < f->parameter_count; ++p) managed |= f->parameters[p] == XR_XIR_STRING;
    for (uint32_t i = 0; i < f->instruction_count; ++i) {
        const XrXirInstruction *op = &f->instructions[i];
        managed |= (op->type == XR_XIR_STRING &&
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
    if (c->mutable_slots && c->source->declarations->functions[function].module != 0)
        return lower_unsupported(c, "module function owner");
    if (!lower_type(f->result))
        return lower_unsupported(c, "function result type");
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (lower_type(f->parameters[p]) <= XR_LIR_UNIT)
            return lower_unsupported(c, "function parameter type");
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
    uint32_t hidden = lower_hidden_cells(c, index);
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
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        parameters[p] = lower_type(f->parameters[p]);
    for (uint32_t s = 0; s < hidden; ++s)
        parameters[f->parameter_count + s] =
            xr_xir_cell_element(c->source->types, c->source->declarations->slots[s].type) == XR_XIR_STRING
            ? XR_LIR_STRING_CELL : XR_LIR_I64_CELL;
    XrXirType result = c->mutable_slots && index == c->source->declarations->modules[0].initializer
        ? c->source->functions[c->source->declarations->entry_function].result : f->result;
    XrLirFunctionSpec spec = {.name = name,
                              .name_length = (size_t) prefix + f->name_length,
                              .parameters = parameters,
                              .parameter_count = count,
                              .result = lower_type(result)};
    bool good = lower_build(c, xr_lir_declare_function(c->module, &spec, &c->functions[index]));
    xr_free(name);
    xr_free(parameters);
    return good;
}

static void lower_function_free(LowerFunction *f) {
    xr_free(f->literal_uses);
    xr_free(f->slots);
    xr_free(f->frame_cells);
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
    if ((f->value_count && (!f->values || !f->cell_for)) || !f->cell_types)
        return false;
    for (uint32_t v = 0; v < f->value_count; ++v)
        f->cell_for[v] = UINT32_MAX;
    for (uint32_t b = 0; b < source->block_count; ++b)
        if (f->reachable[b]) {
            const XrXirBlock *block = &source->blocks[b];
            for (uint32_t i = block->first; i < block->first + block->count; ++i) {
                const XrXirInstruction *op = &source->instructions[i];
                if (lower_state_new(op->op) &&
                    !lower_string_storage(c, source, op) &&
                    lower_module_cell(c, c->function, source->parameter_count + i) == UINT32_MAX) {
                    XR_DCHECK(f->cell_count < source->instruction_count,
                              "each state has one defining instruction");
                    uint32_t id = source->parameter_count + i;
                    f->cell_for[id] = f->cell_count;
                    XrXirType element = op->op == XR_XIR_CELL_NEW
                                            ? xr_xir_cell_element(c->source->types, op->type)
                                            : op->type;
                    f->cell_types[f->cell_count++] = lower_type(element);
                }
            }
        }
    return true;
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
                if (lower_string_storage(c, source, op)) continue;
                if (lower_module_cell(c, c->function, lower_state_new(op->op)
                        ? source->parameter_count + i : op->args[0]) != UINT32_MAX) continue;
                if (!lower_state_new(op->op) && !lower_state_read(op->op) &&
                    !lower_state_write(op->op))
                    continue;
                c->block = b;
                c->instruction = i;
                uint32_t cell;
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
                types[p] = lower_type(source->instructions[block->first + p].type);
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
            for (uint32_t p = 0; p < phis; ++p)
                if (!lower_build(c, xr_lir_block_parameter(
                                        c->module, f->blocks[b], p,
                                        &f->values[source->parameter_count + block->first + p])))
                    return false;
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
        case XR_XIR_SLOT_LOAD:
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
        case XR_XIR_CONVERT_NUMBER:
            return xr_xir_operand_type(f->source, op->args[0]) == XR_XIR_I64
                       ? (op->type == XR_XIR_I64 ? XR_LIR_INTCAST : XR_LIR_ITOF)
                       : XR_LIR_FCAST;
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
    if (value->type == XR_LIR_STRING && value->block &&
        value->block != f->blocks[f->owner->block].id)
        return lower_unsupported(f->owner, "String alias across blocks without owned parameters");
    XR_DCHECK(out->module == f->owner->module && out->function == f->function.id,
              "SSA values remain in their defining output function");
    return true;
}
static bool lower_emit_scalar(LowerFunction *f, uint32_t b, uint32_t i) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[i];
    uint32_t result = f->source->parameter_count + i;
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
    if (op->op == XR_XIR_COPY && op->type != XR_XIR_STRING)
        return lower_value(f, op->args[0], &f->values[result]);
    if (op->op == XR_XIR_STRING_LEN) {
        XrLirValue input = {0};
        if (!lower_value(f, op->args[0], &input)) return false;
        XrLirInstruction length = {.op=XR_LIR_STRING_LEN, .type=XR_LIR_I64,
            .operands=&input, .operand_count=1, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &length, &f->values[result]));
    }
    if ((op->op == XR_XIR_COPY && op->type == XR_XIR_STRING) ||
        op->op == XR_XIR_TO_STRING || op->op == XR_XIR_CONCAT_STRING) {
        XrLirValue args[2] = {{0}};
        uint32_t count = op->op == XR_XIR_CONCAT_STRING ? 2u : 1u;
        for (uint32_t a = 0; a < count; ++a) if (!lower_value(f, op->args[a], &args[a])) return false;
        XrLirInstruction owned = {.op = op->op == XR_XIR_CONCAT_STRING ? XR_LIR_STRING_CONCAT :
            xr_xir_operand_type(f->source, op->args[0]) == XR_XIR_I64 ? XR_LIR_STRING_I64 :
            xr_xir_operand_type(f->source, op->args[0]) == XR_XIR_F64 ? XR_LIR_STRING_F64 : XR_LIR_STRING_RETAIN,
            .type=XR_LIR_STRING, .operands=args, .operand_count=count, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &owned, &f->values[result]));
    }
    XrLirInstruction instruction = {.op = lower_operation(f, op),
                                    .type = lower_type(op->type),
                                    .predicate = lower_predicate(op),
                                    .source = lower_source(c)};
    XrLirValue operands[2] = {{0}};
    if (instruction.op == XR_LIR_CONST)
        instruction.bits = op->op == XR_XIR_SLOT_LOAD
            ? c->literals[op->immediate].bits : (uint64_t) op->immediate;
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
    uint32_t hidden = op->op == XR_XIR_PRINT ? 0 : lower_hidden_cells(c, (uint32_t)op->immediate);
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
    for (uint32_t s = 0; s < hidden && good; ++s) {
        arguments[op->args[1] + s] = f->slots[s];
        good = f->slots[s].id != 0;
    }
    XrLirInstruction call = {
        .op = XR_LIR_CALL,
        .type = lower_type(op->type),
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
    *handled = false;
    bool string = lower_string_storage(c, f->source, op);
    if (!c->mutable_slots && !string) return true;
    uint32_t result = f->source->parameter_count + i;
    if (op->op == XR_XIR_SLOT_LOAD) {
        f->values[result] = f->slots[op->immediate]; *handled = true; return true;
    }
    bool create = op->op == XR_XIR_CELL_NEW;
    uint32_t s = lower_module_cell(c, c->function, create ? result : op->args[0]);
    if ((!string && s == UINT32_MAX) || (!create && op->op != XR_XIR_CELL_READ && op->op != XR_XIR_CELL_WRITE))
        return true;
    *handled = true;
    XrLirValue args[2] = {0};
    if (!lower_value(f, op->args[0], &args[0])) return false;
    if (op->op == XR_XIR_CELL_WRITE && !lower_value(f, op->args[1], &args[1])) return false;
    XrLirInstruction v = {.op = create ? XR_LIR_CELL_NEW :
        op->op == XR_XIR_CELL_READ ? XR_LIR_CELL_READ : XR_LIR_CELL_WRITE,
        .type = create ? (string ? XR_LIR_STRING_CELL : XR_LIR_I64_CELL) : lower_type(op->type), .operands=args,
        .operand_count=op->op == XR_XIR_CELL_WRITE ? 2u : 1u, .source=lower_source(c)};
    bool good = lower_build(c, v.type == XR_LIR_UNIT
        ? xr_lir_emit_effect(c->module,f->blocks[b],&v)
        : xr_lir_emit_value(c->module,f->blocks[b],&v,&f->values[result]));
    if (create && good && s != UINT32_MAX) f->slots[s] = f->values[result];
    return good;
}

static bool lower_string_copy(LowerFunction *, uint32_t, XrLirValue, XrLirValue *);

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
    for (uint32_t i = block->first + f->phis[b]; i < block->first + block->count - 1; ++i) {
        c->instruction = i;
        const XrXirInstruction *op = &f->source->instructions[i];
        uint32_t result = f->source->parameter_count + i;
        if (op->op == XR_XIR_SLOT_INIT)
            continue;
        bool handled;
        if (!lower_module_operation(f, b, i, &handled)) return false;
        if (handled) continue;
        if (lower_state_new(op->op) || lower_state_read(op->op) || lower_state_write(op->op)) {
            uint32_t cell;
            if (!lower_cell_index(f, lower_state_new(op->op) ? result : op->args[0], &cell))
                return false;
            if (lower_state_read(op->op)) {
                if (!f->current[cell].id) {
                    lower_error(c, XR_LIR_VERIFY_ERROR, "Cell read has no SSA state");
                    return false;
                }
                if (f->cell_types[cell] == XR_LIR_STRING) {
                    if (!lower_string_copy(f, b, f->current[cell], &f->values[result])) return false;
                } else f->values[result] = f->current[cell];
            } else if (!lower_value(f, op->args[lower_state_new(op->op) ? 0 : 1],
                                    &f->current[cell]))
                return false;
        } else if (op->op == XR_XIR_CALL || op->op == XR_XIR_PRINT) {
            if (!lower_call(f, b, i))
                return false;
        } else if (!lower_emit_scalar(f, b, i))
            return false;
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
            if (type == XR_LIR_STRING || type == XR_LIR_STRING_CELL)
                return lower_unsupported(c, "loop-carried managed String transfer");
        }
    return true;
}
static bool lower_string_copy(LowerFunction *f, uint32_t block, XrLirValue input, XrLirValue *output) {
    XrLirInstruction copy = {.op=XR_LIR_STRING_RETAIN, .type=XR_LIR_STRING,
        .operands=&input, .operand_count=1, .source=lower_source(f->owner)};
    return lower_build(f->owner, xr_lir_emit_value(f->owner->module, f->blocks[block], &copy, output));
}
static bool lower_string_kept(const XrLirInstruction *term, uint32_t id) {
    if (term->op == XR_LIR_RET) return term->operand_count && term->operands[0].id == id;
    for (uint32_t e = 0; e < term->edge_count; ++e)
        for (uint32_t a = 0; a < term->edges[e].argument_count; ++a)
            if (term->edges[e].arguments[a].id == id) return true;
    return false;
}
static bool lower_string_release(LowerFunction *f, uint32_t block,
    XrLirValue value, const XrLirInstruction *term) {
    XrLirType type = f->owner->module->functions[f->function.id - 1].values[value.id - 1].type;
    if (type != XR_LIR_STRING || lower_string_kept(term, value.id))
        return true;
    XrLirInstruction drop = {.op=XR_LIR_STRING_DROP,
        .type=XR_LIR_UNIT, .operands=&value, .operand_count=1, .source=lower_source(f->owner)};
    return lower_build(f->owner, xr_lir_emit_effect(f->owner->module, f->blocks[block], &drop));
}
static bool lower_strings_release(LowerFunction *f, uint32_t block, const XrLirInstruction *term) {
    XrLirFunctionData *function = &f->owner->module->functions[f->function.id - 1];
    uint32_t block_id = f->blocks[block].id - 1;
    uint32_t parameters = function->blocks[block_id].parameter_count;
    for (uint32_t p = 0; p < parameters; ++p) {
        XrLirValue value = {f->owner->module, f->function.id, function->blocks[block_id].parameters[p]};
        if (!lower_string_release(f, block, value, term)) return false;
    }
    uint32_t count = function->blocks[block_id].instruction_count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t result = function->blocks[block_id].instructions[i].view.result;
        if (result) {
            XrLirValue value = {f->owner->module, f->function.id, result};
            if (!lower_string_release(f, block, value, term)) return false;
        }
    }
    if (term->op == XR_LIR_RET)
        for (uint32_t i = 0; i < f->frame_cell_count; ++i) {
            XrLirInstruction drop = {.op=XR_LIR_STRING_CELL_DROP, .type=XR_LIR_UNIT,
                .operands=&f->frame_cells[i], .operand_count=1, .source=lower_source(f->owner)};
            if (!lower_build(f->owner, xr_lir_emit_effect(f->owner->module, f->blocks[block], &drop)))
                return false;
        }
    return true;
}

/* Physical Cell contents survive block edges, unlike temporary String owners.
 * Inventory actual entry constructors; incoming hidden parameters remain borrows. */
static bool lower_frame_cells(LowerFunction *f) {
    XrLirFunctionData *function = &f->owner->module->functions[f->function.id - 1];
    const XrLirBlockData *entry = &function->blocks[function->entry - 1];
    for (uint32_t i = 0; i < entry->instruction_count; ++i) {
        const XrLirInstructionView *v = &entry->instructions[i].view;
        f->frame_cell_count += v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_STRING_CELL;
    }
    f->frame_cells = lower_alloc(f->owner, f->frame_cell_count, sizeof(*f->frame_cells));
    if (f->frame_cell_count && !f->frame_cells) return false;
    uint32_t at = 0;
    for (uint32_t i = 0; i < entry->instruction_count; ++i) {
        const XrLirInstructionView *v = &entry->instructions[i].view;
        if (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_STRING_CELL)
            f->frame_cells[at++] = (XrLirValue){f->owner->module, f->function.id, v->result};
    }
    XR_DCHECK(at == f->frame_cell_count, "each entry Cell has one normal-exit owner");
    return true;
}
static bool lower_terminator(LowerFunction *f, uint32_t b) {
    Lower *c = f->owner;
    const XrXirBlock *block = &f->source->blocks[b];
    c->block = b;
    c->instruction = block->first + block->count - 1;
    const XrXirInstruction *term = lower_term(c, f->source, b);
    if (!term)
        return false;
    XR_DCHECK(term->op == XR_XIR_RETURN || term->op == XR_XIR_JUMP || term->op == XR_XIR_BRANCH,
              "admission rejected unsupported terminators");
    XrLirValue value = {0};
    XrLirInstruction instruction = {.type = XR_LIR_UNIT, .source = lower_source(c)};
    bool good = true;
    if (term->op == XR_XIR_RETURN && c->mutable_slots &&
        c->function == c->source->declarations->modules[0].initializer) {
        uint32_t entry = c->source->declarations->entry_function;
        XrLirType type = lower_type(c->source->functions[entry].result);
        XrLirInstruction call = {.op=XR_LIR_CALL,.type=type,.callee=c->functions[entry],
            .operands=f->slots,.operand_count=lower_hidden_cells(c, entry),
            .source=lower_source(c)};
        good = lower_build(c, type == XR_LIR_UNIT
            ? xr_lir_emit_effect(c->module,f->blocks[b],&call)
            : xr_lir_emit_value(c->module,f->blocks[b],&call,&value));
        instruction.op=XR_LIR_RET;
        instruction.operands=type == XR_LIR_UNIT ? NULL : &value;
        instruction.operand_count=type == XR_LIR_UNIT ? 0u : 1u;
    } else if (term->op == XR_XIR_RETURN) {
        instruction.op = XR_LIR_RET;
        if (f->source->result != XR_XIR_UNIT) {
            good = lower_value(f, term->args[0], &value);
            if (good && f->source->result == XR_XIR_STRING) {
                XrLirValue result = {0};
                good = lower_string_copy(f, b, value, &result);
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
    if (good) good = lower_strings_release(f, b, &instruction);
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
            good = lower_string_copy(&f, 0, f.values[p], &retained);
            f.values[p] = retained;
        }
    if (c->mutable_slots && good) {
        uint32_t count = c->source->declarations->slot_count;
        f.slots = lower_alloc(c,count,sizeof(*f.slots));
        good = f.slots != NULL;
        if (index != c->source->declarations->modules[0].initializer)
            for (uint32_t s=0; s<lower_hidden_cells(c, index) && good; ++s)
                good = lower_build(c,xr_lir_function_parameter(c->module,f.function,
                    f.source->parameter_count+s,&f.slots[s]));
    }
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
    uint32_t init = d->modules[d->root_module].initializer, entry = d->entry_function;
    XR_DCHECK(c->functions[init].id && c->functions[entry].id,
              "both program roots have output functions");
    XrLirFunctionSpec spec = {
        .name = "main", .name_length = 4, .result = lower_type(c->source->functions[entry].result)};
    XrLirFunction wrapper = {0};
    XrLirBlock block = {0};
    XrLirValue result = {0};
    if (!lower_build(c, xr_lir_declare_function(c->module, &spec, &wrapper)) ||
        !lower_build(c, xr_lir_define_function(c->module, wrapper)) ||
        !lower_build(c, xr_lir_add_block(c->module, wrapper, NULL, 0, &block)) ||
        !lower_build(c, xr_lir_set_entry(c->module, wrapper, block)))
        return false;
    uint32_t count = c->mutable_slots ? 1u : c->initialization_count + 1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t selected = c->mutable_slots ? init : i == c->initialization_count ? entry :
            d->modules[c->module_order[i]].initializer;
        XrLirInstruction call = {.op = XR_LIR_CALL,
                                 .type = c->mutable_slots ? spec.result :
                                     lower_type(c->source->functions[selected].result),
                                 .callee = c->functions[selected]};
        bool good = call.type == XR_LIR_UNIT
                        ? lower_build(c, xr_lir_emit_effect(c->module, block, &call))
                        : lower_build(c, xr_lir_emit_value(c->module, block, &call, &result));
        if (!good)
            return false;
    }
    XrLirInstruction ret = {.op = XR_LIR_RET,
                            .type = XR_LIR_UNIT,
                            .operands = spec.result == XR_LIR_UNIT ? NULL : &result,
                            .operand_count = spec.result == XR_LIR_UNIT ? 0u : 1u};
    if (!lower_build(c, xr_lir_set_terminator(c->module, block, &ret)))
        return false;
    *output = wrapper;
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
    if (!lower_modules(&c) || !lower_literal(&c)) {
        xr_free(c.module_slot_for_value); xr_free(c.module_publications);
        lower_modules_free(&c);
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
    if (good)
        good = lower_build(&c, xr_lir_module_create(NULL, &c.module));
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
    if (good) {
        XR_DCHECK(c.status == XR_LIR_OK, "only a complete successful module is published");
        *output = (XrLirLowered) {c.module, entry};
        c.module = NULL;
    }
    xr_lir_module_destroy(c.module);
    xr_free(c.module_slot_for_value);
    xr_free(c.module_publications);
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
