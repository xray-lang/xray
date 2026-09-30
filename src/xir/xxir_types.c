/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_types.c - Bounded structural type admission and owned snapshots
 *
 * KEY CONCEPT:
 *   Earlier node edges form a finite graph whose exact contracts have one identity.
 */
#include "xxir_types.h"
#include "../base/xmalloc.h"

XrXirStatus xr_xir_callable_weakening(const XrXirTypes *types,
    XrXirType source, XrXirType target, uint64_t *work) {
    const XrXirTypeNode *from = xr_xir_callable_signature(types, source);
    const XrXirTypeNode *to = xr_xir_callable_signature(types, target);
    if (!from || !to || !work || from->flags != XR_XIR_CALLABLE_NO_SUSPEND || to->flags ||
        from->parameter_count != to->parameter_count || from->result != to->result) return XR_XIR_BAD_TYPE;
    uint64_t cost = (uint64_t)from->parameter_count + 1;
    if (cost > *work) return XR_XIR_BUDGET;
    *work -= cost;
    for (uint32_t p = 0; p < from->parameter_count; ++p)
        if (from->parameters[p].type != to->parameters[p].type ||
            from->parameters[p].mode != to->parameters[p].mode) return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus nominal_identities_verify(const XrXirNominalTable *table, XrXirBudget *budget);
static XrXirStatus nominal_copy_identities(const XrXirNominalTable *table, XrXirNominalTable **output);
static bool nominal_identity_copy_work(const XrXirNominalTable *table, XrXirBudget *budget);
static void nominal_free_identities(XrXirNominalTable *table);
static XrXirStatus nominal_table_verify(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirBudget *budget);
static XrXirStatus nominal_nodes_verify(const XrXirTypes *types, XrXirBudget *budget);
static XrXirStatus nominal_layout_verify(const XrXirTypes *types, XrXirBudget *budget);
static XrXirStatus nominal_copy_table(const XrXirNominalTable *table,
    XrXirNominalTable **output);

const XrXirTypeNode *xr_xir_type_node(const XrXirTypes *types, XrXirType type) {
    uint32_t id = (uint32_t) type;
    return types && types->nodes && id >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
        id < XR_XIR_CONSTRUCTED_TYPE_LIMIT && id - XR_XIR_CONSTRUCTED_TYPE_BASE < types->count ?
        &types->nodes[id - XR_XIR_CONSTRUCTED_TYPE_BASE] : NULL;
}
static bool type_has_kind(const XrXirTypes *types, XrXirType type, uint32_t kind) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == kind;
}
bool xr_xir_type_is_callable(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_CALLABLE);
}
bool xr_xir_type_is_array(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_ARRAY);
}
bool xr_xir_type_is_cell(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_CELL);
}
bool xr_xir_type_is_nominal(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_NOMINAL);
}
static bool nominal_has_kind(const XrXirTypes *types, XrXirType type, uint32_t kind) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    const XrXirNominalTable *table = types ? types->nominals : NULL;
    if (!node || node->kind != XR_XIR_TYPE_NOMINAL || !table ||
        node->nominal.declaration >= table->count ||
        (table->declarations != NULL) == (table->identities != NULL)) return false;
    uint32_t declaration = node->nominal.declaration;
    return (table->declarations ? table->declarations[declaration].kind :
        table->identities[declaration].kind) == kind;
}
XR_FUNC bool xr_xir_type_is_struct(const XrXirTypes *types, XrXirType type) {
    return nominal_has_kind(types, type, XR_XIR_NOMINAL_STRUCT);
}
XR_FUNC bool xr_xir_type_is_enum(const XrXirTypes *types, XrXirType type) {
    return nominal_has_kind(types, type, XR_XIR_NOMINAL_ENUM);
}
bool xr_xir_type_is_owned(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 || type == XR_XIR_ERROR ||
        type == XR_XIR_PANIC_INFO || (node && (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY ||
                  node->kind == XR_XIR_TYPE_CELL || node->kind == XR_XIR_TYPE_NOMINAL));
}
const XrXirTypeNode *xr_xir_callable_signature(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == XR_XIR_TYPE_CALLABLE ? node : NULL;
}
static XrXirType type_element(const XrXirTypes *types, XrXirType type, uint32_t kind) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == kind ? node->element : XR_XIR_UNIT;
}
XrXirType xr_xir_cell_element(const XrXirTypes *types, XrXirType type) {
    return type_element(types, type, XR_XIR_TYPE_CELL);
}
XrXirType xr_xir_array_element(const XrXirTypes *types, XrXirType type) {
    return type_element(types, type, XR_XIR_TYPE_ARRAY);
}
uint32_t xr_xir_type_span(const XrXirTypes *types, XrXirType type) {
    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE + 1;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node ? node->parameter_span : 0;
}
XrXirStatus xr_xir_type_markers(const XrXirTypes *types, XrXirType type, uint32_t required,
    const XrXirConstraint *constraints, uint32_t parameter_count, uint64_t *work) {
    if (required & ~XR_XIR_CONSTRAINT_MASK) return XR_XIR_BAD_TYPE;
    if (required & XR_XIR_CONSTRAINT_ERROR) {
        if (!work || !*work) return XR_XIR_BUDGET;
        --*work;
        uint32_t id = (uint32_t) type;
        bool parameter = id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT;
        if (parameter ? (!constraints || id - XR_XIR_TYPE_PARAMETER_BASE >= parameter_count ||
            !(constraints[id - XR_XIR_TYPE_PARAMETER_BASE].markers & XR_XIR_CONSTRAINT_ERROR)) :
            (type != XR_XIR_ERROR && !xr_xir_type_is_enum(types, type))) return XR_XIR_BAD_TYPE;
    }
    if (!(required & XR_XIR_CONSTRAINT_SENDABLE)) return XR_XIR_OK;
    for (;;) {
        if (!work || !*work) return XR_XIR_BUDGET;
        --*work;
        uint32_t id = (uint32_t) type;
        if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
            type == XR_XIR_ATOMIC_I64) return XR_XIR_OK;
        if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
            return constraints && id - XR_XIR_TYPE_PARAMETER_BASE < parameter_count &&
                (constraints[id - XR_XIR_TYPE_PARAMETER_BASE].markers & XR_XIR_CONSTRAINT_SENDABLE) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        if (!node || node->kind != XR_XIR_TYPE_ARRAY) return XR_XIR_BAD_TYPE;
        if ((uint32_t) node->element >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
            (uint32_t) node->element < XR_XIR_CONSTRUCTED_TYPE_LIMIT && (uint32_t) node->element >= id)
            return XR_XIR_BAD_TYPE;
        type = node->element;
    }
}
static bool type_component(const XrXirTypes *types, XrXirType type, uint32_t earlier) {
    uint32_t id = (uint32_t) type;
    if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
        type == XR_XIR_ATOMIC_I64 || type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO) return true;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) return true;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && id - XR_XIR_CONSTRUCTED_TYPE_BASE < earlier &&
        (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY);
}
static bool callable_component(const XrXirTypes *types, XrXirType type, uint32_t earlier) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return type_component(types, type, earlier) || (node && node->kind == XR_XIR_TYPE_NOMINAL &&
        (uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE < earlier);
}
static XrXirStatus type_payload(const XrXirTypes *types, uint32_t index, XrXirBudget *remaining) {
    const XrXirTypeNode *node = &types->nodes[index];
    uint32_t span = 0;
    if (node->kind != XR_XIR_TYPE_NOMINAL &&
        (node->nominal.declaration || node->nominal.arguments || node->nominal.argument_count ||
         node->nominal.fields || node->nominal.field_count)) return XR_XIR_BAD_STRUCTURE;
    if (node->kind == XR_XIR_TYPE_CALLABLE) {
        if (node->element != XR_XIR_UNIT || (node->flags & ~XR_XIR_CALLABLE_NO_SUSPEND) ||
            (node->result != XR_XIR_UNIT && !callable_component(types, node->result, index))) return XR_XIR_BAD_TYPE;
        if (node->parameter_count > 65536 || node->parameter_count > remaining->parameters) return XR_XIR_BUDGET;
        uint64_t bytes = (uint64_t) node->parameter_count * sizeof(*node->parameters);
        if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || node->parameter_count > remaining->work)
            return XR_XIR_BUDGET;
        remaining->metadata_bytes -= bytes; remaining->work -= node->parameter_count;
        remaining->parameters -= node->parameter_count;
        if ((node->parameter_count != 0) != (node->parameters != NULL)) return XR_XIR_BAD_STRUCTURE;
        span = xr_xir_type_span(types, node->result);
        for (uint32_t p = 0; p < node->parameter_count; ++p) {
            if (node->parameters[p].mode || !callable_component(types, node->parameters[p].type, index)) return XR_XIR_BAD_TYPE;
            uint32_t component = xr_xir_type_span(types, node->parameters[p].type);
            if (component > span) span = component;
        }
    } else if (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CELL) {
        if (node->parameters || node->parameter_count || node->result != XR_XIR_UNIT || node->flags)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *element = xr_xir_type_node(types, node->element);
        bool nominal_element = element &&
            element->kind == XR_XIR_TYPE_NOMINAL &&
            (uint32_t) node->element - XR_XIR_CONSTRUCTED_TYPE_BASE < index;
        if (!nominal_element && !type_component(types, node->element, index)) return XR_XIR_BAD_TYPE;
        span = xr_xir_type_span(types, node->element);
    } else if (node->kind == XR_XIR_TYPE_NOMINAL) {
        if (!types->nominals || (!types->nominals->declarations && !types->nominals->identities) ||
            node->nominal.declaration >= types->nominals->count) return XR_XIR_BAD_TYPE;
        if (node->element != XR_XIR_UNIT || node->parameters || node->parameter_count ||
            node->result != XR_XIR_UNIT || node->flags ||
            (node->nominal.argument_count != 0) != (node->nominal.arguments != NULL) ||
            (node->nominal.field_count != 0) != (node->nominal.fields != NULL)) return XR_XIR_BAD_STRUCTURE;
        uint32_t count = node->nominal.argument_count;
        uint64_t total = (uint64_t) count + node->nominal.field_count;
        uint64_t bytes = total * sizeof(XrXirType);
        if (count > 65536 || total > remaining->parameters || total > remaining->work ||
            bytes > SIZE_MAX || bytes > remaining->metadata_bytes) return XR_XIR_BUDGET;
        remaining->parameters -= (uint32_t) total; remaining->work -= total; remaining->metadata_bytes -= bytes;
        for (uint32_t a = 0; a < count; ++a) {
            XrXirType argument = node->nominal.arguments[a];
            const XrXirTypeNode *nested = xr_xir_type_node(types, argument);
            bool nominal = nested && nested->kind == XR_XIR_TYPE_NOMINAL &&
                (uint32_t) argument - XR_XIR_CONSTRUCTED_TYPE_BASE < index;
            if (!nominal && !type_component(types, argument, index)) return XR_XIR_BAD_TYPE;
            uint32_t component = xr_xir_type_span(types, argument);
            if (component > span) span = component;
        }
    } else return XR_XIR_BAD_TYPE;
    if (types->nominals && types->nominals->identities && span) return XR_XIR_BAD_TYPE;
    return node->parameter_span == span ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
static XrXirStatus type_unique(const XrXirTypes *types, uint32_t index, uint64_t *work) {
    const XrXirTypeNode *node = &types->nodes[index];
    for (uint32_t j = 0; j < index; ++j) {
        if (!*work) return XR_XIR_BUDGET;
        --*work;
        const XrXirTypeNode *previous = &types->nodes[j];
        if (previous->kind != node->kind || previous->element != node->element ||
            previous->parameter_count != node->parameter_count || previous->result != node->result ||
            previous->flags != node->flags || previous->nominal.declaration != node->nominal.declaration ||
            previous->nominal.argument_count != node->nominal.argument_count) continue;
        if (node->parameter_count > *work) return XR_XIR_BUDGET;
        *work -= node->parameter_count;
        bool same = true;
        for (uint32_t p = 0; p < node->parameter_count; ++p)
            if (node->parameters[p].type != previous->parameters[p].type ||
                node->parameters[p].mode != previous->parameters[p].mode) same = false;
        if (node->nominal.argument_count > *work) return XR_XIR_BUDGET;
        *work -= node->nominal.argument_count;
        for (uint32_t a = 0; a < node->nominal.argument_count; ++a)
            if (node->nominal.arguments[a] != previous->nominal.arguments[a]) same = false;
        if (same) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_types_verify(const XrXirTypes *types, XrXirBudget *remaining) {
    if (!types) return XR_XIR_OK;
    if (!remaining || (types->count != 0) != (types->nodes != NULL) ||
        (!types->count && !types->nominals)) return XR_XIR_BAD_STRUCTURE;
    if (types->count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_BUDGET;
    uint64_t bytes = sizeof(*types) + (uint64_t) types->count * sizeof(*types->nodes);
    if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || types->count > remaining->work) return XR_XIR_BUDGET;
    remaining->metadata_bytes -= bytes; remaining->work -= types->count;
    for (uint32_t i = 0; i < types->count; ++i) {
        XrXirStatus status = type_payload(types, i, remaining);
        if (status == XR_XIR_OK) status = type_unique(types, i, &remaining->work);
        if (status != XR_XIR_OK) return status;
    }
    XrXirStatus status = nominal_table_verify(types->nominals, types, remaining);
    if (status == XR_XIR_OK) status = nominal_nodes_verify(types, remaining);
    return status == XR_XIR_OK ? nominal_layout_verify(types, remaining) : status;
}
void xr_xir_types_free(XrXirTypes *types) {
    if (!types) return;
    if (types->nodes)
        for (uint32_t i = 0; i < types->count; ++i) {
            xr_free((void *) types->nodes[i].parameters);
            xr_free((void *) types->nodes[i].nominal.arguments);
            xr_free((void *) types->nodes[i].nominal.fields);
        }
    xr_xir_nominal_free((XrXirNominalTable *) types->nominals);
    xr_free((void *) types->nodes); xr_free(types);
}
XrXirStatus xr_xir_types_clone(const XrXirTypes *types, XrXirTypes **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!types) return XR_XIR_OK;
    if ((types->count != 0) != (types->nodes != NULL) || (!types->count && !types->nominals) ||
        types->count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_BAD_STRUCTURE;
    XrXirTypes *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    XrXirTypeNode *nodes = types->count ? xr_calloc(types->count, sizeof(*nodes)) : NULL;
    if (types->count && !nodes) { xr_free(copy); return XR_XIR_OUT_OF_MEMORY; }
    copy->nodes = nodes; copy->count = types->count;
    for (uint32_t i = 0; i < types->count; ++i) {
        nodes[i] = types->nodes[i];
        nodes[i].nominal.arguments = NULL; nodes[i].nominal.fields = NULL;
        uint32_t count = nodes[i].parameter_count;
        XrXirCallableParameter *parameters = count ? xr_malloc((size_t) count * sizeof(*parameters)) : NULL;
        nodes[i].parameters = parameters;
        if (count && !parameters) { xr_xir_types_free(copy); return XR_XIR_OUT_OF_MEMORY; }
        if (count) memcpy(parameters, types->nodes[i].parameters, (size_t) count * sizeof(*parameters));
        uint32_t arguments = nodes[i].nominal.argument_count;
        XrXirType *owned = arguments ? xr_malloc((size_t) arguments * sizeof(*owned)) : NULL;
        nodes[i].nominal.arguments = owned;
        if (arguments && !owned) { xr_xir_types_free(copy); return XR_XIR_OUT_OF_MEMORY; }
        if (arguments) memcpy(owned, types->nodes[i].nominal.arguments, (size_t) arguments * sizeof(*owned));
        uint32_t fields = nodes[i].nominal.field_count;
        XrXirType *owned_fields = fields ? xr_malloc((size_t) fields * sizeof(*owned_fields)) : NULL;
        nodes[i].nominal.fields = owned_fields;
        if (fields && !owned_fields) { xr_xir_types_free(copy); return XR_XIR_OUT_OF_MEMORY; }
        if (fields) memcpy(owned_fields, types->nodes[i].nominal.fields, (size_t) fields * sizeof(*owned_fields));
    }
    XrXirNominalTable *nominals = NULL;
    XrXirStatus status = nominal_copy_table(types->nominals, &nominals);
    if (status != XR_XIR_OK) { xr_xir_types_free(copy); return status; }
    copy->nominals = nominals;
    *output = copy; return XR_XIR_OK;
}
XrXirType xr_xir_operand_type(const XrXirFunction *function, uint32_t value) {
    if (value < function->parameter_count) return function->parameters[value];
    value -= function->parameter_count;
    return value < function->instruction_count ? function->instructions[value].type : XR_XIR_UNIT;
}

#include "xxir_type_match.inc.c"
#include "xxir_type_context.inc.c"
#include "xxir_nominal.inc.c"
#include "xxir_nominal_identity.inc.c"
#include "xxir_nominal_layout.inc.c"
