/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_interface.c - Bounded interface structure and independent ownership
 *
 * KEY CONCEPT:
 *   Declaration indices and copied type arguments survive producer destruction.
 */
#include "xxir_interface.h"
#include "xxir_compile_memory.h"
#include "xxir_interface_members.h"
#include "xxir_types.h"
#include "xxir_constraints.h"
#include "xxir_constraint_proof.h"
#include "../base/xmalloc.h"
#include <string.h>

static bool interface_charge(XrXirCompileContext *b, uint64_t bytes, uint64_t work) {
    if (bytes > SIZE_MAX ||!xir_compile_work(b, work)) return false;
      return true;
}
static XrXirStatus interface_name(XrXirLiteral name, XrXirCompileContext *b) {
    if (!name.bytes || !name.length) return XR_XIR_BAD_STRUCTURE;
    if (!interface_charge(b, (uint64_t)name.length + 1, name.length)) return XR_XIR_BUDGET;
    return memchr(name.bytes, 0, name.length) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus interface_same_name(XrXirLiteral a, XrXirLiteral b,
    XrXirCompileContext *budget, bool *same) {
    *same = false;
    if (!interface_charge(budget, 0, a.length == b.length ? (uint64_t)a.length + 1 : 1)) return XR_XIR_BUDGET;
    *same = a.length == b.length && !memcmp(a.bytes, b.bytes, a.length);
    return XR_XIR_OK;
}
static XrXirStatus interface_method_shape(const XrXirInterfaceMethod *method,
    uint32_t parent_count, const XrXirTypes *types, XrXirCompileContext *b) {
    if (!!method->constraints != !!method->own_parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (parent_count > 65536 || method->own_parameter_count > 65536 - parent_count ||
        method->own_parameter_count > b->limits.parameters) return XR_XIR_BUDGET;
    uint32_t count = parent_count + method->own_parameter_count;
    uint64_t bytes = (uint64_t)method->own_parameter_count * sizeof(*method->constraints);
    if (!interface_charge(b,bytes,method->own_parameter_count)) return XR_XIR_BUDGET;
    b->limits.parameters -= method->own_parameter_count;
    XrXirStatus status = interface_name(method->name,b);
    if (status != XR_XIR_OK) return status;
    if (method->receiver || !xr_xir_callable_signature(types,method->signature)) return XR_XIR_BAD_TYPE;
    status = xr_xir_compile_type_expression_shape(b, types, method->signature, count);
    for (uint32_t p = 0; status == XR_XIR_OK && p < method->own_parameter_count; ++p)
        status = xr_xir_compile_constraint_structure(b, types, method->constraints[p], count);
    return status;
}
static XrXirStatus interface_declaration(const XrXirInterfaceDeclaration *d,
    const XrXirTypes *types, XrXirCompileContext *b) {
    if (d->exported > 1 || (!!d->constraints != !!d->parameter_count) ||
        (!!d->parents != !!d->parent_count) || (!!d->methods != !!d->method_count)) return XR_XIR_BAD_STRUCTURE;
    if (d->parameter_count > 65536 || d->parameter_count > b->limits.parameters) return XR_XIR_BUDGET;
    uint64_t bytes = (uint64_t)d->parameter_count * sizeof(*d->constraints) +
        (uint64_t)d->parent_count * sizeof(*d->parents) + (uint64_t)d->method_count * sizeof(*d->methods);
    if (!interface_charge(b, bytes, (uint64_t)d->parameter_count + d->parent_count + d->method_count)) return XR_XIR_BUDGET;
    b->limits.parameters -= d->parameter_count;
    XrXirStatus status = interface_name(d->module, b);
    if (status == XR_XIR_OK) status = interface_name(d->name, b);
    if (status != XR_XIR_OK) return status;
    for (uint32_t p = 0; p < d->parameter_count; ++p) {
        status = xr_xir_compile_constraint_structure(b, types, d->constraints[p], d->parameter_count);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t m = 0; m < d->method_count; ++m) {
        const XrXirInterfaceMethod *method = &d->methods[m];
        status = interface_method_shape(method,d->parameter_count,types,b);
        if (status != XR_XIR_OK) return status;
        for (uint32_t earlier = 0; earlier < m; ++earlier) {
            bool same;
            status = interface_same_name(method->name, d->methods[earlier].name, b, &same);
            if (status != XR_XIR_OK) return status;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus interface_parents(const XrXirInterfaceTable *table,
    const XrXirInterfaceDeclaration *d, const XrXirTypes *types, XrXirCompileContext *b) {
    for (uint32_t p = 0; p < d->parent_count; ++p) {
        const XrXirInterfaceApplication *app = &d->parents[p];
        if (app->declaration >= table->count || (!!app->arguments != !!app->argument_count)) return XR_XIR_BAD_STRUCTURE;
        const XrXirInterfaceDeclaration *parent = &table->declarations[app->declaration];
        if (app->argument_count != parent->parameter_count) return XR_XIR_BAD_TYPE;
        if (!interface_charge(b, (uint64_t)app->argument_count * sizeof(*app->arguments), (uint64_t)app->argument_count + 1))
            return XR_XIR_BUDGET;
        if (!parent->exported) {
            bool same; XrXirStatus status = interface_same_name(d->module, parent->module, b, &same);
            if (status != XR_XIR_OK) return status;
            if (!same) return XR_XIR_BAD_TYPE;
        }
        for (uint32_t a = 0; a < app->argument_count; ++a) {
            XrXirType type = app->arguments[a];
            if (type == XR_XIR_UNIT || xr_xir_type_is_cell(types, type)) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xr_xir_compile_type_expression_shape(b, types, type, d->parameter_count);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
typedef struct InterfaceWalk { uint32_t declaration, next; } InterfaceWalk;
static XrXirStatus interface_acyclic(const XrXirInterfaceTable *table, XrXirCompileContext *b) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint64_t bytes = (uint64_t)table->count * (sizeof(InterfaceWalk) + 1);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;

    unsigned char *color = xir_compile_calloc(b, table->count, 1, &allocation_status);
    InterfaceWalk *stack = xir_compile_calloc(b, table->count, sizeof(*stack), &allocation_status);
    if (!color || !stack) { xr_compile_resources_free(color); xr_compile_resources_free(stack);  return allocation_status; }
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t root = 0; root < table->count && status == XR_XIR_OK; ++root) {
        if (!interface_charge(b, 0, 1)) { status = XR_XIR_BUDGET; break; }
        if (color[root]) continue;
        uint32_t depth = 1; stack[0] = (InterfaceWalk){root, 0}; color[root] = 1;
        while (depth) {
            if (!interface_charge(b, 0, 1)) { status = XR_XIR_BUDGET; break; }
            InterfaceWalk *top = &stack[depth - 1];
            const XrXirInterfaceDeclaration *d = &table->declarations[top->declaration];
            if (top->next == d->parent_count) { color[top->declaration] = 2; --depth; continue; }
            uint32_t next = d->parents[top->next++].declaration;
            if (color[next] == 1) { status = XR_XIR_BAD_STRUCTURE; break; }
            if (color[next] == 2) continue;
            color[next] = 1; stack[depth++] = (InterfaceWalk){next, 0};
        }
    }
    xr_compile_resources_free(stack); xr_compile_resources_free(color);  return status;
}
static XrXirStatus interface_structure(const XrXirInterfaceTable *table,
    const XrXirTypes *types, XrXirCompileContext *b) {
    if (!table) return XR_XIR_OK;
    if (!table->count || !table->declarations) return XR_XIR_BAD_STRUCTURE;
    if (!interface_charge(b, sizeof(*table) + (uint64_t)table->count * sizeof(*table->declarations), table->count))
        return XR_XIR_BUDGET;
    XrXirTypes descriptors = types ? *types : (XrXirTypes){0};
    descriptors.interfaces = table;
    XrXirStatus status = xr_xir_compile_type_descriptors_verify(b, &descriptors);
    if (status != XR_XIR_OK) return status;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirInterfaceDeclaration *d = &table->declarations[i];
        status = interface_declaration(d, &descriptors, b);
        if (status != XR_XIR_OK) return status;
        for (uint32_t j = 0; j < i; ++j) {
            bool module = false, name = false;
            status = interface_same_name(d->module, table->declarations[j].module, b, &module);
            if (status == XR_XIR_OK) status = interface_same_name(d->name, table->declarations[j].name, b, &name);
            if (status != XR_XIR_OK) return status;
            if (module && name) return XR_XIR_BAD_STRUCTURE;
        }
    }
    for (uint32_t i = 0; i < table->count; ++i) {
        status = interface_parents(table, &table->declarations[i], &descriptors, b);
        if (status != XR_XIR_OK) return status;
    }
    status = interface_acyclic(table, b);
    if (status == XR_XIR_OK) status = xr_xir_compile_interfaces_verify_members_verified(b, table, &descriptors);
    return status;
}
XrXirStatus xr_xir_compile_interfaces_verify_structure(const XrXirCompileContext *compile_context, const XrXirInterfaceTable *table, const XrXirTypes *types) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = interface_structure(table, types, &remaining);
    if (status == XR_XIR_OK) *budget = remaining;
    return status;
}
void xr_xir_compile_interfaces_free(XrXirInterfaceTable *table) {
    if (!table) return;
    for (uint32_t i = 0; table->declarations && i < table->count; ++i) {
        const XrXirInterfaceDeclaration *d = &table->declarations[i];
        xr_compile_resources_free((void *)d->module.bytes); xr_compile_resources_free((void *)d->name.bytes);
        xr_xir_compile_constraint_array_free((XrXirConstraint *)d->constraints, d->parameter_count);
        xr_xir_compile_interface_applications_free((XrXirInterfaceApplication *)d->parents, d->parent_count);
        for (uint32_t m = 0; d->methods && m < d->method_count; ++m) {
            xr_compile_resources_free((void *)d->methods[m].name.bytes);
            xr_xir_compile_constraint_array_free((XrXirConstraint *)d->methods[m].constraints, d->methods[m].own_parameter_count);
        }
        xr_compile_resources_free((void *)d->methods);
    }
    xr_compile_resources_free((void *)table->declarations); xr_compile_resources_free(table);
}
static bool interface_copy_name(const XrXirCompileContext *compile_context, XrXirLiteral source, XrXirLiteral *target, XrXirStatus *allocation_status) {
    if ((uint64_t)source.length + 1 > SIZE_MAX ||
        !xir_compile_work(compile_context, (uint64_t)source.length + 1)) {
        *allocation_status = XR_XIR_BUDGET; return false;
    }
    char *bytes = xir_compile_alloc(compile_context, (size_t)source.length + 1, allocation_status);
    if (!bytes) return false;
    memcpy(bytes, source.bytes, source.length); bytes[source.length] = 0;
    *target = (XrXirLiteral){bytes, source.length}; return true;
}
static bool interface_copy_declaration(const XrXirCompileContext *compile_context, const XrXirInterfaceDeclaration *source, XrXirInterfaceDeclaration *d, XrXirStatus *allocation_status) {
    d->exported = source->exported;
    if (!interface_copy_name(compile_context, source->module, &d->module, allocation_status) || !interface_copy_name(compile_context, source->name, &d->name, allocation_status)) return false;
    XrXirConstraint *constraints = NULL;
    if ((*allocation_status = xr_xir_compile_constraint_array_copy_verified(compile_context, source->constraints, source->parameter_count, &constraints)) != XR_XIR_OK)
        return false;
    d->constraints = constraints;
    d->parameter_count = source->parameter_count;
    XrXirInterfaceApplication *parents = NULL;
    if ((*allocation_status = xr_xir_compile_interface_applications_copy_verified(compile_context, source->parents, source->parent_count, &parents)) != XR_XIR_OK)
        return false;
    d->parents = parents; d->parent_count = source->parent_count;
    XrXirInterfaceMethod *methods = source->method_count ? xir_compile_calloc(compile_context, source->method_count, sizeof(*methods), allocation_status) : NULL;
    if (source->method_count && !methods) return false;
    d->methods = methods; d->method_count = source->method_count;
    for (uint32_t m = 0; m < d->method_count; ++m) {
        methods[m].signature = source->methods[m].signature; methods[m].receiver = source->methods[m].receiver;
        if (!interface_copy_name(compile_context, source->methods[m].name, &methods[m].name, allocation_status)) return false;
        XrXirConstraint *own = NULL;
        if ((*allocation_status = xr_xir_compile_constraint_array_copy_verified(compile_context, source->methods[m].constraints, source->methods[m].own_parameter_count, &own)) != XR_XIR_OK) return false;
        methods[m].constraints = own;
        methods[m].own_parameter_count = source->methods[m].own_parameter_count;
    }
    return true;
}
XrXirStatus xr_xir_compile_interfaces_copy_verified(const XrXirCompileContext *compile_context, const XrXirInterfaceTable *table, XrXirInterfaceTable **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus allocation_status = XR_XIR_OK;

    if (!output) return XR_XIR_BAD_STRUCTURE;
    if (!table) { *output = NULL; return XR_XIR_OK; }
    if (!xir_compile_work(compile_context, table->count)) return XR_XIR_BUDGET;
    XrXirInterfaceTable *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    XrXirInterfaceDeclaration *declarations = xir_compile_calloc(compile_context, table->count, sizeof(*declarations), &allocation_status);
    if (!declarations) { xr_compile_resources_free(copy); return allocation_status; }
    copy->declarations = declarations; copy->count = table->count;
    for (uint32_t i = 0; i < table->count; ++i) {
        if (!interface_copy_declaration(compile_context, &table->declarations[i], &declarations[i], &allocation_status)) {
            xr_xir_compile_interfaces_free(copy); return allocation_status;
        }
    }
    *output = copy; return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_interfaces_clone(const XrXirCompileContext *compile_context, const XrXirInterfaceTable *table, const XrXirTypes *types, XrXirInterfaceTable **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;

    if (!output || !budget) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = interface_structure(table, types, &remaining);
    if (status == XR_XIR_OK) status = xr_xir_compile_interfaces_copy_verified(compile_context, table, output);
    if (status == XR_XIR_OK) *budget = remaining;
    return status;
}
