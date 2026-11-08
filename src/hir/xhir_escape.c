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
#include <stdio.h>

enum { ESCAPE_RESTRICTION = 1u, ESCAPE_STORAGE = 2u };
enum { ESCAPE_LOCAL = 1u, ESCAPE_EXTERNAL = 2u };

typedef struct HirEscapeNode {
    uint32_t head, parameter, borrowed;
    uint8_t storage;
    bool queued;
} HirEscapeNode;

typedef struct HirEscapeEdge { uint32_t target, next, flags; } HirEscapeEdge;

typedef struct HirEscape {
    const XrXirCompileContext *memory;
    const XrXirModule *module;
    const XrHirEscapeDeclaration *declarations;
    XrCompileRequestControl *control;
    HirEscapeNode *nodes;
    HirEscapeEdge *edges;
    uint32_t *offsets, *queue;
    uint32_t count, edge_count, edge_capacity, progress;
    uint32_t queue_first, queue_count;
    XrXirStatus status;
} HirEscape;

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
    if (!count) return true;
    owner->nodes = xir_compile_calloc(owner->memory, owner->count, sizeof(*owner->nodes), &owner->status);
    owner->queue = xir_compile_calloc(owner->memory, owner->count, sizeof(*owner->queue), &owner->status);
    if (!owner->nodes || !owner->queue) return false;
    for (uint32_t n = 0; n < owner->count; ++n) owner->nodes[n].head = UINT32_MAX;
    for (uint32_t f = 0; f < functions; ++f) {
        const XrXirFunction *body = &owner->module->functions[f];
        const XrHirEscapeDeclaration *declaration = &owner->declarations[f];
        if (declaration->captures > body->parameter_count ||
            declaration->count > body->parameter_count - declaration->captures) {
            owner->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        for (uint32_t p = 0; p < body->parameter_count; ++p) {
            uint32_t node = owner->offsets[f] + p;
            if (p >= declaration->captures && xr_xir_type_is_cell(owner->module->types, body->parameters[p]))
                owner->nodes[node].storage = ESCAPE_EXTERNAL;
            if (p >= declaration->captures && p - declaration->captures < declaration->count &&
                xr_xir_callable_signature(owner->module->types, body->parameters[p]) &&
                !xr_xir_function_parameter_escaping(body, p)) owner->nodes[node].parameter = node + 1;
        }
    }
    return true;
}

static bool escape_alias(HirEscape *owner, uint32_t f, uint32_t i, const XrXirInstruction *op) {
    const XrXirFunction *body = &owner->module->functions[f];
    uint32_t result = body->parameter_count + i;
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_CELL_READ: case XR_XIR_CELL_TAKE: case XR_XIR_VALUE_TAKE:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_CELL_PLACE: case XR_XIR_PLACE_READ:
        return escape_value_edge(owner, f, op->args[0], result, ESCAPE_RESTRICTION | ESCAPE_STORAGE);
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_NEW_TAKE: case XR_XIR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
        owner->nodes[owner->offsets[f] + result].storage = ESCAPE_LOCAL;
        return escape_value_edge(owner, f, op->args[0], result, ESCAPE_RESTRICTION);
    case XR_XIR_LOCAL_UNINIT:
        owner->nodes[owner->offsets[f] + result].storage = ESCAPE_LOCAL;
        return true;
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return escape_value_edge(owner, f, op->args[1], op->args[0], ESCAPE_RESTRICTION);
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
    default: return true;
    }
}

static const char *escape_argument_reason(HirEscape *owner, uint32_t f,
    const XrXirInstruction *op, uint32_t ordinal) {
    const XrXirFunction *body = &owner->module->functions[f];
    if (op->op == XR_XIR_GO || op->op == XR_XIR_SCOPED_GO || op->op == XR_XIR_GENERATOR_NEW)
        return "a task or generator";
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) {
        if (op->immediate < 0 || (uint64_t)op->immediate >= owner->module->function_count) return "an unknown callee";
        const XrXirFunction *target = &owner->module->functions[op->immediate];
        if (ordinal >= target->parameter_count) return "an unknown parameter";
        if (xr_xir_function_parameter_escaping(target, ordinal)) return "an escaping parameter";
        return xr_xir_callable_signature(owner->module->types, target->parameters[ordinal]) ? NULL : "a generic or stored value parameter";
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
    return xr_xir_callable_signature(owner->module->types, signature->parameters[ordinal].type) ? NULL : "a generic or stored value parameter";
}

static bool escape_borrow_captures(HirEscape *owner) {
    for (uint32_t f = 0; f < owner->module->function_count; ++f) {
        const XrXirFunction *body = &owner->module->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            if (!escape_poll(owner)) return false;
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op != XR_XIR_FUNCTION_REF) continue;
            uint32_t result = owner->offsets[f] + body->parameter_count + i;
            for (uint32_t a = 0; a < op->args[1]; ++a) {
                uint32_t value = body->operands[op->args[0] + a];
                uint32_t source = escape_node(owner, f, value), flags = 0;
                if (owner->status != XR_XIR_OK) return false;
                XrXirType type = xr_xir_operand_type(body, value);
                owner->status = xr_xir_compile_view_type_flags(owner->memory, owner->module->types, type, &flags);
                if (owner->status != XR_XIR_OK) return false;
                bool reference = xr_xir_type_is_cell(owner->module->types, type) &&
                    owner->nodes[source].storage != ESCAPE_LOCAL;
                if (flags || reference) owner->nodes[result].borrowed = source + 1;
            }
        }
    }
    return true;
}

static XrXirStatus escape_reject(HirEscape *owner, uint32_t f, uint32_t i,
    uint32_t value, const char *reason, XrHirEscapeDiagnostic *diagnostic) {
    uint32_t index = escape_node(owner, f, value);
    if (owner->status != XR_XIR_OK) return owner->status;
    HirEscapeNode restricted = owner->nodes[index];
    if (!reason || (!restricted.parameter && !restricted.borrowed)) return XR_XIR_OK;
    diagnostic->function = f;
    diagnostic->instruction = i;
    diagnostic->code = restricted.parameter ? XR_ERR_ANALYZE_FUNCTION_ESCAPE : XR_ERR_ANALYZE_BORROW_ESCAPE;
    if (restricted.parameter) {
        uint32_t origin = restricted.parameter - 1, function = 0;
        while (function + 1 < owner->module->function_count && owner->offsets[function + 1] <= origin) ++function;
        uint32_t parameter = origin - owner->offsets[function];
        const XrHirEscapeDeclaration *declaration = &owner->declarations[function];
        const char *name = declaration->names && parameter >= declaration->captures &&
            parameter - declaration->captures < declaration->count ? declaration->names[parameter - declaration->captures] : "callback";
        snprintf(diagnostic->message, sizeof(diagnostic->message),
            "E0398: parameter '%s' cannot escape through %s. Declare '%s: escaping fn(...)'", name, reason, name);
    } else snprintf(diagnostic->message, sizeof(diagnostic->message), "E0383: borrowed closure capture cannot escape through %s", reason);
    return XR_XIR_BAD_TYPE;
}

static XrXirStatus escape_uses(HirEscape *owner, uint32_t f, uint32_t i, XrHirEscapeDiagnostic *diagnostic) {
    const XrXirFunction *body = &owner->module->functions[f];
    const XrXirInstruction *op = &body->instructions[i];
    switch (op->op) {
    case XR_XIR_RETURN: return body->result == XR_XIR_UNIT ? XR_XIR_OK :
        escape_reject(owner, f, i, op->args[0], "a return", diagnostic);
    case XR_XIR_NULLABLE_SOME: return escape_reject(owner, f, i, op->args[0], "nullable storage", diagnostic);
    case XR_XIR_SLOT_STORE: return escape_reject(owner, f, i, op->args[0], "module storage", diagnostic);
    case XR_XIR_ARRAY_PUSH: case XR_XIR_CLASS_SET: case XR_XIR_STRUCT_SET: case XR_XIR_SET_ADD:
        return escape_reject(owner, f, i, op->args[1], "field or collection storage", diagnostic);
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_PLACE_WRITE: {
        uint32_t storage = escape_node(owner, f, op->args[0]);
        if (owner->status != XR_XIR_OK) return owner->status;
        return owner->nodes[storage].storage == ESCAPE_LOCAL ? XR_XIR_OK :
            escape_reject(owner, f, i, op->args[1], "a ref or stored place", diagnostic);
    }
    default: break;
    }
    bool call = op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE || op->op == XR_XIR_CALL_INDIRECT ||
        op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT ||
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
        XrXirStatus status = escape_reject(owner, f, i, body->operands[op->args[0] + a], reason, diagnostic);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_check(const XrXirCompileContext *memory, const XrXirModule *module,
    const XrHirEscapeDeclaration *declarations, XrCompileRequestControl *control, XrHirEscapeDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(memory) || !module || !diagnostic ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    *diagnostic = (XrHirEscapeDiagnostic){0};
    HirEscape owner = {.memory = memory, .module = module, .declarations = declarations,
        .control = control, .status = XR_XIR_OK};
    XrHirEscapeDeclaration *external = NULL;
    if (!declarations && module->function_count) {
        external = xir_compile_calloc(memory, module->function_count, sizeof(*external), &owner.status);
        if (external) {
            for (uint32_t f = 0; f < module->function_count; ++f) {
                const XrXirFunction *body = &module->functions[f];
                for (uint32_t i = 0; i < body->instruction_count && escape_poll(&owner); ++i) {
                    const XrXirInstruction *op = &body->instructions[i];
                    if (op->op != XR_XIR_FUNCTION_REF && op->op != XR_XIR_CLEANUP_REGISTER) continue;
                    if (op->immediate < 0 || (uint64_t)op->immediate >= module->function_count) {
                        owner.status = XR_XIR_BAD_STRUCTURE;
                        break;
                    }
                    uint32_t target = (uint32_t)op->immediate;
                    if (external[target].captures < op->args[1]) external[target].captures = op->args[1];
                }
            }
            for (uint32_t f = 0; f < module->function_count && owner.status == XR_XIR_OK; ++f) {
                if (external[f].captures > module->functions[f].parameter_count) owner.status = XR_XIR_BAD_STRUCTURE;
                else external[f].count = module->functions[f].parameter_count - external[f].captures;
            }
            owner.declarations = external;
        }
    }
    if (owner.status == XR_XIR_OK && escape_prepare(&owner)) {
        for (uint32_t f = 0; f < module->function_count && owner.status == XR_XIR_OK; ++f) {
            const XrXirFunction *body = &module->functions[f];
            for (uint32_t i = 0; i < body->instruction_count && escape_poll(&owner); ++i)
                if (!escape_alias(&owner, f, i, &body->instructions[i])) break;
        }
        if (owner.status == XR_XIR_OK && escape_close(&owner) &&
            escape_borrow_captures(&owner) && escape_close(&owner))
            for (uint32_t f = 0; f < module->function_count && owner.status == XR_XIR_OK; ++f)
                for (uint32_t i = 0; i < module->functions[f].instruction_count && escape_poll(&owner); ++i) {
                    owner.status = escape_uses(&owner, f, i, diagnostic);
                    if (owner.status != XR_XIR_OK) break;
                }
    }
    xr_compile_resources_free(owner.nodes);
    xr_compile_resources_free(owner.edges);
    xr_compile_resources_free(owner.offsets);
    xr_compile_resources_free(owner.queue);
    xr_compile_resources_free(external);
    return owner.status;
}
