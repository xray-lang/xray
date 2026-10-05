/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_query.c - Independent ownership for immutable semantic facts
 *
 * KEY CONCEPT:
 *   The snapshot owns facts and strings, never parser or construction state.
 */
#include "xxir_source_query_internal.h"
#include "xxir_nominal.h"
#include "xxir_interface.h"
#include "xxir_implementation.h"
#include "xxir_compile_memory.h"
#include <string.h>
typedef struct SourceQueryMemory { struct SourceQueryMemory *next; } SourceQueryMemory;
struct XrXirSourceSnapshot {
    XrXirCompileContext context;
    XrXirSourceView view;
    SourceQueryMemory *memory;
};
typedef struct SourceQueryCopy {
    XrXirSourceSnapshot *snapshot;
    XrXirStatus status;
} SourceQueryCopy;
static bool query_work(SourceQueryCopy *copy, uint64_t work) {
    if (copy->status != XR_XIR_OK) return false;
    if (!xir_compile_work(&copy->snapshot->context, work)) copy->status = XR_XIR_BUDGET;
    return copy->status == XR_XIR_OK;
}
static size_t query_length(SourceQueryCopy *copy, const char *text) {
    size_t length = 0;
    if (!text) return 0;
    while (query_work(copy, 1)) {
        if (!text[length]) return length + 1;
        if (length == SIZE_MAX - 1) { copy->status = XR_XIR_BUDGET; return 0; }
        ++length;
    }
    return 0;
}
static void *query_allocate(SourceQueryCopy *copy, size_t count, size_t size) {
    if (copy->status != XR_XIR_OK || !count) return NULL;
    if (!size || count > (SIZE_MAX - sizeof(SourceQueryMemory)) / size) {
        copy->status = XR_XIR_BUDGET; return NULL;
    }
    size_t bytes = sizeof(SourceQueryMemory) + count * size;
    SourceQueryMemory *memory = xir_compile_calloc(&copy->snapshot->context, 1, bytes, &copy->status);
    if (!memory) return NULL;
    memory->next = copy->snapshot->memory; copy->snapshot->memory = memory;
    return memory + 1;
}
static void *query_copy(SourceQueryCopy *copy, const void *source, size_t count, size_t size) {
    if (copy->status != XR_XIR_OK || !count) return NULL;
    if (!source) { copy->status = XR_XIR_BAD_STRUCTURE; return NULL; }
    void *memory = query_allocate(copy, count, size);
    if (!memory || !query_work(copy, count * size)) return NULL;
    memcpy(memory, source, count * size);
    return memory;
}
static const char *query_string(SourceQueryCopy *copy, const char *source) {
    size_t bytes = query_length(copy, source);
    return bytes ? query_copy(copy, source, bytes, 1) : NULL;
}
static XrXirInterfaceApplication *query_applications(SourceQueryCopy *copy,
    const XrXirInterfaceApplication *source, uint32_t count) {
    if (copy->status != XR_XIR_OK) return NULL;
    if (!!source != !!count) { copy->status = XR_XIR_BAD_STRUCTURE; return NULL; }
    XrXirInterfaceApplication *applications = query_copy(copy, source, count, sizeof(*applications));
    for (uint32_t i = 0; applications && i < count && query_work(copy, 1); ++i) {
        if (!!source[i].arguments != !!source[i].argument_count) { copy->status = XR_XIR_BAD_STRUCTURE; break; }
        applications[i].arguments = query_copy(copy, source[i].arguments,
            source[i].argument_count, sizeof(*source[i].arguments));
    }
    return applications;
}
static XrXirConstraint *query_constraints(SourceQueryCopy *copy,
    const XrXirConstraint *source, uint32_t count) {
    if (copy->status != XR_XIR_OK) return NULL;
    if (!!source != !!count) { copy->status = XR_XIR_BAD_STRUCTURE; return NULL; }
    XrXirConstraint *constraints = query_copy(copy, source, count, sizeof(*constraints));
    for (uint32_t i = 0; constraints && i < count && query_work(copy, 1); ++i)
        constraints[i].interfaces = query_applications(copy, source[i].interfaces, source[i].interface_count);
    return constraints;
}
static void query_application(SourceQueryCopy *copy, XrXirInterfaceApplication *app) {
    if (copy->status != XR_XIR_OK) return;
    if (!!app->arguments != !!app->argument_count) { copy->status = XR_XIR_BAD_STRUCTURE; return; }
    app->arguments = query_copy(copy, app->arguments, app->argument_count, sizeof(*app->arguments));
}
static void query_implementations(SourceQueryCopy *copy, const XrXirImplementationTable *source) {
    copy->snapshot->view.implementations = NULL;
    if (!source || copy->status != XR_XIR_OK) return;
    if (!source->records || !source->count) { copy->status = XR_XIR_BAD_STRUCTURE; return; }
    XrXirImplementationTable *table = query_copy(copy, source, 1, sizeof(*table));
    copy->snapshot->view.implementations = table;
    if (!table) return;
    XrXirImplementation *records = query_copy(copy, source->records, source->count, sizeof(*records));
    table->records = records;
    for (uint32_t i = 0; records && i < source->count && query_work(copy, 1); ++i) {
        query_application(copy, &records[i].interface);
        if (copy->status != XR_XIR_OK) break;
        if (!!records[i].bindings != !!records[i].binding_count) { copy->status = XR_XIR_BAD_STRUCTURE; break; }
        XrXirImplementationBinding *bindings = query_copy(copy, records[i].bindings,
            records[i].binding_count, sizeof(*bindings));
        records[i].bindings = bindings;
        for (uint32_t b = 0; bindings && b < records[i].binding_count && query_work(copy, 1); ++b)
            query_application(copy, &bindings[b].requirement);
    }
}
static void query_modules(SourceQueryCopy *copy, const XrXirSourceView *source) {
    XrXirSourceQueryModule *modules = query_copy(copy, source->modules, source->module_count, sizeof(*modules));
    copy->snapshot->view.modules = modules;
    if (!modules) return;
    for (uint32_t i = 0; i < source->module_count && query_work(copy, 1); ++i) {
        modules[i].identity = query_string(copy, source->modules[i].identity);
        modules[i].path = query_string(copy, source->modules[i].path);
    }
}
_Static_assert(_Alignof(SourceQueryMemory) >= _Alignof(XrXirSourceDeclaration) &&
    sizeof(SourceQueryMemory) % _Alignof(XrXirSourceDeclaration) == 0 &&
    _Alignof(SourceQueryMemory) >= _Alignof(XrXirSourceType), "query table alignment");
static bool query_declaration_extent(SourceQueryCopy *copy, const XrXirSourceDeclaration *source,
    size_t *extent) {
    if (source->parameter_count && !source->parameters) {
        copy->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    size_t alignment = _Alignof(XrXirSourceType);
    size_t padding = source->parameter_count ? (alignment - *extent % alignment) % alignment : 0;
    if (padding > SIZE_MAX - *extent) return false;
    size_t bytes = *extent + padding;
    if (source->parameter_count > (SIZE_MAX - bytes) / sizeof(XrXirSourceType)) return false;
    bytes += (size_t)source->parameter_count * sizeof(XrXirSourceType);
    size_t names = query_length(copy, source->name);
    size_t signatures = query_length(copy, source->signature);
    if (copy->status != XR_XIR_OK) return false;
    if (names > SIZE_MAX - bytes || signatures > SIZE_MAX - bytes - names) return false;
    *extent = bytes + names + signatures;
    return *extent <= SIZE_MAX - sizeof(SourceQueryMemory);
}
static void query_declaration_place(SourceQueryCopy *copy, unsigned char *storage, size_t *offset,
    const XrXirSourceDeclaration *source, XrXirSourceDeclaration *output) {
    size_t alignment = _Alignof(XrXirSourceType);
    if (source->parameter_count) *offset += (alignment - *offset % alignment) % alignment;
    size_t bytes = (size_t)source->parameter_count * sizeof(XrXirSourceType);
    output->parameters = source->parameter_count ? (const XrXirSourceType *)(storage + *offset) : NULL;
    if (bytes && source->parameters && query_work(copy, bytes))
        memcpy(storage + *offset, source->parameters, bytes);
    *offset += bytes;
    size_t names = query_length(copy, source->name);
    output->name = names ? (const char *)(storage + *offset) : NULL;
    if (names && query_work(copy, names)) memcpy(storage + *offset, source->name, names);
    *offset += names;
    size_t signatures = query_length(copy, source->signature);
    output->signature = signatures ? (const char *)(storage + *offset) : NULL;
    if (signatures && query_work(copy, signatures)) memcpy(storage + *offset, source->signature, signatures);
    *offset += signatures;
}
static bool query_declaration_admit(SourceQueryCopy *copy, size_t extent, size_t table_bytes,
    uint64_t row_work) {
    if (extent > SIZE_MAX - sizeof(SourceQueryMemory)) {
        copy->status = XR_XIR_BUDGET; return false;
    }
    size_t payload = sizeof(SourceQueryMemory) + extent;
    uint64_t work = payload;
    if (table_bytes > UINT64_MAX - work || row_work > UINT64_MAX - work - table_bytes) {
        copy->status = XR_XIR_BUDGET; return false;
    }
    work += table_bytes + row_work;
    XrCompileResourceStatus status = xr_compile_resources_admit(
        copy->snapshot->context.resources, payload, work);
    if (status != XR_COMPILE_RESOURCE_OK) copy->status = xir_compile_resource_status(status);
    return copy->status == XR_XIR_OK;
}
static void query_declarations(SourceQueryCopy *copy, const XrXirSourceView *source) {
    copy->snapshot->view.declarations = NULL;
    if (copy->status != XR_XIR_OK || !source->declaration_count) return;
    size_t count = source->declaration_count;
    if (count > (SIZE_MAX - sizeof(SourceQueryMemory)) / sizeof(XrXirSourceDeclaration)) {
        copy->status = XR_XIR_BUDGET; return;
    }
    size_t table_bytes = count * sizeof(XrXirSourceDeclaration), extent = table_bytes;
    if (!query_declaration_admit(copy, extent, table_bytes, 2 * (uint64_t)count)) return;
    for (size_t i = 0; i < count && query_work(copy, 1); ++i) {
        if (!query_declaration_extent(copy, &source->declarations[i], &extent)) {
            if (copy->status == XR_XIR_OK) copy->status = XR_XIR_BUDGET;
            return;
        }
        if (!query_declaration_admit(copy, extent, table_bytes, (uint64_t)count + count - i - 1)) return;
    }
    unsigned char *storage = query_allocate(copy, extent, 1);
    if (!storage || !query_work(copy, table_bytes)) return;
    XrXirSourceDeclaration *declarations = (XrXirSourceDeclaration *)storage;
    memcpy(declarations, source->declarations, table_bytes);
    copy->snapshot->view.declarations = declarations;
    size_t offset = table_bytes;
    for (size_t i = 0; i < count && query_work(copy, 1); ++i) {
        query_declaration_place(copy, storage, &offset, &source->declarations[i], &declarations[i]);
        declarations[i].generic_constraints = query_constraints(copy,
            source->declarations[i].generic_constraints, declarations[i].generic_parameter_count);
        declarations[i].type_parameter_kinds = query_copy(copy,
            source->declarations[i].type_parameter_kinds,
            source->declarations[i].type_parameter_kinds ? declarations[i].generic_parameter_count : 0,
            sizeof(uint32_t));
    }
}
static void query_literal(SourceQueryCopy *copy, XrXirLiteral *literal) {
    literal->bytes = query_copy(copy, literal->bytes, literal->length, 1);
}
static void query_nominals(SourceQueryCopy *copy, XrXirTypes *types) {
    const XrXirNominalTable *source = types->nominals;
    if (!source) return;
    XrXirNominalTable *table = query_copy(copy, source, 1, sizeof(*table));
    types->nominals = table;
    if (!table) return;
    XrXirNominalDeclaration *decls = query_copy(copy, source->declarations, source->count, sizeof(*decls));
    table->declarations = decls;
    if (!decls) return;
    for (uint32_t i = 0; i < source->count && query_work(copy, 1); ++i) {
        query_literal(copy, &decls[i].module); query_literal(copy, &decls[i].name);
        decls[i].constraints = query_constraints(copy, decls[i].constraints, decls[i].parameter_count);
        XrXirNominalField *fields = query_copy(copy, decls[i].fields, decls[i].field_count, sizeof(*fields));
        decls[i].fields = fields;
        for (uint32_t j = 0; fields && j < decls[i].field_count && query_work(copy, 1); ++j)
            query_literal(copy, &fields[j].name);
        XrXirNominalVariant *variants = query_copy(copy, decls[i].variants, decls[i].variant_count, sizeof(*variants));
        decls[i].variants = variants;
        for (uint32_t j = 0; variants && j < decls[i].variant_count && query_work(copy, 1); ++j)
            query_literal(copy, &variants[j].name);
    }
}
static void query_interfaces(SourceQueryCopy *copy, XrXirTypes *types) {
    const XrXirInterfaceTable *source = types->interfaces;
    if (!source) return;
    XrXirInterfaceTable *table = query_copy(copy, source, 1, sizeof(*table));
    types->interfaces = table;
    if (!table) return;
    XrXirInterfaceDeclaration *decls = query_copy(copy, source->declarations, source->count, sizeof(*decls));
    table->declarations = decls;
    for (uint32_t i = 0; decls && i < source->count && query_work(copy, 1); ++i) {
        query_literal(copy, &decls[i].module); query_literal(copy, &decls[i].name);
        decls[i].constraints = query_constraints(copy, decls[i].constraints, decls[i].parameter_count);
        decls[i].parents = query_applications(copy, decls[i].parents, decls[i].parent_count);
        XrXirInterfaceMethod *methods = query_copy(copy, decls[i].methods, decls[i].method_count, sizeof(*methods));
        decls[i].methods = methods;
        for (uint32_t m = 0; methods && m < decls[i].method_count && query_work(copy, 1); ++m) {
            query_literal(copy, &methods[m].name);
            methods[m].constraints = query_constraints(copy,methods[m].constraints,
                methods[m].own_parameter_count);
        }
    }
}
static void query_types(SourceQueryCopy *copy, const XrXirTypes *source) {
    if (!source) { copy->snapshot->view.types = NULL; return; }
    XrXirTypes *types = query_copy(copy, source, 1, sizeof(*types));
    if (!types) return;
    copy->snapshot->view.types = types;
    query_nominals(copy, types);
    query_interfaces(copy, types);
    XrXirTypeNode *nodes = query_copy(copy, source->nodes, source->count, sizeof(*nodes));
    types->nodes = nodes;
    if (!nodes) return;
    for (uint32_t i = 0; i < source->count && query_work(copy, 1); ++i) {
        nodes[i].parameters = query_copy(copy, source->nodes[i].parameters,
            nodes[i].parameter_count, sizeof(*nodes[i].parameters));
        nodes[i].nominal.arguments = query_copy(copy, source->nodes[i].nominal.arguments,
            nodes[i].nominal.argument_count, sizeof(*nodes[i].nominal.arguments));
        nodes[i].nominal.fields = query_copy(copy, source->nodes[i].nominal.fields,
            nodes[i].nominal.field_count, sizeof(*nodes[i].nominal.fields));
    }
}
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_copy(const XrXirCompileContext *context,
    const XrXirSourceView *view, XrXirSourceSnapshot **output) {
    if (!view || !xir_compile_context_valid(context) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
    if ((view->module_count && !view->modules) || (view->declaration_count && !view->declarations) ||
        (view->reference_count && !view->references) || (view->expression_count && !view->expressions) ||
        (view->types && view->types->count && !view->types->nodes)) return XR_XIR_BAD_STRUCTURE;
    if (view->types && view->types->nominals && view->types->nominals->identities) return XR_XIR_BAD_STAGE;
    XrXirStatus status = XR_XIR_OK;
    XrXirSourceSnapshot *snapshot = xir_compile_calloc(context, 1, sizeof(*snapshot), &status);
    if (!snapshot) return status;
    snapshot->context = *context;
    SourceQueryCopy copy = {snapshot, XR_XIR_OK};
    if (query_work(&copy, sizeof(*view))) snapshot->view = *view;
    query_modules(&copy, view); query_declarations(&copy, view); query_types(&copy, view->types);
    query_implementations(&copy, view->implementations);
    snapshot->view.references = query_copy(&copy, view->references, view->reference_count, sizeof(*view->references));
    snapshot->view.expressions = query_copy(&copy, view->expressions, view->expression_count, sizeof(*view->expressions));
    if (copy.status != XR_XIR_OK) { xr_xir_compile_source_snapshot_free(snapshot); return copy.status; }
    *output = snapshot; return XR_XIR_OK;
}
XR_FUNC const XrXirSourceView *xr_xir_compile_source_snapshot_view(const XrXirSourceSnapshot *snapshot) {
    return snapshot ? &snapshot->view : NULL;
}
XR_FUNC void xr_xir_compile_source_snapshot_free(XrXirSourceSnapshot *snapshot) {
    if (!snapshot) return;
    while (snapshot->memory) {
        SourceQueryMemory *next = snapshot->memory->next;
        xr_compile_resources_free(snapshot->memory); snapshot->memory = next;
    }
    xr_compile_resources_free(snapshot);
}
XR_FUNC void xr_xir_compile_source_result_free(XrXirSourceResult *result) {
    if (!result) return;
    xr_xir_compile_artifact_free(result->checked); xr_xir_compile_source_snapshot_free(result->snapshot);
    memset(result, 0, sizeof(*result));
}
