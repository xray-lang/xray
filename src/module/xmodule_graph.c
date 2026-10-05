/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_graph.c - Owned source discovery and transactional Tarjan ordering
 */
#include "xmodule_graph.h"
#include "xmodule_compile_internal.h"
#include "xstdlib_embedded.h"
#include "xsemver.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../toolchain/xcompiler_session.h"
#include "../os/os_file_read.h"
#define GRAPH_INITIAL_CAP 16
#define GRAPH_MAX_MODULES 1024
#define GRAPH_EMBEDDED_STDLIB_PREFIX "<embedded stdlib>/"
extern void xr_program_destroy(struct AstNode *ast);
static XrModuleStatus graph_failure(XrModuleGraph *graph, XrModuleStatus status) {
    if (graph->resolution_status == XR_MODULE_OK) graph->resolution_status = status;
    return graph->resolution_status;
}
static void spec_cleanup(XrModuleSpec *spec) {
    xr_compile_resources_free(spec->canonical); xr_compile_resources_free(spec->logical_path);
    xr_compile_resources_free(spec->source_path); xr_compile_resources_free((char *)spec->authority.namespace_id);
    xr_compile_resources_free((char *)spec->authority.physical_root); xr_compile_resources_free(spec->dep_indices);
    if (spec->ast) xr_program_destroy(spec->ast);
    xr_hashmap_owned_free(spec->export_symbols); *spec = (XrModuleSpec){0};
}
XR_FUNC void xr_compile_module_graph_free(XrModuleGraph *graph) {
    if (!graph) return;
    for (int i = 0; i < graph->spec_count; ++i) spec_cleanup(&graph->specs[i]);
    xr_compile_resources_free(graph->specs); xr_hashmap_owned_free(graph->id_index);
    xr_compile_resources_free(graph->topo_order); xr_compile_resources_free(graph->cycle_desc);
    xr_compile_resources_free(graph->unresolved_error); xr_compile_resources_free(graph);
}
XR_FUNC XrModuleStatus xr_compile_module_graph_new(XrCompileResources *resources,
    XrCompilerSession *session, XrModuleResolver *resolver, XrModuleGraph **output) {
    if (!resources || !session || !resolver || !output || resolver->resources != resources ||
        xr_compile_session_resources(session) != resources) return XR_MODULE_INVALID;
    ModuleWork work = {resources,XR_MODULE_OK};
    XrModuleGraph *graph = module_calloc(&work,1,sizeof(*graph));
    if (!graph) return work.status;
    graph->resources = resources; graph->resolver = resolver; graph->compiler_session = session; graph->entry_index = -1;
    graph->specs = module_calloc(&work,GRAPH_INITIAL_CAP,sizeof(*graph->specs)); graph->spec_capacity = GRAPH_INITIAL_CAP;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    if (work.status == XR_MODULE_OK) module_io(&work,xr_hashmap_owned_new(&policy,&graph->id_index));
    if (work.status != XR_MODULE_OK) { xr_compile_module_graph_free(graph); return work.status; }
    *output = graph; return XR_MODULE_OK;
}
XR_FUNC XrModuleStatus xr_compile_module_graph_find(const XrModuleGraph *graph, const char *canonical, int *output) {
    if (!graph || !graph->resources || !canonical || !output) return XR_MODULE_INVALID;
    void *value = NULL; XrModuleStatus status = xr_module_status_from_io(xr_hashmap_owned_get(graph->id_index,canonical,&value));
    if (status == XR_MODULE_OK) *output = value ? (int)(intptr_t)value-1 : -1;
    return status;
}
XR_FUNC XrModuleStatus xr_compile_module_graph_find_source(const XrModuleGraph *graph, const char *path, int *output) {
    if (!graph || !graph->resources || !path || !output) return XR_MODULE_INVALID;
    ModuleWork work = {graph->resources,XR_MODULE_OK}; int found = -1;
    for (int i = 0; i < graph->spec_count && module_work(&work,1); ++i)
        if (graph->specs[i].source_path && module_equal(&work,graph->specs[i].source_path,path)) { found = i; break; }
    if (work.status == XR_MODULE_OK) *output = found;
    return work.status;
}
XR_FUNC XrModuleStatus xr_compile_module_graph_owns_top_level_decl(const XrModuleGraph *graph,
    const XrModuleSpec *spec, const AstNode *decl, bool *output) {
    if (!graph || !graph->resources || !output) return XR_MODULE_INVALID;
    ModuleWork work = {graph->resources,XR_MODULE_OK}; bool owned = false; uint32_t matches = 0;
    for (int i = 0; i < graph->spec_count && module_work(&work,1); ++i)
        if (&graph->specs[i] == spec) { owned = true; break; }
    if (owned && decl && spec->ast && spec->ast->type == AST_PROGRAM) {
        const ProgramNode *program = &spec->ast->as.program;
        for (int i = 0; i < program->count && module_work(&work,1); ++i)
            if (program->statements[i] == decl) ++matches;
    }
    if (work.status == XR_MODULE_OK) *output = matches == 1;
    return work.status;
}
XR_FUNC XrModuleStatus xr_compile_module_graph_find_named_dependency(const XrModuleGraph *graph,
    const char *importer_path, const char *specifier, int *output) {
    if (!graph || !graph->resources || !importer_path || !specifier || !output) return XR_MODULE_INVALID;
    ModuleWork work = {graph->resources,XR_MODULE_OK}; int importer = -1, match = -1;
    module_status(&work,xr_compile_module_graph_find_source(graph,importer_path,&importer));
    if (work.status == XR_MODULE_OK && importer < 0) module_status(&work,xr_compile_module_graph_find(graph,importer_path,&importer));
    if (work.status != XR_MODULE_OK) return work.status;
    if (importer < 0) { *output = -1; return XR_MODULE_OK; }
    size_t length = module_length(&work,specifier);
    const XrModuleSpec *owner = &graph->specs[importer]; XrOsIoPolicy policy = xr_compile_io_policy(graph->resources);
    for (int edge = 0; edge < owner->dep_count && module_work(&work,1); ++edge) {
        int index = owner->dep_indices[edge];
        if (index < 0 || index >= graph->spec_count) { module_status(&work,XR_MODULE_INVALID); break; }
        const XrModuleSpec *candidate = &graph->specs[index]; const char *coordinate = candidate->authority.namespace_id;
        bool matches = candidate->kind == XR_MOD_STDLIB && coordinate && module_equal(&work,coordinate,specifier);
        if (!matches && candidate->kind == XR_MOD_PACKAGE && coordinate) {
            size_t n = module_length(&work,coordinate);
            if (n > length && n-length > 1 && module_prefix(&work,coordinate,specifier) &&
                module_work(&work,1) && coordinate[length] == '@')
                module_io(&work,xr_semver_is_valid_owned(&policy,coordinate+length+1,&matches));
        }
        if (!matches || work.status != XR_MODULE_OK) continue;
        char *expected = NULL;
        module_status(&work,xr_compile_module_identity_from_logical(graph->resources,&candidate->authority,candidate->logical_path,&expected));
        if (work.status == XR_MODULE_OK && (!candidate->canonical || !module_equal(&work,expected,candidate->canonical) ||
            (candidate->kind == XR_MOD_STDLIB && candidate->authority.kind != XR_MODULE_IDENTITY_STDLIB) ||
            (candidate->kind == XR_MOD_PACKAGE && candidate->authority.kind != XR_MODULE_IDENTITY_PACKAGE) || match >= 0))
            module_status(&work,XR_MODULE_INVALID);
        xr_compile_resources_free(expected); match = index;
    }
    if (work.status == XR_MODULE_OK) *output = match;
    return work.status;
}
static int graph_add_spec(ModuleWork *work, XrModuleGraph *graph, const char *canonical, const char *logical,
    const char *path, XrModuleKind kind, const XrModuleIdentityAuthority *authority) {
    if (graph->spec_count >= GRAPH_MAX_MODULES) { module_status(work,XR_MODULE_BUDGET); return -1; }
    if (graph->spec_count >= graph->spec_capacity) {
        int capacity = graph->spec_capacity*2; void *memory = graph->specs;
        if (!module_resize(work,&memory,(size_t)capacity*sizeof(*graph->specs))) return -1;
        graph->specs = memory; graph->spec_capacity = capacity;
    }
    XrModuleSpec spec = {0}; spec.kind = kind; spec.authority.kind = authority->kind;
    spec.canonical = module_dup(work,canonical); spec.logical_path = module_dup(work,logical); spec.source_path = module_dup(work,path);
    spec.authority.namespace_id = module_dup(work,authority->namespace_id);
    spec.authority.physical_root = module_dup(work,authority->physical_root); spec.topo_index = spec.scc_id = -1;
    int index = graph->spec_count;
    if (work->status == XR_MODULE_OK) module_io(work,xr_hashmap_owned_set(graph->id_index,spec.canonical,(void *)(intptr_t)(index+1)));
    if (work->status != XR_MODULE_OK) { spec_cleanup(&spec); return -1; }
    graph->specs[index] = spec; ++graph->spec_count; return index;
}
static void spec_add_dep(ModuleWork *work, XrModuleSpec *spec, int index) {
    for (int i = 0; i < spec->dep_count && module_work(work,1); ++i) if (spec->dep_indices[i] == index) return;
    if (spec->dep_count == spec->dep_capacity) {
        if (spec->dep_capacity >= GRAPH_MAX_MODULES) { module_status(work,XR_MODULE_BUDGET); return; }
        int capacity = spec->dep_capacity ? spec->dep_capacity*2 : 4; void *memory = spec->dep_indices;
        if (!module_resize(work,&memory,(size_t)capacity*sizeof(int))) return;
        spec->dep_indices = memory; spec->dep_capacity = capacity;
    }
    if (module_work(work,1)) spec->dep_indices[spec->dep_count++] = index;
}
static void graph_resolve_dependency(ModuleWork *work, XrModuleGraph *graph, int from, const char *specifier) {
    if (!specifier) { module_status(work,XR_MODULE_INVALID); return; }
    XrModuleSpec *source = &graph->specs[from]; XrModuleId id = {0};
    module_status(work,xr_compile_module_resolver_resolve(graph->resolver,specifier,source->source_path,&source->authority,&id,NULL));
    if (work->status != XR_MODULE_OK) return;
    bool embedded = false;
    if (!id.source_path && (id.kind == XR_MOD_STDLIB || id.kind == XR_MOD_PACKAGE)) {
        const char *text = NULL;
        module_io(work,xr_get_embedded_stdlib_work(work,module_io_charge,id.authority.namespace_id,&text));
        if (!text || work->status != XR_MODULE_OK) { xr_compile_module_id_cleanup(&id); return; }
        id.source_path = module_format(work,GRAPH_EMBEDDED_STDLIB_PREFIX "%s/%s.xr",id.authority.namespace_id,id.authority.namespace_id);
        embedded = true;
    }
    int target = -1;
    if (work->status == XR_MODULE_OK) module_status(work,xr_compile_module_graph_find(graph,id.canonical,&target));
    bool fresh = target < 0;
    if (fresh && work->status == XR_MODULE_OK) target = graph_add_spec(work,graph,id.canonical,id.logical_path,id.source_path,id.kind,&id.authority);
    if (work->status == XR_MODULE_OK) {
        XrModuleSpec *spec = &graph->specs[target];
        if (!fresh && (spec->representation != id.representation ||
            (id.representation == XR_MODULE_CHECKED_LIBRARY && spec->resource != id.resource))) module_status(work,XR_MODULE_INVALID);
        else if (id.representation == XR_MODULE_CHECKED_LIBRARY && (!id.resource || !id.resource->checked)) module_status(work,XR_MODULE_INVALID);
        else { spec->representation = id.representation; spec->resource = id.resource; spec->embedded_source = embedded; }
    }
    xr_compile_module_id_cleanup(&id);
    /* Adding a spec may move the entire array. Resolve the source again by index. */
    if (work->status == XR_MODULE_OK) spec_add_dep(work,&graph->specs[from],target);
}
static void graph_imports(ModuleWork *work, XrModuleGraph *graph, int index, AstNode *ast) {
    if (ast->type != AST_PROGRAM) { module_status(work,XR_MODULE_INVALID); return; }
    for (int i = 0; i < ast->as.program.count && module_work(work,1); ++i) {
        AstNode *statement = ast->as.program.statements[i]; if (!statement) continue;
        if (statement->type == AST_IMPORT_STMT) graph_resolve_dependency(work,graph,index,statement->as.import_stmt.module_name);
        else if (statement->type == AST_EXPORT_STMT && statement->as.export_stmt.from_path)
            graph_resolve_dependency(work,graph,index,statement->as.export_stmt.from_path);
    }
    if (work->status == XR_MODULE_OK) graph->specs[index].status = XR_MODSPEC_RESOLVED;
}
typedef struct GraphSourceRoot {
    const char *canonical, *logical, *path;
    XrModuleKind kind;
    const XrModuleIdentityAuthority *authority;
    const char *source;
} GraphSourceRoot;
static XrModuleStatus parse_status(XrParseStatus status) {
    switch (status) {
    case XR_PARSE_OK: return XR_MODULE_OK;
    case XR_PARSE_BUDGET: return XR_MODULE_BUDGET;
    case XR_PARSE_OUT_OF_MEMORY: return XR_MODULE_OUT_OF_MEMORY;
    case XR_PARSE_IO: return XR_MODULE_IO;
    default: return XR_MODULE_INVALID;
    }
}
static XrModuleStatus read_status(XrFileReadStatus status) {
    switch (status) {
    case XR_FILE_READ_OK: return XR_MODULE_OK;
    case XR_FILE_READ_LIMIT: return XR_MODULE_BUDGET;
    case XR_FILE_READ_OUT_OF_MEMORY: return XR_MODULE_OUT_OF_MEMORY;
    case XR_FILE_READ_MISSING: return XR_MODULE_NOT_FOUND;
    case XR_FILE_READ_IO: return XR_MODULE_IO;
    default: return XR_MODULE_INVALID;
    }
}
static void graph_expand(ModuleWork *work, XrModuleGraph *graph, const GraphSourceRoot *root) {
    int entry = -1; module_status(work,xr_compile_module_graph_find(graph,root->canonical,&entry));
    if (entry >= 0 && work->status == XR_MODULE_OK) {
        const XrModuleSpec *found = &graph->specs[entry];
        if (found->kind != root->kind || found->authority.kind != root->authority->kind ||
            !module_equal(work,found->source_path,root->path) ||
            !module_equal(work,found->authority.namespace_id,root->authority->namespace_id) ||
            !module_equal(work,found->authority.physical_root,root->authority->physical_root)) { module_status(work,XR_MODULE_INVALID); return; }
        if (found->status >= XR_MODSPEC_RESOLVED) {
            if (root->source) {
                XrFingerprint content = {0};
                module_status(work,module_resource_status(xr_compile_module_source_fingerprint(
                    graph->resources,root->source,&content)));
                if (module_work(work,sizeof(content.bytes)) &&
                    memcmp(content.bytes,found->source_content_fingerprint.bytes,sizeof(content.bytes)))
                    module_status(work,XR_MODULE_INVALID);
            }
            return;
        }
    }
    if (work->status != XR_MODULE_OK) return;
    xr_compile_resources_free(graph->topo_order); graph->topo_order = NULL; graph->topo_count = 0;
    xr_compile_resources_free(graph->cycle_desc); graph->cycle_desc = NULL; graph->has_cycle = false;
    for (int i = 0; i < graph->spec_count && module_work(work,1); ++i) graph->specs[i].topo_index = graph->specs[i].scc_id = -1;
    if (entry < 0 && work->status == XR_MODULE_OK) entry = graph_add_spec(work,graph,root->canonical,root->logical,root->path,root->kind,root->authority);
    if (work->status != XR_MODULE_OK) return;
    if (graph->entry_index < 0) graph->entry_index = entry;
    XrOsIoPolicy policy = xr_compile_io_policy(graph->resources);
    for (int index = 0; index < graph->spec_count && module_work(work,1); ++index) {
        XrModuleSpec *spec = &graph->specs[index];
        if (spec->representation == XR_MODULE_CHECKED_LIBRARY) {
            if (!spec->resource || !spec->resource->checked) { module_status(work,XR_MODULE_INVALID); break; }
            spec->status = XR_MODSPEC_RESOLVED; continue;
        }
        bool supplied = index == entry && root->source;
        if ((!spec->source_path && !supplied) || spec->status >= XR_MODSPEC_RESOLVED) continue;
        const char *text = root->source; XrFileBytes bytes = {0};
        if (!supplied && spec->embedded_source) {
            module_io(work,xr_get_embedded_stdlib_work(work,module_io_charge,spec->authority.namespace_id,&text));
            if (!text) module_status(work,XR_MODULE_INVALID);
        } else if (!supplied) {
            module_status(work,read_status(xr_os_io_read_under_root(&policy,spec->authority.physical_root,spec->logical_path,SIZE_MAX-1,&bytes)));
            for (size_t i = 0; i < bytes.size && module_work(work,1); ++i)
                if (!bytes.data[i]) { module_status(work,XR_MODULE_INVALID); break; }
            text = bytes.data;
        }
        AstNode *ast = NULL;
        if (work->status == XR_MODULE_OK) module_status(work,parse_status(xr_compile_parse_with_source(graph->compiler_session,text,spec->source_path,&ast)));
        if (work->status == XR_MODULE_OK) module_status(work,module_resource_status(
            xr_compile_module_source_fingerprint(graph->resources,text,&spec->source_content_fingerprint)));
        xr_compile_resources_free(bytes.data);
        if (work->status != XR_MODULE_OK) { if (ast) xr_program_destroy(ast); break; }
        spec->ast = ast; spec->status = XR_MODSPEC_PARSED;
        graph_imports(work,graph,index,ast);
    }
}
static XrModuleKind authority_kind(const XrModuleIdentityAuthority *authority) {
    return authority->kind == XR_MODULE_IDENTITY_MEMORY ? XR_MOD_MEMORY :
        authority->kind == XR_MODULE_IDENTITY_STDLIB ? XR_MOD_STDLIB :
        authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE;
}
static XrModuleStatus graph_result(ModuleWork *work, XrModuleGraph *graph, char **error) {
    if (work->status != XR_MODULE_OK) {
        graph_failure(graph,work->status);
        if (work->status != XR_MODULE_BUDGET && work->status != XR_MODULE_OUT_OF_MEMORY)
            module_error(graph->resources,error,work->status == XR_MODULE_NOT_FOUND ? "module not found" : "module graph build failed");
    }
    return work->status;
}
static XrModuleStatus graph_include_path(XrModuleGraph *graph, const char *path,
    const XrModuleIdentityAuthority *authority, char **error) {
    if (!path || !authority) return XR_MODULE_INVALID;
    ModuleWork work = {graph->resources,graph->resolution_status}; XrOsIoPolicy policy = xr_compile_io_policy(graph->resources);
    char *absolute = NULL, *identity = NULL, *logical = NULL;
    if (work.status == XR_MODULE_OK) module_io(&work,xr_realpath_owned(&policy,path,&absolute));
    if (work.status == XR_MODULE_OK) module_status(&work,xr_compile_module_identity_from_source(graph->resources,authority,absolute,&identity,&logical));
    if (work.status == XR_MODULE_OK) {
        GraphSourceRoot root = {identity,logical,absolute,authority_kind(authority),authority,NULL}; graph_expand(&work,graph,&root);
    }
    xr_compile_resources_free(absolute); xr_compile_resources_free(identity); xr_compile_resources_free(logical);
    return graph_result(&work,graph,error);
}
XR_FUNC XrModuleStatus xr_compile_module_graph_build(XrModuleGraph *graph, const char *path,
    const XrModuleIdentityAuthority *authority, char **error) {
    if (!graph || !graph->resources || graph->spec_count) return XR_MODULE_INVALID;
    return graph_include_path(graph,path,authority,error);
}
XR_FUNC XrModuleStatus xr_compile_module_graph_include(XrModuleGraph *graph, const char *path,
    const XrModuleIdentityAuthority *authority, char **error) {
    if (!graph || !graph->resources || graph->entry_index < 0) return XR_MODULE_INVALID;
    return graph_include_path(graph,path,authority,error);
}
XR_FUNC XrModuleStatus xr_compile_module_graph_build_source(XrModuleGraph *graph,
    const XrModuleIdentityAuthority *authority, const char *source, char **error) {
    if (!authority || authority->kind != XR_MODULE_IDENTITY_MEMORY) return XR_MODULE_INVALID;
    return xr_compile_module_graph_build_logical_source(graph,authority,NULL,NULL,source,error);
}
static XrModuleStatus graph_include_logical_source(XrModuleGraph *graph,
    const XrModuleIdentityAuthority *authority, const char *logical, const char *path, const char *source, char **error) {
    if (!source || !authority) return XR_MODULE_INVALID;
    ModuleWork work = {graph->resources,graph->resolution_status};
    char *identity = NULL, *physical = NULL, *physical_logical = NULL;
    if (work.status == XR_MODULE_OK) module_status(&work,xr_compile_module_identity_from_logical(graph->resources,authority,logical,&identity));
    if (work.status == XR_MODULE_OK) {
        if (authority->kind == XR_MODULE_IDENTITY_MEMORY) { if (path) module_status(&work,XR_MODULE_INVALID); }
        else if (authority->physical_root) {
            module_status(&work,xr_compile_module_identity_from_source(graph->resources,authority,path,&physical,&physical_logical));
            if (work.status == XR_MODULE_OK && !module_equal(&work,identity,physical)) module_status(&work,XR_MODULE_INVALID);
        } else if (authority->kind != XR_MODULE_IDENTITY_STDLIB) module_status(&work,XR_MODULE_INVALID);
    }
    if (work.status == XR_MODULE_OK) {
        GraphSourceRoot root = {identity,logical,path,authority_kind(authority),authority,source}; graph_expand(&work,graph,&root);
    }
    xr_compile_resources_free(identity); xr_compile_resources_free(physical); xr_compile_resources_free(physical_logical);
    return graph_result(&work,graph,error);
}
XR_FUNC XrModuleStatus xr_compile_module_graph_build_logical_source(XrModuleGraph *graph,
    const XrModuleIdentityAuthority *authority, const char *logical, const char *path, const char *source, char **error) {
    if (!graph || !graph->resources || graph->spec_count) return XR_MODULE_INVALID;
    return graph_include_logical_source(graph,authority,logical,path,source,error);
}
XR_FUNC XrModuleStatus xr_compile_module_graph_include_logical_source(XrModuleGraph *graph,
    const XrModuleIdentityAuthority *authority, const char *logical, const char *path, const char *source, char **error) {
    if (!graph || !graph->resources || graph->entry_index < 0) return XR_MODULE_INVALID;
    return graph_include_logical_source(graph,authority,logical,path,source,error);
}
XR_FUNC XrModuleStatus xr_compile_module_graph_add_dependency(XrModuleGraph *graph, int from, int to) {
    if (!graph || !graph->resources || from < 0 || to < 0 || from >= graph->spec_count ||
        to >= graph->spec_count || graph->entry_index < 0) return XR_MODULE_INVALID;
    ModuleWork work = {graph->resources,graph->resolution_status};
    XrModuleSpec *source = &graph->specs[from];
    int count = source->dep_count;
    spec_add_dep(&work,source,to);
    if (work.status == XR_MODULE_OK && count != source->dep_count) {
        xr_compile_resources_free(graph->topo_order); graph->topo_order = NULL; graph->topo_count = 0;
        xr_compile_resources_free(graph->cycle_desc); graph->cycle_desc = NULL; graph->has_cycle = false;
        for (int i = 0; i < graph->spec_count && module_work(&work,1); ++i)
            graph->specs[i].topo_index = graph->specs[i].scc_id = -1;
    }
    return graph_result(&work,graph,NULL);
}
/* All Tarjan metadata remains private until a complete order and any cycle
 * description have passed admission. A failed retry preserves the prior order. */
typedef struct GraphTarjanNode { int index, lowlink, scc; bool on_stack; } GraphTarjanNode;
typedef struct GraphTarjanCtx {
    ModuleWork *work; XrModuleGraph *graph; GraphTarjanNode *nodes; int *stack, *sizes;
    int stack_top, next_index, next_scc;
} GraphTarjanCtx;
static void strongconnect(GraphTarjanCtx *context, int vertex) {
    if (!module_work(context->work,1)) return;
    GraphTarjanNode *node = &context->nodes[vertex];
    node->index = node->lowlink = context->next_index++; context->stack[context->stack_top++] = vertex; node->on_stack = true;
    const XrModuleSpec *spec = &context->graph->specs[vertex];
    for (int i = 0; i < spec->dep_count && module_work(context->work,1); ++i) {
        int next = spec->dep_indices[i];
        if (next < 0 || next >= context->graph->spec_count) { module_status(context->work,XR_MODULE_INVALID); return; }
        if (context->nodes[next].index < 0) {
            strongconnect(context,next);
            if (context->work->status != XR_MODULE_OK) return;
            if (context->nodes[next].lowlink < node->lowlink) node->lowlink = context->nodes[next].lowlink;
        } else if (context->nodes[next].on_stack && context->nodes[next].index < node->lowlink) node->lowlink = context->nodes[next].index;
    }
    if (context->work->status == XR_MODULE_OK && node->lowlink == node->index) {
        int scc = context->next_scc++, next;
        do {
            if (!module_work(context->work,1)) return;
            next = context->stack[--context->stack_top]; context->nodes[next].on_stack = false;
            context->nodes[next].scc = scc; ++context->sizes[scc];
        } while (next != vertex);
    }
}
static const char *cycle_name(ModuleWork *work, const XrModuleSpec *spec) {
    if (spec->logical_path && module_work(work,1) && spec->logical_path[0]) return spec->logical_path;
    const char *name = spec->canonical ? spec->canonical : "?", *last = name;
    for (const char *p = name; module_work(work,1); ++p) { if (!*p) break; if (*p == '/') last = p+1; }
    return last;
}
static char *format_cycle(ModuleWork *work, const XrModuleGraph *graph, const int *path, int count) {
    static const char prefix[] = "E0504: circular dependency: "; size_t total = sizeof(prefix);
    for (int i = 0; i < count && module_work(work,1); ++i) {
        size_t n = module_length(work,cycle_name(work,&graph->specs[path[i]]));
        size_t separator = i ? 4 : 0;
        if (n > SIZE_MAX-total || separator > SIZE_MAX-total-n) { module_status(work,XR_MODULE_BUDGET); break; }
        total += n+separator;
    }
    char *text = module_alloc(work,total); size_t pos = sizeof(prefix)-1;
    if (!text) return NULL;
    module_copy(work,text,prefix,pos);
    for (int i = 0; i < count && module_work(work,1); ++i) {
        if (i) { module_copy(work,text+pos," -> ",4); pos += 4; }
        const char *name = cycle_name(work,&graph->specs[path[i]]); size_t n = module_length(work,name);
        if (module_copy(work,text+pos,name,n)) pos += n;
    }
    if (module_work(work,1)) text[pos] = 0;
    if (work->status != XR_MODULE_OK) { xr_compile_resources_free(text); return NULL; }
    return text;
}
static bool cycle_dfs(GraphTarjanCtx *context, int scc, int start, int current, bool *seen, int *path, int depth, int *length) {
    if (!module_work(context->work,1)) return false;
    seen[current] = true; path[depth] = current;
    const XrModuleSpec *spec = &context->graph->specs[current];
    for (int i = 0; i < spec->dep_count && module_work(context->work,1); ++i) {
        int next = spec->dep_indices[i]; if (context->nodes[next].scc != scc) continue;
        if (next == start && depth >= 1) { path[depth+1] = start; *length = depth+2; return true; }
        if (!seen[next] && cycle_dfs(context,scc,start,next,seen,path,depth+1,length)) return true;
    }
    seen[current] = false; return false;
}
static char *cycle_description(GraphTarjanCtx *context) {
    ModuleWork *work = context->work; XrModuleGraph *graph = context->graph; int n = graph->spec_count;
    for (int scc = 0; scc < context->next_scc && module_work(work,1); ++scc) {
        if (context->sizes[scc] <= 1) continue;
        bool *seen = module_calloc(work,(size_t)n,sizeof(bool)); int *path = module_calloc(work,(size_t)n+1,sizeof(int));
        char *description = NULL;
        for (int i = 0; i < n && module_work(work,1); ++i) {
            if (context->nodes[i].scc != scc) continue;
            if (!module_work(work,(uint64_t)n)) break;
            memset(seen,0,(size_t)n*sizeof(bool)); int length = 0;
            if (cycle_dfs(context,scc,i,i,seen,path,0,&length)) { description = format_cycle(work,graph,path,length); break; }
        }
        xr_compile_resources_free(seen); xr_compile_resources_free(path);
        if (!description && work->status == XR_MODULE_OK) module_status(work,XR_MODULE_INVALID);
        return description;
    }
    for (int i = 0; i < n && module_work(work,1); ++i)
        for (int edge = 0; edge < graph->specs[i].dep_count && module_work(work,1); ++edge)
            if (graph->specs[i].dep_indices[edge] == i) { int path[] = {i,i}; return format_cycle(work,graph,path,2); }
    return NULL;
}
XR_FUNC XrModuleStatus xr_compile_module_graph_topological_sort(XrModuleGraph *graph) {
    if (!graph || !graph->resources) return XR_MODULE_INVALID;
    if (graph->resolution_status != XR_MODULE_OK) return graph->resolution_status;
    if (!graph->spec_count) return XR_MODULE_OK;
    ModuleWork work = {graph->resources,XR_MODULE_OK}; int n = graph->spec_count;
    GraphTarjanNode *nodes = module_calloc(&work,(size_t)n,sizeof(*nodes));
    int *stack = module_calloc(&work,(size_t)n,sizeof(int)), *sizes = module_calloc(&work,(size_t)n,sizeof(int));
    int *order = module_calloc(&work,(size_t)n,sizeof(int));
    for (int i = 0; i < n && module_work(&work,1); ++i) nodes[i].index = nodes[i].lowlink = nodes[i].scc = -1;
    GraphTarjanCtx context = {&work,graph,nodes,stack,sizes,0,0,0};
    for (int i = 0; i < n && module_work(&work,1); ++i) if (nodes[i].index < 0) strongconnect(&context,i);
    char *description = work.status == XR_MODULE_OK ? cycle_description(&context) : NULL;
    int position = 0;
    for (int scc = 0; scc < context.next_scc && module_work(&work,1); ++scc)
        for (int i = 0; i < n && module_work(&work,1); ++i) if (nodes[i].scc == scc) order[position++] = i;
    if (work.status == XR_MODULE_OK && position != n) module_status(&work,XR_MODULE_INVALID);
    if (module_work(&work,(uint64_t)n)) {
        for (int i = 0; i < n; ++i) { int index = order[i]; graph->specs[index].topo_index = i; graph->specs[index].scc_id = nodes[index].scc; }
        xr_compile_resources_free(graph->topo_order); xr_compile_resources_free(graph->cycle_desc);
        graph->topo_order = order; order = NULL; graph->topo_count = n;
        graph->cycle_desc = description; description = NULL; graph->has_cycle = graph->cycle_desc != NULL;
    }
    xr_compile_resources_free(nodes); xr_compile_resources_free(stack); xr_compile_resources_free(sizes);
    xr_compile_resources_free(order); xr_compile_resources_free(description);
    return work.status != XR_MODULE_OK ? work.status : graph->has_cycle ? XR_MODULE_INVALID : XR_MODULE_OK;
}
XR_FUNC const char *xr_module_spec_import_name(const XrModuleSpec *spec) {
    return !spec ? NULL : spec->kind == XR_MOD_STDLIB ? spec->authority.namespace_id : spec->source_path;
}
