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
#include "xxir_type_match_internal.h"
#include "xxir_compile_memory.h"

XR_FUNC XrXirCompileLimits xr_xir_compile_default_limits(void) {
    return (XrXirCompileLimits){1024, 65536, 4096, 1048576, UINT64_C(16) * 1024 * 1024};
}
#include "xxir_interface.h"
#include "xxir_constraints.h"
#include "xxir_constraint_proof.h"
#include "../base/xmalloc.h"
#include "../shared/xnative_declaration.h"

XrXirStatus xr_xir_callable_weakening_admit(const XrXirTypes *types,
    XrXirType source, XrXirType target, void *work_owner, bool (*charge)(void *, uint64_t)) {
    if (!work_owner || !charge) return XR_XIR_BAD_STRUCTURE;
    if (!charge(work_owner, 1)) return XR_XIR_BUDGET;
    const XrXirTypeNode *from = xr_xir_callable_signature(types, source);
    const XrXirTypeNode *to = xr_xir_callable_signature(types, target);
    if (!from || !to || from->flags != XR_XIR_CALLABLE_NO_SUSPEND || to->flags ||
        from->parameter_count != to->parameter_count || from->result != to->result) return XR_XIR_BAD_TYPE;
    for (uint32_t p = 0; p < from->parameter_count; ++p) {
        if (!charge(work_owner, 1)) return XR_XIR_BUDGET;
        if (from->parameters[p].type != to->parameters[p].type ||
            from->parameters[p].mode != to->parameters[p].mode) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static bool callable_compile_charge(void *owner, uint64_t work) {
    return xr_compile_resources_work(owner, work) == XR_COMPILE_RESOURCE_OK;
}
XrXirStatus xr_xir_compile_callable_weakening(const XrXirCompileContext *compile_context,
    const XrXirTypes *types, XrXirType source, XrXirType target) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    return xr_xir_callable_weakening_admit(types, source, target,
        compile_context->resources, callable_compile_charge);
}
static XrXirStatus nominal_identities_verify(const XrXirNominalTable *table, XrXirCompileContext *budget);
static XrXirStatus nominal_copy_identities(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, XrXirNominalTable **output);
static void nominal_free_identities(XrXirNominalTable *table);
static XrXirStatus nominal_table_verify(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirCompileContext *budget);
static XrXirStatus nominal_nodes_verify(const XrXirTypes *types, XrXirCompileContext *budget);
static XrXirStatus nominal_layout_verify(const XrXirTypes *types, XrXirCompileContext *budget);
static XrXirStatus nominal_copy_table(const XrXirCompileContext *compile_context, const XrXirNominalTable *table,
    XrXirNominalTable **output);

const XrXirTypeNode *xr_xir_type_node(const XrXirTypes *types, XrXirType type) {
    uint32_t id = (uint32_t) type;
    return types && types->nodes && id >= XR_XIR_CONSTRUCTED_TYPE_BASE &&
        id < XR_XIR_CONSTRUCTED_TYPE_LIMIT && id - XR_XIR_CONSTRUCTED_TYPE_BASE < types->count ?
        &types->nodes[id - XR_XIR_CONSTRUCTED_TYPE_BASE] : NULL;
}
XR_FUNC bool xr_xir_type_is_atomic(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == XR_XIR_TYPE_ATOMIC;
}
XR_FUNC XrXirType xr_xir_atomic_element(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == XR_XIR_TYPE_ATOMIC ? node->element : XR_XIR_UNIT;
}
XR_FUNC const XrXirNominalNativeRecord *xr_xir_nominal_native_record(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    if (!node || node->kind != XR_XIR_TYPE_NOMINAL || !types->nominals ||
        node->nominal.declaration >= types->nominals->count) return NULL;
    const XrXirNominalTable *table = types->nominals;
    if (table->declarations && !table->identities) return &table->declarations[node->nominal.declaration].native;
    return table->identities && !table->declarations ? &table->identities[node->nominal.declaration].native : NULL;
}
XR_FUNC bool xr_xir_nominal_native_ordering(const XrXirTypes *types, XrXirType type) {
    if (!xr_xir_type_is_enum(types, type)) return false;
    const XrXirNominalNativeRecord *record = xr_xir_nominal_native_record(types, type);
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ORDERING);
    return record && record->native_id == XR_NATIVE_DECLARATION_ORDERING && native &&
        native->kind == XR_NATIVE_DECLARATION_VALUE &&
        !memcmp(record->source_fingerprint, native->source_fingerprint.bytes, sizeof(record->source_fingerprint));
}
static bool type_has_kind(const XrXirTypes *types, XrXirType type, uint32_t kind) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == kind;
}
bool xr_xir_type_is_callable(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_CALLABLE);
}
#include "xxir_class_field_cap.inc.c"
bool xr_xir_type_is_array(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_ARRAY);
}
XR_FUNC bool xr_xir_type_is_nullable(const XrXirTypes *types, XrXirType type) {
    return type_has_kind(types, type, XR_XIR_TYPE_NULLABLE);
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
XR_FUNC bool xr_xir_type_is_class(const XrXirTypes *types, XrXirType type) {
    return nominal_has_kind(types, type, XR_XIR_NOMINAL_CLASS);
}
bool xr_xir_type_is_owned(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return type == XR_XIR_STRING || type == XR_XIR_ERROR ||
        type == XR_XIR_PANIC_INFO || (node && (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY ||
                  node->kind == XR_XIR_TYPE_CELL || node->kind == XR_XIR_TYPE_NOMINAL ||
                  node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_TUPLE || node->kind == XR_XIR_TYPE_ATOMIC));
}
XR_FUNC const XrXirTypeNode *xr_xir_tuple_signature(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == XR_XIR_TYPE_TUPLE ? node : NULL;
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
XR_FUNC XrXirType xr_xir_nullable_element(const XrXirTypes *types, XrXirType type) {
    return type_element(types, type, XR_XIR_TYPE_NULLABLE);
}
uint32_t xr_xir_type_span(const XrXirTypes *types, XrXirType type) {
    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE + 1;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node ? node->parameter_span : 0;
}
static bool type_component(const XrXirTypes *types, XrXirType type, uint32_t earlier) {
    uint32_t id = (uint32_t) type;
    if (type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
        type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO) return true;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) return true;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && id - XR_XIR_CONSTRUCTED_TYPE_BASE < earlier &&
        (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY ||
         node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_TUPLE || node->kind == XR_XIR_TYPE_ATOMIC);
}
static bool callable_component(const XrXirTypes *types, XrXirType type, uint32_t earlier) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return type_component(types, type, earlier) || (node && node->kind == XR_XIR_TYPE_NOMINAL &&
        (uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE < earlier);
}
static XrXirStatus type_payload(const XrXirTypes *types, uint32_t index, XrXirCompileContext *remaining) {
    const XrXirTypeNode *node = &types->nodes[index];
    uint32_t span = 0;
    if (node->kind != XR_XIR_TYPE_NOMINAL &&
        (node->nominal.declaration || node->nominal.arguments || node->nominal.argument_count ||
         node->nominal.fields || node->nominal.field_count)) return XR_XIR_BAD_STRUCTURE;
    if (node->kind == XR_XIR_TYPE_CALLABLE) {
        if (node->element != XR_XIR_UNIT || (node->flags & ~XR_XIR_CALLABLE_NO_SUSPEND) ||
            (node->result != XR_XIR_UNIT && !callable_component(types, node->result, index))) return XR_XIR_BAD_TYPE;
        if (node->parameter_count > 65536 || node->parameter_count > remaining->limits.parameters) return XR_XIR_BUDGET;
        uint64_t bytes = (uint64_t) node->parameter_count * sizeof(*node->parameters);
        if (bytes > SIZE_MAX ||!xir_compile_work(remaining, node->parameter_count))
            return XR_XIR_BUDGET;

        remaining->limits.parameters -= node->parameter_count;
        if ((node->parameter_count != 0) != (node->parameters != NULL)) return XR_XIR_BAD_STRUCTURE;
        span = xr_xir_type_span(types, node->result);
        for (uint32_t p = 0; p < node->parameter_count; ++p) {
            if (node->parameters[p].mode || !callable_component(types, node->parameters[p].type, index)) return XR_XIR_BAD_TYPE;
            uint32_t component = xr_xir_type_span(types, node->parameters[p].type);
            if (component > span) span = component;
        }
    } else if (node->kind == XR_XIR_TYPE_TUPLE) {
        if (node->element != XR_XIR_UNIT || node->result != XR_XIR_UNIT || node->flags)
            return XR_XIR_BAD_STRUCTURE;
        if (!node->parameter_count || !node->parameters) return XR_XIR_BAD_STRUCTURE;
        if (node->parameter_count > 65536 || node->parameter_count > remaining->limits.parameters ||
            !xir_compile_work(remaining, node->parameter_count)) return XR_XIR_BUDGET;
        remaining->limits.parameters -= node->parameter_count;
        for (uint32_t p = 0; p < node->parameter_count; ++p) {
            XrXirType field = node->parameters[p].type;
            if (node->parameters[p].mode ||
                (field != XR_XIR_UNIT && !callable_component(types, field, index))) return XR_XIR_BAD_TYPE;
            uint32_t component = xr_xir_type_span(types, field);
            if (component > span) span = component;
        }
    } else if (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CELL ||
               node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_ATOMIC) {
        if (node->parameters || node->parameter_count || node->result != XR_XIR_UNIT || node->flags)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *element = xr_xir_type_node(types, node->element);
        bool nominal_element = element &&
            element->kind == XR_XIR_TYPE_NOMINAL &&
            (uint32_t) node->element - XR_XIR_CONSTRUCTED_TYPE_BASE < index;
        if (!nominal_element && !type_component(types, node->element, index)) return XR_XIR_BAD_TYPE;
        if (node->kind == XR_XIR_TYPE_ATOMIC) {
            uint32_t element_id=(uint32_t)node->element;
            bool parameter=element_id>=XR_XIR_TYPE_PARAMETER_BASE && element_id<XR_XIR_TYPE_PARAMETER_LIMIT;
            if (!parameter && node->element!=XR_XIR_I64 && node->element!=XR_XIR_F64 && node->element!=XR_XIR_BOOL)
                return XR_XIR_BAD_TYPE;
        }
        span = xr_xir_type_span(types, node->element);
    } else if (node->kind == XR_XIR_TYPE_NOMINAL) {
        if (!types->nominals || (!types->nominals->declarations && !types->nominals->identities) ||
            node->nominal.declaration >= types->nominals->count) return XR_XIR_BAD_TYPE;
        if (node->element != XR_XIR_UNIT || node->parameters || node->parameter_count ||
            node->result != XR_XIR_UNIT || node->flags ||
            (node->nominal.argument_count != 0) != (node->nominal.arguments != NULL) ||
            (node->nominal.field_count != 0) != (node->nominal.fields != NULL)) return XR_XIR_BAD_STRUCTURE;
        uint32_t count = node->nominal.argument_count;
        if (xr_xir_type_is_class(types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+index))) {
            if (!xir_compile_work(remaining, node->nominal.field_count)) return XR_XIR_BUDGET;

            for (uint32_t f=0; f<node->nominal.field_count; ++f) {
                XrXirType field=node->nominal.fields[f];
                if (!types->nominals->declarations || !xr_xir_type_span(types,field)) {
                    XrXirStatus status=xr_xir_compile_class_field_verify(remaining, types, field);
                    if (status!=XR_XIR_OK) return status;
                }
            }
        }
        uint64_t total = (uint64_t) count + node->nominal.field_count;
        uint64_t bytes = total * sizeof(XrXirType);
        if (count > 65536 || total > remaining->limits.parameters ||!xir_compile_work(remaining, total) ||
            bytes > SIZE_MAX) return XR_XIR_BUDGET;
        remaining->limits.parameters -= (uint32_t) total;
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
static XrXirStatus type_unique(const XrXirTypes *types, uint32_t index, const XrXirCompileContext *work) {
    const XrXirTypeNode *node = &types->nodes[index];
    for (uint32_t j = 0; j < index; ++j) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;

        const XrXirTypeNode *previous = &types->nodes[j];
        if (previous->kind != node->kind || previous->element != node->element ||
            previous->parameter_count != node->parameter_count || previous->result != node->result ||
            previous->flags != node->flags || previous->nominal.declaration != node->nominal.declaration ||
            previous->nominal.argument_count != node->nominal.argument_count) continue;
        if (!xir_compile_work(work, node->parameter_count)) return XR_XIR_BUDGET;

        bool same = true;
        for (uint32_t p = 0; p < node->parameter_count; ++p)
            if (node->parameters[p].type != previous->parameters[p].type ||
                node->parameters[p].mode != previous->parameters[p].mode) same = false;
        if (!xir_compile_work(work, node->nominal.argument_count)) return XR_XIR_BUDGET;

        for (uint32_t a = 0; a < node->nominal.argument_count; ++a)
            if (node->nominal.arguments[a] != previous->nominal.arguments[a]) same = false;
        if (same) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_type_descriptors_verify(const XrXirCompileContext *compile_context, const XrXirTypes *types) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!types) return XR_XIR_OK;
    if (!remaining || (types->count != 0) != (types->nodes != NULL) ||
        (!types->count && !types->nominals && !types->interfaces)) return XR_XIR_BAD_STRUCTURE;
    if (types->count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_BUDGET;
    uint64_t bytes = sizeof(*types) + (uint64_t) types->count * sizeof(*types->nodes);
    if (bytes > SIZE_MAX ||!xir_compile_work(remaining, types->count)) return XR_XIR_BUDGET;

    for (uint32_t i = 0; i < types->count; ++i) {
        XrXirStatus status = type_payload(types, i, remaining);
        if (status == XR_XIR_OK) status = type_unique(types, i, remaining);
        if (status != XR_XIR_OK) return status;
    }
    XrXirStatus status = nominal_table_verify(types->nominals, types, remaining);
    if (status == XR_XIR_OK) status = nominal_nodes_verify(types, remaining);
    if (status == XR_XIR_OK) status = nominal_layout_verify(types, remaining);
    return status;
}
static bool type_parameter_count_add(const XrXirCompileContext *context, uint32_t *total, uint64_t amount) {
    if (!xir_compile_work(context, 1) || amount > context->limits.parameters - *total) return false;
    *total += (uint32_t)amount;
    return true;
}
XrXirStatus xr_xir_compile_types_parameter_count(const XrXirCompileContext *context,
    const XrXirTypes *types, uint32_t *output) {
    if (!xir_compile_context_valid(context) || !output) return XR_XIR_BAD_STRUCTURE;
    uint32_t total = 0;
    if (types) {
        for (uint32_t i = 0; i < types->count; ++i) {
            const XrXirTypeNode *node = &types->nodes[i];
            uint64_t slots = (uint64_t)node->parameter_count + node->nominal.argument_count + node->nominal.field_count;
            if (!type_parameter_count_add(context, &total, slots)) return XR_XIR_BUDGET;
        }
        if (types->nominals) {
            const XrXirNominalTable *table = types->nominals;
            for (uint32_t i = 0; i < table->count; ++i) {
                uint32_t slots = table->declarations ? table->declarations[i].parameter_count : table->identities[i].arity;
                if (!type_parameter_count_add(context, &total, slots)) return XR_XIR_BUDGET;
            }
        }
        if (types->interfaces) for (uint32_t i = 0; i < types->interfaces->count; ++i) {
            const XrXirInterfaceDeclaration *declaration = &types->interfaces->declarations[i];
            if (!type_parameter_count_add(context, &total, declaration->parameter_count)) return XR_XIR_BUDGET;
            for (uint32_t m = 0; m < declaration->method_count; ++m)
                if (!type_parameter_count_add(context, &total, declaration->methods[m].own_parameter_count)) return XR_XIR_BUDGET;
        }
    }
    *output = total;
    return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_types_structure_verify(const XrXirCompileContext *compile_context, const XrXirTypes *types) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    XrXirStatus status = xr_xir_compile_type_descriptors_verify(remaining, types);
    if (status == XR_XIR_OK && types) status = xr_xir_compile_interfaces_verify_structure(remaining, types->interfaces, types);
    uint32_t parameters;
    if (status == XR_XIR_OK) status = xr_xir_compile_types_parameter_count(compile_context, types, &parameters);
    return status;
}
void xr_xir_compile_types_free(XrXirTypes *types) {
    if (!types) return;
    if (types->nodes)
        for (uint32_t i = 0; i < types->count; ++i) {
            xr_compile_resources_free((void *) types->nodes[i].parameters);
            xr_compile_resources_free((void *) types->nodes[i].nominal.arguments);
            xr_compile_resources_free((void *) types->nodes[i].nominal.fields);
        }
    xr_xir_compile_nominal_free((XrXirNominalTable *) types->nominals);
    xr_xir_compile_interfaces_free((XrXirInterfaceTable *) types->interfaces);
    xr_compile_resources_free((void *) types->nodes); xr_compile_resources_free(types);
}
XrXirStatus xr_xir_compile_types_clone(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirTypes **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    if (!types) { *output = NULL; return XR_XIR_OK; }
    if ((types->count != 0) != (types->nodes != NULL) || (!types->count && !types->nominals && !types->interfaces) ||
        types->count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(compile_context, types->count)) return XR_XIR_BUDGET;
    XrXirTypes *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    XrXirTypeNode *nodes = types->count ? xir_compile_calloc(compile_context, types->count, sizeof(*nodes), &allocation_status) : NULL;
    if (types->count && !nodes) { xr_compile_resources_free(copy); return allocation_status; }
    copy->nodes = nodes; copy->count = types->count;
    for (uint32_t i = 0; i < types->count; ++i) {
        nodes[i] = types->nodes[i];
        nodes[i].nominal.arguments = NULL; nodes[i].nominal.fields = NULL;
        uint32_t count = nodes[i].parameter_count;
        XrXirCallableParameter *parameters = xir_compile_copy(compile_context, types->nodes[i].parameters, (size_t) count * sizeof(*parameters), &allocation_status);
        nodes[i].parameters = parameters;
        if (count && !parameters) { xr_xir_compile_types_free(copy); return allocation_status; }

        uint32_t arguments = nodes[i].nominal.argument_count;
        XrXirType *owned = xir_compile_copy(compile_context, types->nodes[i].nominal.arguments, (size_t) arguments * sizeof(*owned), &allocation_status);
        nodes[i].nominal.arguments = owned;
        if (arguments && !owned) { xr_xir_compile_types_free(copy); return allocation_status; }

        uint32_t fields = nodes[i].nominal.field_count;
        XrXirType *owned_fields = xir_compile_copy(compile_context, types->nodes[i].nominal.fields, (size_t) fields * sizeof(*owned_fields), &allocation_status);
        nodes[i].nominal.fields = owned_fields;
        if (fields && !owned_fields) { xr_xir_compile_types_free(copy); return allocation_status; }

    }
    XrXirNominalTable *nominals = NULL;
    XrXirStatus status = nominal_copy_table(compile_context, types->nominals, &nominals);
    if (status != XR_XIR_OK) { xr_xir_compile_types_free(copy); return status; }
    copy->nominals = nominals;
    XrXirInterfaceTable *interfaces = NULL;
    status = xr_xir_compile_interfaces_copy_verified(compile_context, types->interfaces, &interfaces);
    if (status != XR_XIR_OK) { xr_xir_compile_types_free(copy); return status; }
    copy->interfaces = interfaces;
    *output = copy; return XR_XIR_OK;
}
XrXirType xr_xir_operand_type(const XrXirFunction *function, uint32_t value) {
    if (value < function->parameter_count) return function->parameters[value];
    value -= function->parameter_count;
    return value < function->instruction_count ? function->instructions[value].type : XR_XIR_UNIT;
}

#include "xxir_type_match.inc.c"
#include "xxir_type_context.inc.c"
#include "xxir_nominal_native.inc.c"
#include "xxir_nominal.inc.c"
#include "xxir_nominal_identity.inc.c"
#include "xxir_nominal_layout.inc.c"
