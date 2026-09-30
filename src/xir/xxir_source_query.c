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
#include "../base/xmalloc.h"
#include <string.h>
typedef struct SourceQueryMemory { struct SourceQueryMemory *next; } SourceQueryMemory;
struct XrXirSourceSnapshot {
    XrXirSourceView view;
    SourceQueryMemory *memory;
};
typedef struct SourceQueryCopy {
    XrXirSourceSnapshot *snapshot;
    XrXirBudget *remaining;
    XrXirStatus status;
} SourceQueryCopy;
static void *query_copy(SourceQueryCopy *copy, const void *source, size_t count, size_t size) {
    if (copy->status != XR_XIR_OK || !count) return NULL;
    if (!size || count > (SIZE_MAX - sizeof(SourceQueryMemory)) / size ||
        count > copy->remaining->work ||
        sizeof(SourceQueryMemory) + count * size > copy->remaining->metadata_bytes) {
        copy->status = XR_XIR_BUDGET; return NULL;
    }
    size_t bytes = sizeof(SourceQueryMemory) + count * size;
    SourceQueryMemory *memory = xr_calloc(1, bytes);
    if (!memory) { copy->status = XR_XIR_OUT_OF_MEMORY; return NULL; }
    copy->remaining->metadata_bytes -= bytes; copy->remaining->work -= count;
    memory->next = copy->snapshot->memory; copy->snapshot->memory = memory;
    if (source) memcpy(memory + 1, source, count * size);
    return memory + 1;
}
static const char *query_string(SourceQueryCopy *copy, const char *source) {
    return source ? query_copy(copy, source, strlen(source) + 1, 1) : NULL;
}
static XrXirInterfaceApplication *query_applications(SourceQueryCopy *copy,
    const XrXirInterfaceApplication *source, uint32_t count) {
    if (copy->status != XR_XIR_OK) return NULL;
    if (!!source != !!count) { copy->status = XR_XIR_BAD_STRUCTURE; return NULL; }
    XrXirInterfaceApplication *applications = query_copy(copy, source, count, sizeof(*applications));
    for (uint32_t i = 0; applications && i < count && copy->status == XR_XIR_OK; ++i) {
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
    for (uint32_t i = 0; constraints && i < count && copy->status == XR_XIR_OK; ++i)
        constraints[i].interfaces = query_applications(copy, source[i].interfaces, source[i].interface_count);
    return constraints;
}
static void query_application(SourceQueryCopy *copy, XrXirInterfaceApplication *app) {
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
    for (uint32_t i = 0; records && i < source->count && copy->status == XR_XIR_OK; ++i) {
        query_application(copy, &records[i].interface);
        if (!!records[i].bindings != !!records[i].binding_count) { copy->status = XR_XIR_BAD_STRUCTURE; break; }
        XrXirImplementationBinding *bindings = query_copy(copy, records[i].bindings,
            records[i].binding_count, sizeof(*bindings));
        records[i].bindings = bindings;
        for (uint32_t b = 0; bindings && b < records[i].binding_count && copy->status == XR_XIR_OK; ++b)
            query_application(copy, &bindings[b].requirement);
    }
}
static void query_modules(SourceQueryCopy *copy, const XrXirSourceView *source) {
    XrXirSourceQueryModule *modules = query_copy(copy, source->modules, source->module_count, sizeof(*modules));
    copy->snapshot->view.modules = modules;
    if (!modules) return;
    for (uint32_t i = 0; i < source->module_count && copy->status == XR_XIR_OK; ++i) {
        modules[i].identity = query_string(copy, source->modules[i].identity);
        modules[i].path = query_string(copy, source->modules[i].path);
    }
}
typedef struct SourceQueryDeclarationLayout {
    size_t bytes;
    uint64_t work;
} SourceQueryDeclarationLayout;
_Static_assert(_Alignof(SourceQueryMemory) >= _Alignof(XrXirSourceDeclaration) &&
    sizeof(SourceQueryMemory) % _Alignof(XrXirSourceDeclaration) == 0 &&
    _Alignof(SourceQueryMemory) >= _Alignof(XrXirSourceType), "query table alignment");
static bool query_declaration_extent(const XrXirSourceDeclaration *source,
    SourceQueryDeclarationLayout *layout) {
    size_t alignment = _Alignof(XrXirSourceType);
    size_t padding = source->parameter_count ? (alignment - layout->bytes % alignment) % alignment : 0;
    if (padding > SIZE_MAX - layout->bytes) return false;
    size_t bytes = layout->bytes + padding;
    if (source->parameter_count > (SIZE_MAX - bytes) / sizeof(XrXirSourceType)) return false;
    bytes += (size_t)source->parameter_count * sizeof(XrXirSourceType);
    size_t names = source->name ? strlen(source->name) + 1 : 0;
    size_t signatures = source->signature ? strlen(source->signature) + 1 : 0;
    if (names > SIZE_MAX - bytes || signatures > SIZE_MAX - bytes - names) return false;
    if (names > UINT64_MAX - layout->work || signatures > UINT64_MAX - layout->work - names ||
        source->parameter_count > UINT64_MAX - layout->work - names - signatures) return false;
    layout->bytes = bytes + names + signatures;
    layout->work += names + signatures + source->parameter_count;
    return layout->bytes <= SIZE_MAX - sizeof(SourceQueryMemory);
}
static void query_declaration_place(unsigned char *storage, size_t *offset,
    const XrXirSourceDeclaration *source, XrXirSourceDeclaration *output) {
    size_t alignment = _Alignof(XrXirSourceType);
    if (source->parameter_count) *offset += (alignment - *offset % alignment) % alignment;
    size_t bytes = (size_t)source->parameter_count * sizeof(XrXirSourceType);
    output->parameters = source->parameter_count ? (const XrXirSourceType *)(storage + *offset) : NULL;
    if (bytes && source->parameters) memcpy(storage + *offset, source->parameters, bytes);
    *offset += bytes;
    size_t names = source->name ? strlen(source->name) + 1 : 0;
    output->name = names ? (const char *)(storage + *offset) : NULL;
    if (names) memcpy(storage + *offset, source->name, names);
    *offset += names;
    size_t signatures = source->signature ? strlen(source->signature) + 1 : 0;
    output->signature = signatures ? (const char *)(storage + *offset) : NULL;
    if (signatures) memcpy(storage + *offset, source->signature, signatures);
    *offset += signatures;
}
static void query_declarations(SourceQueryCopy *copy, const XrXirSourceView *source) {
    copy->snapshot->view.declarations = NULL;
    if (copy->status != XR_XIR_OK || !source->declaration_count) return;
    size_t count = source->declaration_count;
    if (count > (SIZE_MAX - sizeof(SourceQueryMemory)) / sizeof(XrXirSourceDeclaration) ||
        count > copy->remaining->work) { copy->status = XR_XIR_BUDGET; return; }
    size_t table_bytes = count * sizeof(XrXirSourceDeclaration);
    if (sizeof(SourceQueryMemory) + table_bytes > copy->remaining->metadata_bytes ||
        count == copy->remaining->work) { copy->status = XR_XIR_BUDGET; return; }
    SourceQueryDeclarationLayout layout = {table_bytes, count};
    for (size_t i = 0; i < count; ++i) {
        if (layout.work >= copy->remaining->work) { copy->status = XR_XIR_BUDGET; return; }
        --copy->remaining->work; /* Charge the row actually inspected, even on failure. */
        if (!query_declaration_extent(&source->declarations[i], &layout) ||
            layout.work > copy->remaining->work ||
            sizeof(SourceQueryMemory) + layout.bytes > copy->remaining->metadata_bytes) {
            copy->status = XR_XIR_BUDGET; return;
        }
    }
    SourceQueryMemory *memory = xr_calloc(1, sizeof(*memory) + layout.bytes);
    if (!memory) { copy->status = XR_XIR_OUT_OF_MEMORY; return; }
    copy->remaining->metadata_bytes -= sizeof(*memory) + layout.bytes;
    copy->remaining->work -= layout.work;
    memory->next = copy->snapshot->memory; copy->snapshot->memory = memory;
    unsigned char *storage = (unsigned char *)(memory + 1);
    XrXirSourceDeclaration *declarations = (XrXirSourceDeclaration *)storage;
    memcpy(declarations, source->declarations, table_bytes);
    copy->snapshot->view.declarations = declarations;
    size_t offset = table_bytes;
    for (size_t i = 0; i < count && copy->status == XR_XIR_OK; ++i) {
        query_declaration_place(storage, &offset, &source->declarations[i], &declarations[i]);
        declarations[i].generic_constraints = query_constraints(copy,
            source->declarations[i].generic_constraints, declarations[i].generic_parameter_count);
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
    for (uint32_t i = 0; i < source->count && copy->status == XR_XIR_OK; ++i) {
        query_literal(copy, &decls[i].module); query_literal(copy, &decls[i].name);
        decls[i].constraints = query_constraints(copy, decls[i].constraints, decls[i].parameter_count);
        XrXirNominalField *fields = query_copy(copy, decls[i].fields, decls[i].field_count, sizeof(*fields));
        decls[i].fields = fields;
        for (uint32_t j = 0; fields && j < decls[i].field_count && copy->status == XR_XIR_OK; ++j)
            query_literal(copy, &fields[j].name);
        XrXirNominalVariant *variants = query_copy(copy, decls[i].variants, decls[i].variant_count, sizeof(*variants));
        decls[i].variants = variants;
        for (uint32_t j = 0; variants && j < decls[i].variant_count && copy->status == XR_XIR_OK; ++j)
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
    for (uint32_t i = 0; decls && i < source->count && copy->status == XR_XIR_OK; ++i) {
        query_literal(copy, &decls[i].module); query_literal(copy, &decls[i].name);
        decls[i].constraints = query_constraints(copy, decls[i].constraints, decls[i].parameter_count);
        decls[i].parents = query_applications(copy, decls[i].parents, decls[i].parent_count);
        XrXirInterfaceMethod *methods = query_copy(copy, decls[i].methods, decls[i].method_count, sizeof(*methods));
        decls[i].methods = methods;
        for (uint32_t m = 0; methods && m < decls[i].method_count && copy->status == XR_XIR_OK; ++m) {
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
    for (uint32_t i = 0; i < source->count && copy->status == XR_XIR_OK; ++i) {
        nodes[i].parameters = query_copy(copy, source->nodes[i].parameters,
            nodes[i].parameter_count, sizeof(*nodes[i].parameters));
        nodes[i].nominal.arguments = query_copy(copy, source->nodes[i].nominal.arguments,
            nodes[i].nominal.argument_count, sizeof(*nodes[i].nominal.arguments));
        nodes[i].nominal.fields = query_copy(copy, source->nodes[i].nominal.fields,
            nodes[i].nominal.field_count, sizeof(*nodes[i].nominal.fields));
    }
}
XrXirStatus xr_xir_source_snapshot_copy(const XrXirSourceView *view,
    XrXirBudget *remaining, XrXirSourceSnapshot **output) {
    if (output) *output = NULL;
    if (!view || !remaining || !output) return XR_XIR_BAD_STRUCTURE;
    if (view->types && view->types->nominals && view->types->nominals->identities) return XR_XIR_BAD_STAGE;
    if (remaining->metadata_bytes < sizeof(XrXirSourceSnapshot) || !remaining->work) return XR_XIR_BUDGET;
    XrXirSourceSnapshot *snapshot = xr_calloc(1, sizeof(*snapshot));
    if (!snapshot) return XR_XIR_OUT_OF_MEMORY;
    remaining->metadata_bytes -= sizeof(*snapshot); --remaining->work;
    snapshot->view = *view;
    SourceQueryCopy copy = {snapshot, remaining, XR_XIR_OK};
    query_modules(&copy, view); query_declarations(&copy, view); query_types(&copy, view->types);
    query_implementations(&copy, view->implementations);
    snapshot->view.references = query_copy(&copy, view->references, view->reference_count, sizeof(*view->references));
    snapshot->view.expressions = query_copy(&copy, view->expressions, view->expression_count, sizeof(*view->expressions));
    if (copy.status != XR_XIR_OK) { xr_xir_source_snapshot_free(snapshot); return copy.status; }
    *output = snapshot; return XR_XIR_OK;
}
const XrXirSourceView *xr_xir_source_snapshot_view(const XrXirSourceSnapshot *snapshot) {
    return snapshot ? &snapshot->view : NULL;
}
void xr_xir_source_snapshot_free(XrXirSourceSnapshot *snapshot) {
    if (!snapshot) return;
    while (snapshot->memory) {
        SourceQueryMemory *next = snapshot->memory->next;
        xr_free(snapshot->memory); snapshot->memory = next;
    }
    xr_free(snapshot);
}
void xr_xir_source_result_free(XrXirSourceResult *result) {
    if (!result) return;
    xr_xir_artifact_free(result->checked); xr_xir_source_snapshot_free(result->snapshot);
    memset(result, 0, sizeof(*result));
}
