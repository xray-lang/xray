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
#include "../xir/xxir_channel.h"
#include "../xir/xxir_receive_control.h"
#include "../shared/xerror_codes.h"
#include "../shared/xr_utf8_core.h"
#include "../base/xhash.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct HirEscapeNode {
    uint32_t head, parameter;
    XrXirType type;
} HirEscapeNode;
typedef struct HirEscapeOrigin { uint32_t parameter, borrowed; } HirEscapeOrigin;
typedef struct HirEscapeFunction { uint32_t parameters, captures, physical_values; } HirEscapeFunction;

typedef struct HirEscapeEdge { uint32_t target, next; } HirEscapeEdge;
typedef enum HirEscapeUseKind { HIR_ESCAPE_FUNCTION_VALUE, HIR_ESCAPE_CELL_PUBLICATION } HirEscapeUseKind;
typedef struct HirEscapeUse {
    uint32_t function, instruction, value, storage;
    const char *reason;
    HirEscapeUseKind kind;
    XirStoragePointKind point;
} HirEscapeUse;
typedef struct HirEscapeCapture { uint32_t source, result; } HirEscapeCapture;
typedef struct HirEscapeObservation {
    uint32_t function, instruction, value;
    XirStoragePointKind point;
} HirEscapeObservation;

struct XrHirEscapeFacts { XirRestrictionView view; };

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
    uint32_t *offsets;
    uint32_t count, edge_count, edge_capacity, progress;
    uint32_t use_count, use_capacity, capture_count, capture_capacity;
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
    if (facts->view.functions) for (uint32_t f = 0; f < facts->view.function_count; ++f) {
        xr_compile_resources_free((void *)facts->view.functions[f].intrinsic);
        xr_compile_resources_free((void *)facts->view.functions[f].flows);
    }
    xr_compile_resources_free((void *)facts->view.functions);
    xr_compile_resources_free((void *)facts->view.formulas);
    xr_compile_resources_free((void *)facts->view.inputs);
    xr_compile_resources_free((void *)facts->view.points);
    xr_compile_resources_free(facts);
}

XR_FUNC XrXirStatus xr_hir_escape_facts_view(const XrHirEscapeFacts *facts, XirRestrictionView *output) {
    if (!facts || !output) return XR_XIR_BAD_STRUCTURE;
    *output = facts->view;
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

static bool escape_edge(HirEscape *owner, uint32_t source, uint32_t target) {
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
    owner->edges[index] = (HirEscapeEdge){target, owner->nodes[source].head};
    owner->nodes[source].head = index;
    return true;
}

static bool escape_value_edge(HirEscape *owner, uint32_t function,
    uint32_t source, uint32_t target) {
    uint32_t from = escape_node(owner, function, source);
    uint32_t to = escape_node(owner, function, target);
    return owner->status == XR_XIR_OK && escape_edge(owner, from, to);
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
    if (!owner->nodes) return false;
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
            if (p >= captures && xr_xir_callable_signature(owner->module->types, body->parameters[p]) &&
                !xr_xir_type_is_c_callable(owner->module->types, body->parameters[p]) &&
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
    case XR_XIR_VALUE_TAKE: case XR_XIR_CELL_PLACE:
        return escape_value_edge(owner, f, op->args[0], result);
    case XR_XIR_PHI:
        for (uint32_t a = 1; a < op->args[1]; a += 2)
            if (!escape_value_edge(owner, f, body->operands[op->args[0] + a], result)) return false;
        return true;
    case XR_XIR_FUNCTION_REF:
        if (op->immediate < 0 || (uint64_t)op->immediate >= owner->module->function_count) {
            owner->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        for (uint32_t a = 0; a < op->args[1]; ++a)
            if (!escape_value_edge(owner, f, body->operands[op->args[0] + a], result)) return false;
        return true;
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
        if (xr_xir_type_is_cell(owner->module->types, target->parameters[ordinal])) return NULL;
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
    if (xr_xir_type_is_cell(owner->module->types, signature->parameters[ordinal].type)) return NULL;
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
        if (!flags) continue;
        if (owner->capture_count == owner->capture_capacity) {
            HirEscapeCapture *grown = xir_compile_array_grow(owner->memory, owner->captures,
                owner->capture_count, sizeof(*grown), &owner->capture_capacity, &owner->status);
            if (!grown) return false;
            owner->captures = grown;
        }
        owner->captures[owner->capture_count++] = (HirEscapeCapture){source, result};
    }
    return owner->status == XR_XIR_OK;
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

static XrXirStatus escape_use(HirEscape *owner, uint32_t f, uint32_t i,
    uint32_t value, uint32_t storage, const char *reason) {
    if (!reason) return XR_XIR_OK;
    if (owner->use_count == owner->use_capacity) {
        HirEscapeUse *grown = xir_compile_array_grow(owner->memory, owner->uses,
            owner->use_count, sizeof(*grown), &owner->use_capacity, &owner->status);
        if (!grown) return owner->status;
        owner->uses = grown;
    }
    owner->uses[owner->use_count++] = (HirEscapeUse){f, i, value, storage, reason,
        HIR_ESCAPE_FUNCTION_VALUE, XIR_STORAGE_BEFORE};
    return XR_XIR_OK;
}

static XrXirStatus escape_record_uses(HirEscape *owner, uint32_t f, uint32_t i, const XrXirInstruction *op) {
    const XrXirFunction *body = &owner->module->functions[f];
    switch (op->op) {
    case XR_XIR_RETURN: {
        if (body->result == XR_XIR_UNIT) return XR_XIR_OK;
        XrXirStatus status = escape_use(owner, f, i, op->args[0], UINT32_MAX, "a return");
        if (status == XR_XIR_OK) owner->uses[owner->use_count - 1].point = XIR_STORAGE_NORMAL;
        return status;
    }
    case XR_XIR_GENERATOR_YIELD:
        return escape_use(owner, f, i, op->args[0], UINT32_MAX, "a task or generator");
    case XR_XIR_NULLABLE_SOME: return escape_use(owner, f, i, op->args[0], UINT32_MAX, "nullable storage");
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE: {
        if (!owner->module->declarations) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_slot_is_unit(owner->module->declarations->slots,
            owner->module->declarations->slot_count, op)) return XR_XIR_OK;
        XrXirStatus status = escape_use(owner, f, i, op->args[0], UINT32_MAX, "module storage");
        if (status == XR_XIR_OK && xr_xir_type_is_cell(owner->module->types, xr_xir_operand_type(body, op->args[0])))
            owner->uses[owner->use_count - 1].kind = HIR_ESCAPE_CELL_PUBLICATION;
        return status;
    }
    case XR_XIR_CHANNEL_TRY_SEND: case XR_XIR_CHANNEL_SEND: case XR_XIR_CHANNEL_SEND_TIMEOUT:
    case XR_XIR_CHANNEL_RECV_OR: {
        uint32_t channel = body->operands[op->args[0]];
        if (xr_xir_channel_element(owner->module->types, xr_xir_operand_type(body, channel)) == XR_XIR_UNIT) return XR_XIR_OK;
        return escape_use(owner, f, i, body->operands[op->args[0] + 1], UINT32_MAX,
            op->op == XR_XIR_CHANNEL_RECV_OR ? "a generic or stored value parameter" : "channel storage");
    }
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
        if (reason && (op->op == XR_XIR_GO || op->op == XR_XIR_SCOPED_GO || op->op == XR_XIR_GENERATOR_NEW ||
            op->op == XR_XIR_SLOT_GROUP_INIT) &&
            xr_xir_type_is_cell(owner->module->types, xr_xir_operand_type(body, body->operands[op->args[0] + a])))
            owner->uses[owner->use_count - 1].kind = HIR_ESCAPE_CELL_PUBLICATION;
    }
    return XR_XIR_OK;
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
    if (xr_xir_place_kind(&owner->module->functions[f], value) != XR_XIR_PLACE_NONE) return true;
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
        if (owner->uses[u].kind == HIR_ESCAPE_FUNCTION_VALUE &&
            !escape_observation(builder, f, i, owner->uses[u].value, owner->uses[u].point)) return false;
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
