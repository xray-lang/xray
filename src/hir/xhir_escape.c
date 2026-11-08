/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_escape.c - Local callable lifetime restrictions and capture closure
 *
 * KEY CONCEPT:
 *   Local storage preserves restrictions. Publication checks consume those
 *   restrictions, while capture edges also check the nested function body.
 */
#include "xhir_escape.h"
#include "../xir/xxir_compile_memory.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_view_types_internal.h"
#include "../xir/xxir_operand_roles.h"
#include "../xir/xxir_interface.h"
#include "../xir/xxir_receive_control.h"
#include "../shared/xerror_codes.h"
#include "../shared/xr_utf8_core.h"
#include <stdio.h>
#include <stdlib.h>

enum { ESCAPE_RESTRICTION = 1u, ESCAPE_STORAGE = 2u, ESCAPE_CONTENTS = 4u };
enum { ESCAPE_LOCAL = 1u, ESCAPE_EXTERNAL = 2u };

typedef struct HirEscapeNode {
    uint32_t head, parameter, borrowed;
    XrXirType type;
    uint8_t storage;
    bool queued;
} HirEscapeNode;
typedef struct HirEscapeOrigin { uint32_t parameter, borrowed; } HirEscapeOrigin;
typedef struct HirEscapeFunction { uint32_t parameters, captures, physical_values; } HirEscapeFunction;

typedef struct HirEscapeEdge { uint32_t target, next, flags; } HirEscapeEdge;
typedef struct HirEscapeUse {
    uint32_t function, instruction, value, storage;
    const char *reason;
} HirEscapeUse;
typedef struct HirEscapeCapture { uint32_t source, result; bool view, reference; } HirEscapeCapture;
typedef struct HirEscapeObservation {
    uint32_t function, instruction, value;
    XirStoragePointKind point;
} HirEscapeObservation;

typedef struct HirEscapeFactRow {
    uint32_t count, flow_count;
    uint8_t *flags;
    XrXirValueFlow *flows;
} HirEscapeFactRow;

struct XrHirEscapeFacts {
    XrCompileResources *resources;
    uint32_t count;
    HirEscapeFactRow *rows;
    XirRestrictionView view;
    bool symbolic;
};

typedef struct HirEscape {
    const XrXirCompileContext *memory;
    const XrXirModule *module;
    const XrHirEscapeDeclaration *declarations;
    XrCompileRequestControl *control;
    HirEscapeNode *nodes;
    HirEscapeFunction *functions;
    HirEscapeEdge *edges;
    HirEscapeUse *uses;
    HirEscapeCapture *captures;
    uint32_t *offsets, *queue;
    uint32_t count, edge_count, edge_capacity, progress;
    uint32_t use_count, use_capacity, capture_count, capture_capacity;
    uint32_t queue_first, queue_count;
    XrXirStatus status;
} HirEscape;

struct XrHirEscapeBuilder {
    HirEscape graph;
    XrXirCompileContext memory;
    XrXirModule module;
    XrXirDeclarations identities;
    XrHirEscapeDeclaration *declarations;
    uint32_t *observed;
    HirEscapeObservation *observations;
    uint32_t observation_count, observation_capacity;
    struct HirRestrictionSolver *point_preparation;
    bool finished;
};

static void escape_points_free(XrHirEscapeBuilder *);

XR_FUNC void xr_hir_escape_facts_free(XrHirEscapeFacts *facts) {
    if (!facts) return;
    for (uint32_t f = 0; f < facts->count; ++f) {
        xr_compile_resources_free(facts->rows[f].flags);
        xr_compile_resources_free(facts->rows[f].flows);
        if (facts->view.functions) xr_compile_resources_free((void *)facts->view.functions[f].intrinsic);
    }
    xr_compile_resources_free(facts->rows);
    xr_compile_resources_free((void *)facts->view.functions);
    xr_compile_resources_free((void *)facts->view.formulas);
    xr_compile_resources_free((void *)facts->view.inputs);
    xr_compile_resources_free((void *)facts->view.points);
    xr_compile_resources_free(facts);
}

XR_FUNC XrXirStatus xr_hir_escape_restricted(const XrHirEscapeFacts *facts,
    uint32_t function, uint32_t value, bool *output) {
    if (!facts || !output || function >= facts->count || value >= facts->rows[function].count)
        return XR_XIR_BAD_STRUCTURE;
    *output = facts->rows[function].flags[value] != 0;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_restriction(const XrHirEscapeFacts *facts,
    uint32_t function, uint32_t instruction, uint32_t value, XirRestrictionFormula *output) {
    if (!facts || !output || function >= facts->count) return XR_XIR_BAD_STRUCTURE;
    if (!facts->symbolic) return XR_XIR_UNRESOLVED;
    const XirRestrictionView *view = &facts->view;
    if (value >= view->functions[function].value_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t formula = view->functions[function].intrinsic[value];
    if (instruction != UINT32_MAX) {
        uint32_t first = 0, end = view->point_count;
        while (first < end) {
            uint32_t middle = first + (end - first) / 2;
            XirRestrictionPoint point = view->points[middle];
            if (point.function < function || (point.function == function &&
                (point.instruction < instruction || (point.instruction == instruction && point.value < value))))
                first = middle + 1;
            else end = middle;
        }
        bool found = false;
        if (first < view->point_count) {
            XirRestrictionPoint point = view->points[first];
            if (point.function == function && point.instruction == instruction && point.value == value) {
                formula = point.formula;
                found = true;
            }
        }
        if (!found) return XR_XIR_UNRESOLVED;
    }
    if (formula >= view->formula_count) return XR_XIR_BAD_STRUCTURE;
    XirRestrictionFormulaRow row = view->formulas[formula];
    *output = (XirRestrictionFormula){row.constant, row.count ? view->inputs + row.first : NULL, row.count};
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_facts_view(const XrHirEscapeFacts *facts, XirRestrictionView *output) {
    if (!facts || !output || !facts->symbolic) return XR_XIR_BAD_STRUCTURE;
    *output = facts->view;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_flows(const XrHirEscapeFacts *facts, uint32_t function,
    const XrXirValueFlow **output, uint32_t *count) {
    if (!facts || !output || !count || function >= facts->count) return XR_XIR_BAD_STRUCTURE;
    *output = facts->rows[function].flows;
    *count = facts->rows[function].flow_count;
    return XR_XIR_OK;
}

static XrHirEscapeFacts *escape_facts_new(const XrXirCompileContext *memory,
    uint32_t count, XrXirStatus *status) {
    XrHirEscapeFacts *facts = xir_compile_calloc(memory, 1, sizeof(*facts), status);
    if (!facts) return NULL;
    facts->resources = memory->resources;
    facts->rows = count ? xir_compile_calloc(memory, count, sizeof(*facts->rows), status) : NULL;
    if (count && !facts->rows) {xr_hir_escape_facts_free(facts); return NULL;}
    facts->count = count;
    return facts;
}

XR_FUNC XrXirStatus xr_hir_escape_facts_copy(const XrXirCompileContext *memory,
    const XrHirEscapeFacts *source, XrCompileRequestControl *control, XrHirEscapeFacts **output) {
    if (!xir_compile_context_valid(memory) || !source || !output || *output ||
        source->resources != memory->resources || source->symbolic) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    XrHirEscapeFacts *copy = escape_facts_new(memory, source->count, &status);
    for (uint32_t f = 0; copy && f < source->count && status == XR_XIR_OK; ++f) {
        status = xir_receive_poll(control);
        if (status != XR_XIR_OK) break;
        uint32_t count = source->rows[f].count;
        copy->rows[f].flags = count ? xir_compile_copy(memory, source->rows[f].flags, count, &status) : NULL;
        copy->rows[f].count = count;
        uint32_t flows = source->rows[f].flow_count;
        copy->rows[f].flows = flows ? xir_compile_copy(memory, source->rows[f].flows,
            (size_t)flows * sizeof(*source->rows[f].flows), &status) : NULL;
        copy->rows[f].flow_count = flows;
    }
    if (status != XR_XIR_OK) {xr_hir_escape_facts_free(copy); return status;}
    *output = copy;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_facts_relocate(const XrXirCompileContext *memory, XrHirEscapeFacts *facts,
    uint32_t function, uint32_t count, const uint32_t *values, XrCompileRequestControl *control) {
    if (!xir_compile_context_valid(memory) || !facts || function >= facts->count ||
        facts->resources != memory->resources || facts->symbolic ||
        (facts->rows[function].count && !values)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    uint8_t *flags = count ? xir_compile_calloc(memory, count, sizeof(*flags), &status) : NULL;
    uint32_t flow_count = facts->rows[function].flow_count;
    XrXirValueFlow *flows = flow_count ? xir_compile_calloc(memory, flow_count, sizeof(*flows), &status) : NULL;
    uint32_t progress = 0;
    for (uint32_t v = 0; v < facts->rows[function].count && status == XR_XIR_OK; ++v) {
        status = xir_receive_step(control, &progress);
        if (status != XR_XIR_OK) break;
        if (values[v] >= count) {status = XR_XIR_BAD_STRUCTURE; break;}
        flags[values[v]] |= facts->rows[function].flags[v];
    }
    for (uint32_t e = 0; e < flow_count && status == XR_XIR_OK; ++e) {
        status = xir_receive_step(control, &progress);
        XrXirValueFlow edge = facts->rows[function].flows[e];
        if (edge.source >= facts->rows[function].count || edge.target >= facts->rows[function].count) {
            status = XR_XIR_BAD_STRUCTURE; break;
        }
        flows[e] = (XrXirValueFlow){values[edge.source], values[edge.target]};
    }
    if (status != XR_XIR_OK) {xr_compile_resources_free(flags); xr_compile_resources_free(flows); return status;}
    xr_compile_resources_free(facts->rows[function].flags);
    xr_compile_resources_free(facts->rows[function].flows);
    facts->rows[function] = (HirEscapeFactRow){.count=count,.flow_count=flow_count,.flags=flags,.flows=flows};
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_function_match(const XrXirFunction *actual,
    const XrXirFunction *required, const char **reason) {
    if (reason) *reason = NULL;
    if (!actual || !required) return XR_XIR_BAD_STRUCTURE;
    if (actual->parameter_count != required->parameter_count) return XR_XIR_OK;
    for (uint32_t p = 0; p < actual->parameter_count; ++p)
        if (xr_xir_function_parameter_escaping(actual, p) != xr_xir_function_parameter_escaping(required, p)) {
            if (reason) *reason = "function parameter escaping flags must match the required signature exactly";
            return XR_XIR_BAD_TYPE;
        }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_callable_match(const XrXirFunction *actual, uint32_t offset,
    const XrXirTypeNode *required, const char **reason) {
    if (reason) *reason = NULL;
    if (!actual || !required || offset > actual->parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (actual->parameter_count - offset != required->parameter_count) return XR_XIR_OK;
    for (uint32_t p = 0; p < required->parameter_count; ++p)
        if (xr_xir_function_parameter_escaping(actual, p + offset) != (required->parameters[p].escaping != 0)) {
            if (reason) *reason = "function parameter escaping flags must match the required signature exactly";
            return XR_XIR_BAD_TYPE;
        }
    return XR_XIR_OK;
}

static bool escape_poll(HirEscape *owner) {
    if (owner->status == XR_XIR_OK)
        owner->status = xir_receive_step(owner->control, &owner->progress);
    return owner->status == XR_XIR_OK;
}

static uint32_t escape_node(HirEscape *owner, uint32_t function, uint32_t value) {
    if (function >= owner->module->function_count ||
        value >= owner->offsets[function + 1] - owner->offsets[function]) {
        owner->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    return owner->offsets[function] + value;
}

static bool escape_edge(HirEscape *owner, uint32_t source, uint32_t target, uint32_t flags) {
    if (source >= owner->count || target >= owner->count) {
        owner->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (owner->edge_count == owner->edge_capacity) {
        HirEscapeEdge *grown = xir_compile_array_grow(owner->memory, owner->edges,
            owner->edge_count, sizeof(*grown), &owner->edge_capacity, &owner->status);
        if (!grown) return false;
        owner->edges = grown;
    }
    uint32_t index = owner->edge_count++;
    owner->edges[index] = (HirEscapeEdge){target, owner->nodes[source].head, flags};
    owner->nodes[source].head = index;
    return true;
}

static bool escape_value_edge(HirEscape *owner, uint32_t function,
    uint32_t source, uint32_t target, uint32_t flags) {
    uint32_t from = escape_node(owner, function, source);
    uint32_t to = escape_node(owner, function, target);
    return owner->status == XR_XIR_OK && escape_edge(owner, from, to, flags);
}

static void escape_enqueue(HirEscape *owner, uint32_t index) {
    if (owner->nodes[index].queued) return;
    owner->queue[(owner->queue_first + owner->queue_count) % owner->count] = index;
    ++owner->queue_count;
    owner->nodes[index].queued = true;
}

static bool escape_close(HirEscape *owner) {
    for (uint32_t n = 0; n < owner->count; ++n)
        if (owner->nodes[n].parameter || owner->nodes[n].borrowed || owner->nodes[n].storage)
            escape_enqueue(owner, n);
    while (owner->queue_count && escape_poll(owner)) {
        uint32_t index = owner->queue[owner->queue_first];
        owner->queue_first = (owner->queue_first + 1) % owner->count;
        --owner->queue_count;
        owner->nodes[index].queued = false;
        HirEscapeNode source = owner->nodes[index];
        for (uint32_t e = source.head; e != UINT32_MAX; e = owner->edges[e].next) {
            if (!escape_poll(owner)) return false;
            HirEscapeEdge edge = owner->edges[e];
            HirEscapeNode *target = &owner->nodes[edge.target];
            bool changed = false;
            if (edge.flags & ESCAPE_RESTRICTION) {
                if (source.parameter && !target->parameter) {target->parameter = source.parameter; changed = true;}
                if (source.borrowed && !target->borrowed) {target->borrowed = source.borrowed; changed = true;}
            }
            if ((edge.flags & ESCAPE_STORAGE) && (source.storage & ~target->storage)) {
                target->storage |= source.storage;
                changed = true;
            }
            if (changed) escape_enqueue(owner, edge.target);
        }
    }
    return owner->status == XR_XIR_OK;
}

static bool escape_prepare(HirEscape *owner) {
    uint32_t functions = owner->module->function_count;
    owner->offsets = xir_compile_calloc(owner->memory, (size_t)functions + 1,
        sizeof(*owner->offsets), &owner->status);
    if (!owner->offsets) return false;
    uint64_t count = 0;
    for (uint32_t f = 0; f < functions; ++f) {
        const XrXirFunction *body = &owner->module->functions[f];
        count += (uint64_t)body->parameter_count + body->instruction_count;
        if (count > UINT32_MAX) {owner->status = XR_XIR_OVERFLOW; return false;}
        owner->offsets[f + 1] = (uint32_t)count;
    }
    owner->count = (uint32_t)count;
    owner->functions = functions ? xir_compile_calloc(owner->memory, functions,
        sizeof(*owner->functions), &owner->status) : NULL;
    if (functions && !owner->functions) return false;
    for (uint32_t f = 0; f < functions; ++f) {
        uint32_t captures = owner->module->declarations ? owner->module->declarations->functions[f].capture_count : 0;
        owner->functions[f] = (HirEscapeFunction){owner->module->functions[f].parameter_count,
            captures, owner->offsets[f + 1] - owner->offsets[f]};
    }
    if (!count) return true;
    owner->nodes = xir_compile_calloc(owner->memory, owner->count, sizeof(*owner->nodes), &owner->status);
    owner->queue = xir_compile_calloc(owner->memory, owner->count, sizeof(*owner->queue), &owner->status);
    if (!owner->nodes || !owner->queue) return false;
    for (uint32_t n = 0; n < owner->count; ++n) owner->nodes[n].head = UINT32_MAX;
    for (uint32_t f = 0; f < functions; ++f) {
        const XrXirFunction *body = &owner->module->functions[f];
        const XrHirEscapeDeclaration *declaration = owner->declarations ? &owner->declarations[f] : NULL;
        uint32_t captures = owner->module->declarations ? owner->module->declarations->functions[f].capture_count : 0;
        if (captures > body->parameter_count || (declaration && (declaration->captures > body->parameter_count ||
            declaration->count > body->parameter_count - declaration->captures))) {
            owner->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        for (uint32_t p = 0; p < body->parameter_count; ++p) {
            uint32_t node = owner->offsets[f] + p;
            owner->nodes[node].type = body->parameters[p];
            if (p >= captures && xr_xir_type_is_cell(owner->module->types, body->parameters[p]))
                owner->nodes[node].storage = ESCAPE_EXTERNAL;
            if (p >= captures && xr_xir_callable_signature(owner->module->types, body->parameters[p]) &&
                !xr_xir_type_is_c_callable(owner->module->types, body->parameters[p]) &&
                !xr_xir_function_parameter_escaping(body, p)) owner->nodes[node].parameter = node + 1;
        }
    }
    return true;
}

static bool escape_capture_arguments(HirEscape *owner, uint32_t f, const XrXirInstruction *op) {
    const XrXirFunction *body = &owner->module->functions[f];
    if (!owner->module->declarations) return true;
    uint32_t first = 0, count = 0;
    if (!xr_xir_direct_argument_range(op, &first, &count) || op->immediate < 0 ||
        (uint64_t)op->immediate >= owner->module->function_count) {
        owner->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    uint32_t target = (uint32_t)op->immediate;
    uint32_t captures = owner->module->declarations->functions[target].capture_count;
    if (captures > count) {owner->status = XR_XIR_BAD_STRUCTURE; return false;}
    for (uint32_t p = 0; p < captures; ++p) {
        uint32_t from = escape_node(owner, f, body->operands[first + p]);
        uint32_t to = escape_node(owner, target, p);
        if (owner->status != XR_XIR_OK || !escape_edge(owner, from, to, ESCAPE_RESTRICTION | ESCAPE_STORAGE)) return false;
    }
    return true;
}

static bool escape_alias(HirEscape *owner, uint32_t f, uint32_t i, const XrXirInstruction *op) {
    const XrXirFunction *body = &owner->module->functions[f];
    uint32_t result = body->parameter_count + i;
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_VALUE_TAKE: case XR_XIR_CELL_PLACE:
        return escape_value_edge(owner, f, op->args[0], result, ESCAPE_RESTRICTION | ESCAPE_STORAGE);
    case XR_XIR_CELL_READ: case XR_XIR_CELL_TAKE: case XR_XIR_PLACE_READ:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
        return escape_value_edge(owner, f, op->args[0], result,
            ESCAPE_RESTRICTION | ESCAPE_STORAGE | ESCAPE_CONTENTS);
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_NEW_TAKE: case XR_XIR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
        owner->nodes[owner->offsets[f] + result].storage = ESCAPE_LOCAL;
        return escape_value_edge(owner, f, op->args[0], result, ESCAPE_RESTRICTION | ESCAPE_CONTENTS);
    case XR_XIR_LOCAL_UNINIT:
        owner->nodes[owner->offsets[f] + result].storage = ESCAPE_LOCAL;
        return true;
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
    case XR_XIR_PLACE_WRITE:
        return escape_value_edge(owner, f, op->args[1], op->args[0], ESCAPE_RESTRICTION | ESCAPE_CONTENTS);
    case XR_XIR_PHI:
        for (uint32_t a = 1; a < op->args[1]; a += 2)
            if (!escape_value_edge(owner, f, body->operands[op->args[0] + a], result,
                    ESCAPE_RESTRICTION | ESCAPE_STORAGE)) return false;
        return true;
    case XR_XIR_FUNCTION_REF: case XR_XIR_CLEANUP_REGISTER: {
        if (op->immediate < 0 || (uint64_t)op->immediate >= owner->module->function_count) {
            owner->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        uint32_t target = (uint32_t)op->immediate;
        for (uint32_t a = 0; a < op->args[1]; ++a) {
            uint32_t source = escape_node(owner, f, body->operands[op->args[0] + a]);
            uint32_t capture = escape_node(owner, target, a);
            if (owner->status != XR_XIR_OK ||
                !escape_edge(owner, source, capture, ESCAPE_RESTRICTION | ESCAPE_STORAGE)) return false;
            if (op->op == XR_XIR_FUNCTION_REF &&
                !escape_edge(owner, source, owner->offsets[f] + result, ESCAPE_RESTRICTION)) return false;
        }
        return true;
    }
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_GO: case XR_XIR_SCOPED_GO: case XR_XIR_GENERATOR_NEW:
        return escape_capture_arguments(owner, f, op);
    default: return true;
    }
}

static const char *escape_argument_reason(HirEscape *owner, uint32_t f,
    const XrXirInstruction *op, uint32_t ordinal) {
    const XrXirFunction *body = &owner->module->functions[f];
    if (op->op == XR_XIR_GO || op->op == XR_XIR_SCOPED_GO || op->op == XR_XIR_GENERATOR_NEW)
        return "a task or generator";
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE ||
        op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_CLEANUP_REGISTER) {
        if (op->immediate < 0 || (uint64_t)op->immediate >= owner->module->function_count) return "an unknown callee";
        const XrXirFunction *target = &owner->module->functions[op->immediate];
        if (ordinal >= target->parameter_count) return "an unknown parameter";
        if (owner->module->declarations &&
            ordinal < owner->module->declarations->functions[op->immediate].capture_count) return NULL;
        if (xr_xir_function_parameter_escaping(target, ordinal)) return "an escaping parameter";
        return xr_xir_callable_signature(owner->module->types, target->parameters[ordinal]) &&
            !xr_xir_type_is_c_callable(owner->module->types, target->parameters[ordinal]) ? NULL : "a generic or stored value parameter";
    }
    const XrXirTypeNode *signature = NULL;
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT)
        signature = xr_xir_callable_signature(owner->module->types, xr_xir_operand_type(body, (uint32_t)op->immediate));
    if (op->op == XR_XIR_CALL_REQUIREMENT) {
        const XrXirInterfaceTable *table = owner->module->types ? owner->module->types->interfaces : NULL;
        if (!ordinal) return "an interface receiver";
        --ordinal;
        if (table && op->targets[0] < table->count &&
            op->targets[1] < table->declarations[op->targets[0]].method_count)
            signature = xr_xir_callable_signature(owner->module->types,
                table->declarations[op->targets[0]].methods[op->targets[1]].signature);
    }
    if (!signature || ordinal >= signature->parameter_count) return "an unknown callee";
    if (signature->parameters[ordinal].escaping) return "an escaping parameter";
    return xr_xir_callable_signature(owner->module->types, signature->parameters[ordinal].type) &&
        !xr_xir_type_is_c_callable(owner->module->types, signature->parameters[ordinal].type) ? NULL : "a generic or stored value parameter";
}

static bool escape_record_captures(HirEscape *owner, uint32_t f, uint32_t i, const XrXirInstruction *op) {
    if (op->op != XR_XIR_FUNCTION_REF) return true;
    const XrXirFunction *body = &owner->module->functions[f];
    uint32_t result = owner->offsets[f] + body->parameter_count + i;
    for (uint32_t a = 0; a < op->args[1] && escape_poll(owner); ++a) {
        uint32_t value = body->operands[op->args[0] + a];
        uint32_t source = escape_node(owner, f, value), flags = 0;
        if (owner->status != XR_XIR_OK) return false;
        XrXirType type = xr_xir_operand_type(body, value);
        owner->status = xr_xir_compile_view_type_flags(owner->memory, owner->module->types, type, &flags);
        if (owner->status != XR_XIR_OK) return false;
        bool reference = xr_xir_type_is_cell(owner->module->types, type);
        if (!flags && !reference) continue;
        if (owner->capture_count == owner->capture_capacity) {
            HirEscapeCapture *grown = xir_compile_array_grow(owner->memory, owner->captures,
                owner->capture_count, sizeof(*grown), &owner->capture_capacity, &owner->status);
            if (!grown) return false;
            owner->captures = grown;
        }
        owner->captures[owner->capture_count++] = (HirEscapeCapture){source, result, flags != 0, reference};
    }
    return owner->status == XR_XIR_OK;
}

static bool escape_borrow_captures(HirEscape *owner) {
    for (uint32_t c = 0; c < owner->capture_count && escape_poll(owner); ++c) {
        HirEscapeCapture capture = owner->captures[c];
        if (capture.view || (capture.reference && owner->nodes[capture.source].storage != ESCAPE_LOCAL))
            owner->nodes[capture.result].borrowed = capture.source + 1;
    }
    return true;
}

static const char *escape_parameter_label(const char *name, char label[100]) {
    size_t count = 0;
    while (count < 96 && name[count]) ++count;
    if (!name[count]) return name;
    while (count && xr_utf8_core_is_continuation((uint8_t)name[count])) --count;
    memcpy(label, name, count);
    memcpy(label + count, "...", 4);
    return label;
}

static XrXirStatus escape_reject_origin(HirEscape *owner, uint32_t f, uint32_t i,
    HirEscapeOrigin restricted, const char *reason, XrHirEscapeDiagnostic *diagnostic) {
    if (!reason || (!restricted.parameter && !restricted.borrowed)) return XR_XIR_OK;
    diagnostic->function = f;
    diagnostic->instruction = i;
    diagnostic->code = restricted.parameter ? XR_ERR_ANALYZE_FUNCTION_ESCAPE : XR_ERR_ANALYZE_BORROW_ESCAPE;
    if (restricted.parameter) {
        uint32_t origin = restricted.parameter - 1, function = 0;
        while (function + 1 < owner->module->function_count && owner->offsets[function + 1] <= origin) ++function;
        uint32_t parameter = origin - owner->offsets[function];
        const XrHirEscapeDeclaration *declaration = owner->declarations ? &owner->declarations[function] : NULL;
        const char *name = declaration && declaration->names && parameter >= declaration->captures &&
            parameter - declaration->captures < declaration->count ? declaration->names[parameter - declaration->captures] : "callback";
        char label[100];
        name = escape_parameter_label(name, label);
        snprintf(diagnostic->message, sizeof(diagnostic->message),
            "E0398: parameter '%s' cannot escape through %s. Declare '%s: escaping fn(...)'", name, reason, name);
    } else snprintf(diagnostic->message, sizeof(diagnostic->message), "E0383: borrowed closure capture cannot escape through %s", reason);
    return XR_XIR_BAD_TYPE;
}

static XrXirStatus escape_reject(HirEscape *owner, uint32_t f, uint32_t i,
    uint32_t value, const char *reason, XrHirEscapeDiagnostic *diagnostic) {
    uint32_t index = escape_node(owner, f, value);
    if (owner->status != XR_XIR_OK) return owner->status;
    HirEscapeNode node = owner->nodes[index];
    return escape_reject_origin(owner, f, i, (HirEscapeOrigin){node.parameter, node.borrowed}, reason, diagnostic);
}

static XrXirStatus escape_use(HirEscape *owner, uint32_t f, uint32_t i,
    uint32_t value, uint32_t storage, const char *reason) {
    if (!reason) return XR_XIR_OK;
    if (owner->use_count == owner->use_capacity) {
        HirEscapeUse *grown = xir_compile_array_grow(owner->memory, owner->uses,
            owner->use_count, sizeof(*grown), &owner->use_capacity, &owner->status);
        if (!grown) return owner->status;
        owner->uses = grown;
    }
    owner->uses[owner->use_count++] = (HirEscapeUse){f, i, value, storage, reason};
    return XR_XIR_OK;
}

static XrXirStatus escape_record_uses(HirEscape *owner, uint32_t f, uint32_t i, const XrXirInstruction *op) {
    const XrXirFunction *body = &owner->module->functions[f];
    switch (op->op) {
    case XR_XIR_RETURN: return body->result == XR_XIR_UNIT ? XR_XIR_OK :
        escape_use(owner, f, i, op->args[0], UINT32_MAX, "a return");
    case XR_XIR_GENERATOR_YIELD:
        return escape_use(owner, f, i, op->args[0], UINT32_MAX, "a task or generator");
    case XR_XIR_NULLABLE_SOME: return escape_use(owner, f, i, op->args[0], UINT32_MAX, "nullable storage");
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE:
        return escape_use(owner, f, i, op->args[0], UINT32_MAX, "module storage");
    case XR_XIR_ARRAY_REPEAT: case XR_XIR_ARRAY_PUSH: case XR_XIR_CLASS_SET: case XR_XIR_STRUCT_SET: case XR_XIR_SET_ADD:
        return escape_use(owner, f, i, op->args[1], UINT32_MAX, "field or collection storage");
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_PLACE_WRITE: {
        uint32_t storage = escape_node(owner, f, op->args[0]);
        if (owner->status != XR_XIR_OK) return owner->status;
        return escape_use(owner, f, i, op->args[1], storage, "a ref or stored place");
    }
    default: break;
    }
    bool call = op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE || op->op == XR_XIR_CALL_INDIRECT ||
        op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT ||
        op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_CLEANUP_REGISTER ||
        op->op == XR_XIR_GO || op->op == XR_XIR_SCOPED_GO || op->op == XR_XIR_GENERATOR_NEW;
    bool store = op->op == XR_XIR_ARRAY_NEW || op->op == XR_XIR_ARRAY_SET || op->op == XR_XIR_MAP_NEW ||
        op->op == XR_XIR_MAP_SET || op->op == XR_XIR_SET_NEW || op->op == XR_XIR_STRUCT_NEW ||
        op->op == XR_XIR_CLASS_NEW || op->op == XR_XIR_TUPLE_NEW || op->op == XR_XIR_ENUM_NEW ||
        op->op == XR_XIR_SLOT_GROUP_INIT;
    if (!call && !store) return XR_XIR_OK;
    for (uint32_t a = 0; a < op->args[1]; ++a) {
        if (!escape_poll(owner)) return owner->status;
        if (op->op == XR_XIR_SCOPED_GO && !a) continue;
        const char *reason = call ? escape_argument_reason(owner, f, op, a) : "aggregate or collection storage";
        XrXirStatus status = escape_use(owner, f, i, body->operands[op->args[0] + a], UINT32_MAX, reason);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static bool escape_facts_publish(HirEscape *owner, XrHirEscapeFacts **output) {
    XrHirEscapeFacts *facts = escape_facts_new(owner->memory, owner->module->function_count, &owner->status);
    for (uint32_t f = 0; facts && f < facts->count && owner->status == XR_XIR_OK; ++f) {
        uint32_t count = owner->offsets[f + 1] - owner->offsets[f];
        uint32_t first = owner->offsets[f], end = owner->offsets[f + 1], flows = 0;
        for (uint32_t v = first; v < end && escape_poll(owner); ++v)
            for (uint32_t e = owner->nodes[v].head; e != UINT32_MAX && escape_poll(owner); e = owner->edges[e].next)
                if ((owner->edges[e].flags & ESCAPE_RESTRICTION) &&
                    owner->edges[e].target >= first && owner->edges[e].target < end) ++flows;
        facts->rows[f].flags = count ? xir_compile_calloc(owner->memory, count, 1, &owner->status) : NULL;
        facts->rows[f].count = count;
        facts->rows[f].flows = flows ? xir_compile_calloc(owner->memory, flows, sizeof(*facts->rows[f].flows), &owner->status) : NULL;
        facts->rows[f].flow_count = flows;
        for (uint32_t v = 0; v < count && escape_poll(owner); ++v) {
            HirEscapeNode node = owner->nodes[owner->offsets[f] + v];
            facts->rows[f].flags[v] = (uint8_t)((node.parameter ? 1u : 0u) | (node.borrowed ? 2u : 0u));
        }
        uint32_t at = 0;
        for (uint32_t v = first; v < end && escape_poll(owner); ++v)
            for (uint32_t e = owner->nodes[v].head; e != UINT32_MAX && escape_poll(owner); e = owner->edges[e].next)
                if ((owner->edges[e].flags & ESCAPE_RESTRICTION) &&
                    owner->edges[e].target >= first && owner->edges[e].target < end)
                    facts->rows[f].flows[at++] = (XrXirValueFlow){v - first, owner->edges[e].target - first};
    }
    if (owner->status != XR_XIR_OK) {xr_hir_escape_facts_free(facts); return false;}
    *output = facts;
    return true;
}

XR_FUNC void xr_hir_escape_builder_free(XrHirEscapeBuilder *builder) {
    if (!builder) return;
    escape_points_free(builder);
    if (builder->declarations) for (uint32_t f = 0; f < builder->module.function_count; ++f) {
        XrHirEscapeDeclaration row = builder->declarations[f];
        if (row.names) for (uint32_t p = 0; p < row.count; ++p)
            xr_compile_resources_free((void *)row.names[p]);
        xr_compile_resources_free((void *)row.names);
    }
    xr_compile_resources_free(builder->declarations);
    xr_compile_resources_free(builder->observed);
    xr_compile_resources_free(builder->observations);
    xr_compile_resources_free(builder->graph.nodes);
    xr_compile_resources_free(builder->graph.functions);
    xr_compile_resources_free(builder->graph.edges);
    xr_compile_resources_free(builder->graph.offsets);
    xr_compile_resources_free(builder->graph.queue);
    xr_compile_resources_free(builder->graph.uses);
    xr_compile_resources_free(builder->graph.captures);
    xr_compile_resources_free(builder);
}

static bool escape_declaration_copy(XrHirEscapeBuilder *builder, const XrHirEscapeDeclaration *input) {
    HirEscape *owner = &builder->graph;
    uint32_t count = builder->module.function_count;
    builder->declarations = count ? xir_compile_calloc(owner->memory, count,
        sizeof(*builder->declarations), &owner->status) : NULL;
    if (count && !builder->declarations) return false;
    owner->declarations = builder->declarations;
    for (uint32_t f = 0; f < count && escape_poll(owner); ++f) {
        uint32_t physical = builder->module.functions[f].parameter_count;
        if (input[f].captures > physical || input[f].count > physical - input[f].captures) {
            owner->status = XR_XIR_BAD_STRUCTURE; break;
        }
        builder->declarations[f] = input[f];
        builder->declarations[f].names = NULL;
        if (!input[f].names) continue;
        const char **names = input[f].count ? xir_compile_calloc(owner->memory, input[f].count,
            sizeof(*names), &owner->status) : NULL;
        builder->declarations[f].names = names;
        for (uint32_t p = 0; p < input[f].count && escape_poll(owner); ++p) {
            size_t length = 0;
            owner->status = xir_receive_strlen(owner->control, input[f].names[p], &length);
            if (owner->status != XR_XIR_OK) break;
            char *text = xir_compile_alloc(owner->memory, length + 1, &owner->status);
            if (!text) break;
            names[p] = text;
            owner->status = xir_receive_copy(owner->control, text, input[f].names[p], length + 1);
        }
    }
    return owner->status == XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_builder_new(const XrXirCompileContext *memory, const XrXirModule *module,
    const XrHirEscapeDeclaration *declarations, XrCompileRequestControl *control, XrHirEscapeBuilder **output) {
    if (!xir_compile_context_valid(memory) || !module || !output || *output ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    XrHirEscapeBuilder *builder = xir_compile_calloc(memory, 1, sizeof(*builder), &status);
    if (!builder) return status;
    builder->memory = *memory; builder->module = *module;
    if (module->declarations) {
        builder->identities = *module->declarations;
        builder->module.declarations = &builder->identities;
    }
    HirEscape *owner = &builder->graph;
    *owner = (HirEscape){.memory = &builder->memory, .module = &builder->module,
        .control = control, .status = XR_XIR_OK};
    if (declarations) escape_declaration_copy(builder, declarations);
    if (owner->status == XR_XIR_OK) {
        builder->observed = module->function_count ? xir_compile_calloc(memory, module->function_count,
            sizeof(*builder->observed), &owner->status) : NULL;
        if (owner->status == XR_XIR_OK) escape_prepare(owner);
    }
    status = owner->status;
    if (status != XR_XIR_OK) {xr_hir_escape_builder_free(builder); return status;}
    *output = builder;
    return XR_XIR_OK;
}

static bool escape_observation(XrHirEscapeBuilder *builder, uint32_t f, uint32_t i,
    uint32_t value, XirStoragePointKind point) {
    HirEscape *owner = &builder->graph;
    if (value >= owner->functions[f].physical_values) {owner->status = XR_XIR_BAD_STRUCTURE; return false;}
    if (!xr_xir_callable_signature(owner->module->types, owner->nodes[owner->offsets[f] + value].type)) return true;
    for (uint32_t n = builder->observation_count; n; --n) {
        HirEscapeObservation row = builder->observations[n - 1];
        if (row.function != f || row.instruction != i) break;
        if (row.value == value && row.point == point) return true;
    }
    if (builder->observation_count == builder->observation_capacity) {
        HirEscapeObservation *grown = xir_compile_array_grow(owner->memory, builder->observations,
            builder->observation_count, sizeof(*grown), &builder->observation_capacity, &owner->status);
        if (!grown) return false;
        builder->observations = grown;
    }
    builder->observations[builder->observation_count++] = (HirEscapeObservation){f, i, value, point};
    return true;
}

static bool escape_observe_points(XrHirEscapeBuilder *builder, uint32_t f, uint32_t i,
    const XrXirInstruction *op, uint32_t use_first) {
    HirEscape *owner = &builder->graph;
    for (uint32_t u = use_first; u < owner->use_count; ++u)
        if (!escape_observation(builder, f, i, owner->uses[u].value,
            op->op == XR_XIR_RETURN ? XIR_STORAGE_NORMAL : XIR_STORAGE_BEFORE)) return false;
    if (op->op == XR_XIR_FUNCTION_WEAKEN &&
        !escape_observation(builder, f, i, op->args[0], XIR_STORAGE_BEFORE)) return false;
    if ((op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) &&
        !escape_observation(builder, f, i, (uint32_t)op->immediate, XIR_STORAGE_BEFORE)) return false;
    bool call = xr_xir_op_references_function(op->op) || op->op == XR_XIR_CALL_INDIRECT ||
        op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT;
    if (call) for (uint32_t p = 0; p < op->args[1]; ++p)
        if (!escape_observation(builder, f, i, owner->module->functions[f].operands[op->args[0] + p],
            XIR_STORAGE_BEFORE)) return false;
    return true;
}

XR_FUNC XrXirStatus xr_hir_escape_builder_observe(XrHirEscapeBuilder *builder,
    uint32_t function, uint32_t instruction, const XrXirInstruction *op) {
    if (!builder || builder->finished || builder->point_preparation || !op || function >= builder->module.function_count ||
        instruction >= builder->module.functions[function].instruction_count ||
        op != &builder->module.functions[function].instructions[instruction] ||
        instruction != builder->observed[function]) return XR_XIR_BAD_STRUCTURE;
    HirEscape *owner = &builder->graph;
    if (!escape_poll(owner)) return owner->status;
    ++builder->observed[function];
    uint32_t use_first = owner->use_count;
    owner->nodes[owner->offsets[function] + owner->functions[function].parameters + instruction].type = op->type;
    if (escape_alias(owner, function, instruction, op) && escape_record_captures(owner, function, instruction, op))
        owner->status = escape_record_uses(owner, function, instruction, op);
    if (owner->status == XR_XIR_OK) escape_observe_points(builder, function, instruction, op, use_first);
    return owner->status;
}

#include "xhir_escape_terms.inc.c"

static XrXirStatus escape_builder_verdict(XrHirEscapeBuilder *builder,
    XrHirEscapeDiagnostic *diagnostic, XrHirEscapeFacts **output) {
    HirEscape *owner = &builder->graph;
    for (uint32_t f = 0; f < builder->module.function_count && escape_poll(owner); ++f)
        if (builder->observed[f] != builder->module.functions[f].instruction_count)
            owner->status = XR_XIR_BAD_STRUCTURE;
    if (owner->status == XR_XIR_OK && escape_close(owner) && escape_borrow_captures(owner) && escape_close(owner))
        for (uint32_t u = 0; u < owner->use_count && escape_poll(owner); ++u) {
            HirEscapeUse use = owner->uses[u];
            if (use.storage != UINT32_MAX && owner->nodes[use.storage].storage == ESCAPE_LOCAL) continue;
            owner->status = escape_reject(owner, use.function, use.instruction, use.value, use.reason, diagnostic);
            if (owner->status != XR_XIR_OK) break;
        }
    if (owner->status == XR_XIR_OK && output) escape_facts_publish(owner, output);
    return owner->status;
}

XR_FUNC XrXirStatus xr_hir_escape_check(const XrXirCompileContext *memory, const XrXirModule *module,
    const XrHirEscapeDeclaration *declarations, XrCompileRequestControl *control,
    XrHirEscapeDiagnostic *diagnostic, XrHirEscapeFacts **output) {
    if (!diagnostic || (output && *output)) return XR_XIR_BAD_STRUCTURE;
    *diagnostic = (XrHirEscapeDiagnostic){0};
    XrHirEscapeBuilder *builder = NULL;
    XrXirStatus status = xr_hir_escape_builder_new(memory, module, declarations, control, &builder);
    for (uint32_t f = 0; builder && f < module->function_count && status == XR_XIR_OK; ++f) {
        const XrXirFunction *body = &module->functions[f];
        for (uint32_t i = 0; i < body->instruction_count && status == XR_XIR_OK; ++i)
            status = xr_hir_escape_builder_observe(builder, f, i, &body->instructions[i]);
    }
    if (status == XR_XIR_OK) status = escape_builder_verdict(builder, diagnostic, output);
    xr_hir_escape_builder_free(builder);
    return status;
}
