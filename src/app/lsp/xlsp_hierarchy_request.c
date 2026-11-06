/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_hierarchy_request.c - Ledger-owned hierarchy capture and cancellable run
 */
#include "../../base/xresource_limits.h"
#include "xlsp_hierarchy_request.h"
#include "xlsp_source_params.h"
#include "xlsp_source_scope.h"
#include "xlsp_source_call_hierarchy.h"
#include "xlsp_source_type_hierarchy.h"
#include "xlsp_source_binding_lenses.h"
#include "xlsp_source_links.h"
#include "xlsp_token_update.h"
#include "xlsp_source_workspace.h"
#include "xlsp_document_store.h"
#include "xlsp_source_syntax.h"
#include "xlsp_source_selection.h"
#include "xlsp_source_format.h"
#include "xlsp_source_on_type.h"
#include "xlsp_source_rename.h"
#include "../../base/xchecks.h"
#include "../../xir/xxir_compile_memory.h"
#include <limits.h>
#include <string.h>
#include <stdio.h>

struct XlspHierarchyRequest {
    XrXirCompileContext context;
    XlspSourceDocument *documents;
    XlspSourceBuffer **buffers;
    size_t count, root_count;
    char *roots[MAX_WORKSPACE_FOLDERS];
    XlspSourceParams *params;
    const char *uri;
    size_t uri_length;
    XlspConfig config;
    uint64_t revision;
    unsigned mode;
    XlspSourceScopeQueryKind kind;
    XlspSourceTokens *previous_tokens;
    const XrJsonValue *previous_id;
    uint32_t previous_result_id;
    int version;
    XrLspRange range;
    XrFmtConfig formatting;
    XlspSourceOnTypeInput on_type;
    XrLspPosition rename_position;
    const XrJsonValue *rename_name;
    bool started, selected;
};
static bool hierarchy_local_syntax(XlspSourceScopeQueryKind kind) {
    return kind==XLSP_SOURCE_FOLDING||kind==XLSP_SOURCE_SELECTION||kind==XLSP_SOURCE_DOCUMENT_SYMBOLS||
        kind==XLSP_SOURCE_FORMATTING||kind==XLSP_SOURCE_ON_TYPE;
}
static bool hierarchy_rename_kind(XlspSourceScopeQueryKind kind) {
    return kind==XLSP_SOURCE_RENAME||kind==XLSP_SOURCE_PREPARE_RENAME;
}
static unsigned hierarchy_stage(XlspSourceScopeQueryKind kind) {
    if(kind==XLSP_SOURCE_RENAME)return 26u;
    if(kind==XLSP_SOURCE_PREPARE_RENAME)return 27u;
    if (kind == XLSP_SOURCE_ON_TYPE) return 21u;
    if (kind == XLSP_SOURCE_FORMATTING) return 20u;
    if (kind == XLSP_SOURCE_DOCUMENT_SYMBOLS) return 19u;
    if (kind == XLSP_SOURCE_SELECTION) return 18u;
    if (kind == XLSP_SOURCE_FOLDING) return 17u;
    if (kind == XLSP_SOURCE_INLAY_BINDINGS) return 16u;
    if (kind == XLSP_SOURCE_DOCUMENT_LINKS) return 15u;
    return kind >= XLSP_SOURCE_SEMANTIC_FULL ? 11u :
        kind == XLSP_SOURCE_BINDING_LENSES ? 12u : kind == XLSP_SOURCE_TYPE_HIERARCHY ? 25u : 24u;
}
static XrXirStatus hierarchy_length(const char *text, size_t *out) {
    if (!text) return XR_XIR_BAD_STRUCTURE;
    for (size_t i = 0;; ++i) {
        if (!text[i]) { *out = i; return XR_XIR_OK; }
        if (i == SIZE_MAX - 1) return XR_XIR_OVERFLOW;
    }
}
static XrXirStatus hierarchy_string(XrCompileResources *resources, const char *text, char **out) {
    size_t length = 0;
    XrXirStatus status = hierarchy_length(text, &length);
    if (status != XR_XIR_OK) return status;
    status = xir_compile_resource_status(xr_compile_resources_alloc(resources, length + 1, (void **)out));
    if (status == XR_XIR_OK) memcpy(*out, text, length + 1);
    return status;
}
XR_FUNC void xlsp_hierarchy_request_free(XlspHierarchyRequest *request) {
    if (!request) return;
    XrCompileResources *resources = request->context.resources;
    for (size_t i = 0; (request->documents || request->buffers) && i < request->count; ++i) {
        if (request->documents) xr_compile_resources_free((char *)request->documents[i].uri);
        if (request->buffers) xlsp_source_buffer_free(request->buffers[i]);
    }
    for (size_t i = 0; i < request->root_count; ++i) xr_compile_resources_free(request->roots[i]);
    for (int i = 0; i < request->config.ignore_pattern_count; ++i)
        xr_compile_resources_free(request->config.ignore_patterns[i].pattern);
    xr_compile_resources_free(request->config.ignore_patterns);
    xr_compile_resources_free(request->config.log_path);
    xlsp_source_params_free(request->params);
    xlsp_source_tokens_free(request->previous_tokens);
    xr_compile_resources_free(request->documents); xr_compile_resources_free(request->buffers);
    xr_compile_resources_free(request);
    xr_compile_resources_release(resources);
}
static XrXirStatus hierarchy_config(XlspHierarchyRequest *request, const XlspConfig *config) {
    if (config->ignore_pattern_count < 0 || (config->ignore_pattern_count && !config->ignore_patterns))
        return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *resources = request->context.resources;
    request->config = *config;
    request->config.ignore_patterns = NULL; request->config.log_path = NULL;
    request->config.ignore_pattern_count = 0; request->config.ignore_pattern_capacity = 0;
    size_t count = (size_t)config->ignore_pattern_count;
    if (count > SIZE_MAX / sizeof(*config->ignore_patterns)) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    if (count) status = xir_compile_resource_status(xr_compile_resources_calloc(resources, count,
        sizeof(*config->ignore_patterns), (void **)&request->config.ignore_patterns));
    if (status != XR_XIR_OK) return status;
    request->config.ignore_pattern_capacity = config->ignore_pattern_count;
    for (size_t i = 0; i < count; ++i) {
        const XlspIgnorePattern *input = &config->ignore_patterns[i];
        XlspIgnorePattern *pattern = &request->config.ignore_patterns[i];
        status = hierarchy_string(resources, input->pattern, &pattern->pattern);
        if (status != XR_XIR_OK) return status;
        pattern->is_glob = input->is_glob; pattern->is_dir_only = input->is_dir_only;
        ++request->config.ignore_pattern_count;
    }
    if (config->log_path) status = hierarchy_string(resources, config->log_path, &request->config.log_path);
    return status;
}
static XrXirStatus hierarchy_semantic_document(XlspHierarchyRequest *,const XrLspDocument *,size_t);
static XrXirStatus hierarchy_document(XlspHierarchyRequest *request, size_t index,
    const XrLspDocument *doc) {
    XrCompileResources *resources = request->context.resources;
    if (!doc || !doc->content || doc->length == SIZE_MAX) return XR_XIR_BAD_STRUCTURE;
    size_t length = 0;
    XrXirStatus status = hierarchy_length(doc->uri, &length);
    if (status != XR_XIR_OK) return status;
    char *path = NULL, *uri = NULL;
    status = xlsp_source_uri_path(resources, doc->uri, length, &path); xr_compile_resources_free(path);
    if (status != XR_XIR_OK) return status;
    status = xlsp_source_buffer_new(resources, doc->content, doc->length, (int)doc->version, &request->buffers[index]);
    if (status != XR_XIR_OK) return status;
    status = hierarchy_string(resources, doc->uri, &uri);
    if (status != XR_XIR_OK) return status;
    request->documents[index] = (XlspSourceDocument){uri, xlsp_source_buffer_text(request->buffers[index]),
        length, doc->length, doc->version};
    if (request->kind >= XLSP_SOURCE_SEMANTIC_FULL) return hierarchy_semantic_document(request,doc,length);
    return XR_XIR_OK;
}
static XrXirStatus hierarchy_documents(XlspHierarchyRequest *request, const XrLspServer *server) {
    const XrLspDocTable *table = server->doc_table;
    if (!table) return XR_XIR_OK;
    if (table->doc_count < 0 || table->bucket_count <= 0 || !table->buckets) return XR_XIR_BAD_STRUCTURE;
    request->count = (size_t)table->doc_count;
    if (request->count > SIZE_MAX / sizeof(*request->documents) ||
        request->count > SIZE_MAX / sizeof(*request->buffers)) return XR_XIR_BUDGET;
    XrCompileResources *resources = request->context.resources;
    XrXirStatus status = XR_XIR_OK;
    if (request->count) status = xir_compile_resource_status(xr_compile_resources_calloc(resources, request->count,
        sizeof(*request->documents), (void **)&request->documents));
    if (status == XR_XIR_OK && request->count) status = xir_compile_resource_status(xr_compile_resources_calloc(resources,
        request->count, sizeof(*request->buffers), (void **)&request->buffers));
    if (status != XR_XIR_OK) return status;
    size_t used = 0;
    for (int i = 0; i < table->bucket_count; ++i) {
        for (const XrLspDocBucket *bucket = table->buckets[i]; bucket; bucket = bucket->next) {
            if (used == request->count || !bucket->doc || bucket->doc->server != server) return XR_XIR_BAD_STRUCTURE;
            status = hierarchy_document(request, used, bucket->doc);
            if (status != XR_XIR_OK) return status;
            ++used;
        }
    }
    return used == request->count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus hierarchy_member(const XrJsonValue *object,
    const char *key, bool unique, const XrJsonValue **out) {
    if (!object || object->type != XR_JSON_OBJECT) return XR_XIR_BAD_STRUCTURE;
    size_t length = strlen(key);
    for (int i = 0; i < object->as.object.count; ++i) {
        const XrJsonMember *member = &object->as.object.members[i];
        if (member->key_len != length) continue;
        if (!memcmp(member->key, key, length)) {
            if (*out) return XR_XIR_BAD_STRUCTURE;
            *out = member->value;
            if (!unique) return XR_XIR_OK;
        }
    }
    return *out ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus hierarchy_position(const XrJsonValue *value,XrLspPosition *out) {
    const XrJsonValue *line=NULL,*column=NULL;
    XrXirStatus status=hierarchy_member(value,"line",true,&line);
    if(status==XR_XIR_OK)status=hierarchy_member(value,"character",true,&column);
    if(status!=XR_XIR_OK)return status;
    if(line->type!=XR_JSON_NUMBER||!line->is_integer||line->as.integer<0||line->as.integer>INT32_MAX||
        column->type!=XR_JSON_NUMBER||!column->is_integer||column->as.integer<0||column->as.integer>INT32_MAX)
        return XR_XIR_BAD_STRUCTURE;
    *out=(XrLspPosition){(uint32_t)line->as.integer,(uint32_t)column->as.integer};return XR_XIR_OK;
}
#include "xlsp_source_rename_request.inc.c"
static XrXirStatus hierarchy_edit_params(XlspHierarchyRequest *request) {
    const XrJsonValue *params=xlsp_source_params_view(request->params),*options=NULL,*tabs=NULL,*spaces=NULL;
    XrXirStatus status=hierarchy_member(params,"options",true,&options);
    if(status==XR_XIR_OK)status=hierarchy_member(options,"tabSize",true,&tabs);
    if(status==XR_XIR_OK)status=hierarchy_member(options,"insertSpaces",true,&spaces);
    if(status!=XR_XIR_OK)return status;
    if(tabs->type!=XR_JSON_NUMBER||!tabs->is_integer||tabs->as.integer<=0||tabs->as.integer>INT32_MAX||
        spaces->type!=XR_JSON_BOOL)return XR_XIR_BAD_STRUCTURE;
    if(request->kind==XLSP_SOURCE_FORMATTING) {
        if(request->formatting.max_line_length<0)return XR_XIR_BAD_STRUCTURE;
        request->formatting.indent_size=(int)tabs->as.integer;
        request->formatting.use_tabs=spaces->as.boolean?0:1;
        return XR_XIR_OK;
    }
    const XrJsonValue *position=NULL,*trigger=NULL;
    status=hierarchy_member(params,"position",true,&position);
    if(status==XR_XIR_OK)status=hierarchy_position(position,&request->on_type.position);
    if(status==XR_XIR_OK)status=hierarchy_member(params,"ch",true,&trigger);
    if(status!=XR_XIR_OK)return status;
    if(trigger->type!=XR_JSON_STRING||!trigger->as.string||trigger->string_len!=1||
        (trigger->as.string[0]!='}'&&trigger->as.string[0]!='\n'&&trigger->as.string[0]!=';'))return XR_XIR_BAD_STRUCTURE;
    request->on_type.tab_size=(uint32_t)tabs->as.integer;
    request->on_type.insert_spaces=spaces->as.boolean;
    request->on_type.trigger=trigger->as.string[0];
    return XR_XIR_OK;
}
static void hierarchy_format_config(XlspHierarchyRequest *request,const XlspConfig *config) {
    request->formatting=xfmt_default_config;
    request->formatting.max_line_length=config->format_max_line_length;
    request->formatting.align_branch_arrows=config->format_align_branch_arrows?1:0;
    request->formatting.align_enum_values=config->format_align_enum_values?1:0;
    request->formatting.align_struct_fields=config->format_align_struct_fields?1:0;
    request->formatting.align_trailing_comments=config->format_align_trailing_comments?1:0;
    request->formatting.wrap_long_lines=config->format_wrap_long_lines?1:0;
    request->formatting.multiline_trailing_comma=config->format_multiline_trailing_comma?1:0;
}
static XrXirStatus hierarchy_selection_params(XlspHierarchyRequest *request) {
    const XrJsonValue *positions=NULL;
    XrXirStatus status=hierarchy_member(xlsp_source_params_view(request->params),"positions",true,&positions);
    if(status!=XR_XIR_OK)return status;
    if(positions->type!=XR_JSON_ARRAY)return XR_XIR_BAD_STRUCTURE;
    for(int i=0;i<positions->as.array.count;++i) {
        XrLspPosition point={0};status=hierarchy_position(positions->as.array.items[i],&point);
        if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus hierarchy_semantic_params(XlspHierarchyRequest *request) {
    const XrJsonValue *params=xlsp_source_params_view(request->params),*document=NULL,*uri=NULL,*range=NULL,*start=NULL,*end=NULL;
    XrXirStatus status=hierarchy_member(params,"textDocument",true,&document);
    if(status==XR_XIR_OK)status=hierarchy_member(document,"uri",true,&uri);
    if(status!=XR_XIR_OK)return status;
    if(hierarchy_rename_kind(request->kind))return hierarchy_rename_params(request);
    if(request->kind==XLSP_SOURCE_SELECTION)return hierarchy_selection_params(request);
    if(request->kind==XLSP_SOURCE_FORMATTING||request->kind==XLSP_SOURCE_ON_TYPE)return hierarchy_edit_params(request);
    if(request->kind==XLSP_SOURCE_SEMANTIC_DELTA){
        status=hierarchy_member(params,"previousResultId",true,&request->previous_id);
        if(status!=XR_XIR_OK)return status;
        const XrJsonValue *id=request->previous_id;
        if(id->type!=XR_JSON_STRING||!id->as.string)return XR_XIR_BAD_STRUCTURE;
        if(memchr(id->as.string,0,id->string_len))return XR_XIR_BAD_STRUCTURE;
    }else if(request->kind==XLSP_SOURCE_SEMANTIC_RANGE||request->kind==XLSP_SOURCE_INLAY_BINDINGS){
        status=hierarchy_member(params,"range",true,&range);
        if(status==XR_XIR_OK)status=hierarchy_member(range,"start",true,&start);
        if(status==XR_XIR_OK)status=hierarchy_member(range,"end",true,&end);
        if(status==XR_XIR_OK)status=hierarchy_position(start,&request->range.start);
        if(status==XR_XIR_OK)status=hierarchy_position(end,&request->range.end);
        if(status!=XR_XIR_OK)return status;
        if(request->range.start.line>request->range.end.line||
            (request->range.start.line==request->range.end.line&&request->range.start.character>request->range.end.character))
            return XR_XIR_BAD_STRUCTURE;
    }
    return status;
}
static XrXirStatus hierarchy_semantic_document(XlspHierarchyRequest *request,const XrLspDocument *doc,size_t length) {
    if(length!=request->uri_length)return XR_XIR_OK;
    XrCompileResources *resources=request->context.resources;
    XrXirStatus status=XR_XIR_OK;
    if(memcmp(doc->uri,request->uri,length))return XR_XIR_OK;
    if(request->selected)return XR_XIR_BAD_STRUCTURE;
    request->selected=true;request->version=doc->version;
    if (request->kind == XLSP_SOURCE_DOCUMENT_LINKS||request->kind==XLSP_SOURCE_INLAY_BINDINGS||
        hierarchy_local_syntax(request->kind)||hierarchy_rename_kind(request->kind)) return XR_XIR_OK;
    request->previous_result_id=doc->sem_token_result_id;
    if(request->kind!=XLSP_SOURCE_SEMANTIC_RANGE&&doc->sem_token_result_id==UINT32_MAX)return XR_XIR_BUDGET;
    if(request->kind==XLSP_SOURCE_SEMANTIC_DELTA&&doc->source_tokens){
        char id[16];int size=snprintf(id,sizeof(id),"%u",doc->sem_token_result_id);
        if(size<=0||(size_t)size>=sizeof(id))return XR_XIR_BAD_STRUCTURE;
        const XrJsonValue *previous=request->previous_id;
        if(previous->string_len==(size_t)size){
            if(!memcmp(previous->as.string,id,(size_t)size))
                status=xlsp_source_tokens_clone(resources,doc->source_tokens,&request->previous_tokens);
        }
    }
    return status;
}
static XrXirStatus hierarchy_syntax_document(XlspHierarchyRequest *request, const XrLspServer *server) {
    const XrLspDocTable *table = server->doc_table;
    if (!table || table->doc_count <= 0 || table->bucket_count <= 0 || !table->buckets)
        return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *resources = request->context.resources;
    const XrLspDocument *doc = xlsp_document_store_get(table, request->uri);
    if (!doc || doc->server != server) return XR_XIR_BAD_STRUCTURE;
    request->count = 1;
    XrXirStatus status = xir_compile_resource_status(xr_compile_resources_calloc(resources, 1,
        sizeof(*request->documents), (void **)&request->documents));
    if (status == XR_XIR_OK) status = xir_compile_resource_status(xr_compile_resources_calloc(resources, 1,
        sizeof(*request->buffers), (void **)&request->buffers));
    if (status == XR_XIR_OK) status = hierarchy_document(request, 0, doc);
    return status != XR_XIR_OK ? status : request->selected ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus hierarchy_inputs(XlspHierarchyRequest *request, const XrLspServer *server,
    const XrJsonValue *params) {
    XrCompileResources *resources = request->context.resources;
    XrXirStatus status = xir_compile_resource_status(xlsp_source_params_new(resources, params, &request->params));
    if (status != XR_XIR_OK) return status;
    const XrJsonValue *owned = xlsp_source_params_view(request->params);
    const XrJsonValue *object = NULL, *uri = NULL;
    status = hierarchy_member(owned, request->mode ? "item" : "textDocument", false, &object);
    if (status == XR_XIR_OK) status = hierarchy_member(object, "uri", false, &uri);
    if (status != XR_XIR_OK) return status;
    if (uri->type != XR_JSON_STRING || !uri->as.string) return XR_XIR_BAD_STRUCTURE;
    char *path = NULL;
    status = xlsp_source_uri_path(resources, uri->as.string, uri->string_len, &path); xr_compile_resources_free(path);
    if (status != XR_XIR_OK) return status;
    request->uri = uri->as.string; request->uri_length = uri->string_len;
    if (request->kind >= XLSP_SOURCE_SEMANTIC_FULL) {
        if(request->kind==XLSP_SOURCE_INLAY_BINDINGS) {
            request->config.inlay_hints_parameter_names=server->config.inlay_hints_parameter_names;
            request->config.inlay_hints_type_annotations=server->config.inlay_hints_type_annotations;
        }
        if(request->kind==XLSP_SOURCE_FORMATTING)hierarchy_format_config(request,&server->config);
        status=hierarchy_semantic_params(request);
        if(status!=XR_XIR_OK)return status;
        if(status==XR_XIR_OK&&hierarchy_local_syntax(request->kind))
            return hierarchy_syntax_document(request,server);
        if(!hierarchy_rename_kind(request->kind)) {
            status=hierarchy_documents(request,server);
            return status!=XR_XIR_OK?status:request->selected?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
        }
    }
    status = hierarchy_config(request, &server->config);
    if (status != XR_XIR_OK) return status;
    status = hierarchy_documents(request, server);
    if (status != XR_XIR_OK) return status;
    request->root_count = (size_t)server->workspace_folder_count;
    for (size_t i = 0; i < request->root_count; ++i) {
        status = hierarchy_string(resources, server->workspace_folders[i].path, &request->roots[i]);
        if (status != XR_XIR_OK) return status;
    }
    return hierarchy_rename_kind(request->kind)&&!request->selected?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_hierarchy_request_capture(XrLspServer *server, const XrJsonValue *params,
    unsigned mode, XlspSourceScopeQueryKind kind, XlspHierarchyRequest **out) {
    if (!server || !out || *out || kind < XLSP_SOURCE_CALL_HIERARCHY ||
        kind > XLSP_SOURCE_PREPARE_RENAME || mode > 2 || (kind >= XLSP_SOURCE_BINDING_LENSES && mode))
        return XR_XIR_BAD_STRUCTURE;
    server->source_failure.stage = hierarchy_stage(kind); server->source_failure.status = XR_XIR_BAD_STRUCTURE;
    if (!params || server->shutdown_received || server->workspace_folder_count < 0 ||
        server->workspace_folder_count > MAX_WORKSPACE_FOLDERS) return XR_XIR_BAD_STRUCTURE;
    const XrCompileResourceLimits limits = XR_RESOURCE_TOOL_COMPILER_LIMITS;
    XrCompileResources *resources = NULL; XlspHierarchyRequest *request = NULL;
    XrXirStatus status = xir_compile_resource_status(xr_compile_resources_new(&limits, &resources));
    if (status != XR_XIR_OK) goto done;
    status = xir_compile_resource_status(xr_compile_resources_calloc(resources, 1, sizeof(*request), (void **)&request));
    if (status != XR_XIR_OK) goto done;
    request->context = (XrXirCompileContext){resources, xr_xir_compile_default_limits()};
    // Transfer the initial ledger lease with the owner, including failure cleanup.
    resources = NULL;
    request->mode = mode; request->kind = kind; request->revision = server->source_revision;
    status = hierarchy_inputs(request, server, params);
    if (status == XR_XIR_OK) { *out = request; request = NULL; }
done:
    xlsp_hierarchy_request_free(request); xr_compile_resources_release(resources);
    server->source_failure.status = (unsigned)status;
    if (status == XR_XIR_OK) server->source_failure.stage = 0;
    return status;
}
static bool hierarchy_cancelled(const _Atomic bool *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_acquire);
}
static bool hierarchy_cancel_poll(const void *context) {
    return hierarchy_cancelled((const _Atomic bool *)context);
}
static XrXirStatus hierarchy_lenses(XlspHierarchyRequest *request, XlspSourceScope *scope,
    size_t entry, XrJsonValue **out, XrCompileRequestControl *control) {
    const XrJsonValue *document = NULL, *uri = NULL;
    XrXirStatus status = hierarchy_member(xlsp_source_params_view(request->params),
        "textDocument", true, &document);
    if (status == XR_XIR_OK) status = hierarchy_member(document, "uri", true, &uri);
    if (status != XR_XIR_OK) return status;
    return xlsp_source_binding_lenses_json(&request->context, xlsp_source_scope_documents(scope),
        xlsp_source_scope_count(scope), entry, out,control);
}
static XrXirStatus hierarchy_semantic(XlspHierarchyRequest *request,XlspSourceRequestResult *result,XrCompileRequestControl *control) {
    XlspSourceWorkspace *workspace=NULL;XlspSourceTokens *tokens=NULL;
    const XrLspRange *range=request->kind==XLSP_SOURCE_SEMANTIC_RANGE?&request->range:NULL;
    XrXirStatus status=xlsp_source_workspace_build(&request->context,request->documents,
        request->count,&workspace,&result->stage,NULL,control);
    if(status!=XR_XIR_OK)goto done;
    result->stage=11;
    status=xlsp_source_workspace_tokens(workspace,request->uri,request->uri_length,range,&tokens);
    if(status!=XR_XIR_OK)goto done;
    status=xlsp_source_tokens_json(request->context.resources,tokens,request->previous_tokens,
        range?0:request->previous_result_id+1,request->kind==XLSP_SOURCE_SEMANTIC_DELTA,range,&result->json);
    if(status==XR_XIR_OK&&!range){
        XlspTokenUpdateInput input={request->context.resources,request->uri,request->uri_length,
            request->version,request->previous_result_id,&tokens};
        status=xlsp_token_update_new(&input,&result->token_update);
    }
done:
    xlsp_source_workspace_free(workspace);xlsp_source_tokens_free(tokens);
    if(status!=XR_XIR_OK){
        xjson_free(result->json);result->json=NULL;
        xlsp_token_update_free(result->token_update);result->token_update=NULL;
    }
    return status;
}
static XrXirStatus hierarchy_links(XlspHierarchyRequest *request, XrJsonValue **out) {
    size_t entry = SIZE_MAX;
    for (size_t i = 0; i < request->count; ++i) {
        const XlspSourceDocument *document = &request->documents[i];
        if (document->uri_length != request->uri_length) continue;
        if (!memcmp(document->uri, request->uri, request->uri_length)) entry = i;
    }
    if (entry == SIZE_MAX) return XR_XIR_BAD_STRUCTURE;
    XlspSourceLinks *links = NULL;
    XrXirStatus status = xlsp_source_links_build(&request->context, request->documents, request->count, entry, &links);
    if (status == XR_XIR_OK) status = xlsp_source_links_json(links, out);
    xlsp_source_links_free(links);
    return status;
}
static XrXirStatus hierarchy_parameters(XlspHierarchyRequest *request,XrJsonValue **out,XrCompileRequestControl *control) {
    const XlspSourceDocument *document=NULL;
    for(size_t i=0;i<request->count;++i) {
        if(request->documents[i].uri_length!=request->uri_length)continue;
        if(!memcmp(request->documents[i].uri,request->uri,request->uri_length)){document=&request->documents[i];break;}
    }
    if(!document)return XR_XIR_BAD_STRUCTURE;
    XlspSourceCursor first={0},last={0};
    XrXirStatus status=xlsp_source_cursor(request->context.resources,document->text,document->length,request->range.start,&first);
    if(status==XR_XIR_OK)status=xlsp_source_cursor(request->context.resources,document->text,document->length,request->range.end,&last);
    if(status!=XR_XIR_OK)return status;
    if(!request->config.inlay_hints_parameter_names&&!request->config.inlay_hints_type_annotations) {
        XrJsonValue *empty=xjson_new_array();if(!empty)return XR_XIR_OUT_OF_MEMORY;*out=empty;return XR_XIR_OK;
    }
    XlspSourceWorkspace *workspace=NULL;
    status=xlsp_source_workspace_build(&request->context,request->documents,request->count,&workspace,NULL,NULL,control);
    XlspSourceInlayOptions options={request->range,request->config.inlay_hints_type_annotations,request->config.inlay_hints_parameter_names};
    if(status==XR_XIR_OK)status=xlsp_source_workspace_inlay_bindings(workspace,request->uri,request->uri_length,&options,out);
    xlsp_source_workspace_free(workspace);return status;
}
static XrXirStatus hierarchy_syntax(XlspHierarchyRequest *request, XrJsonValue **out,XrCompileRequestControl *control) {
    if (!request->selected || request->count != 1) return XR_XIR_BAD_STRUCTURE;
    XrCompilerSession *session = NULL;
    XlspSyntaxSnapshot *syntax = NULL;
    XlspSourceOutline *outline = NULL;
    XrCompileResourceStatus opened = xr_compile_session_new(request->context.resources, &session);
    XrXirStatus status = xir_compile_resource_status(opened);
    if (status != XR_XIR_OK) goto done;
    if(control){status=xir_compile_resource_status(xr_compile_session_bind_request(session,control));if(status!=XR_XIR_OK)goto done;}
    XrParseStatus parsed = xlsp_source_syntax_build(session, &request->documents[0], &syntax);
    if (parsed != XR_PARSE_OK && parsed != XR_PARSE_RECOVERED) {
        switch(parsed) {
        case XR_PARSE_BUDGET:status=XR_XIR_BUDGET;break;
        case XR_PARSE_MEMORY_LIMIT:status=XR_XIR_MEMORY_LIMIT;break;
        case XR_PARSE_OVERFLOW:status=XR_XIR_OVERFLOW;break;
        case XR_PARSE_CANCELLED:status=XR_XIR_CANCELLED;break;
        case XR_PARSE_DEADLINE:status=XR_XIR_DEADLINE;break;
        case XR_PARSE_CLOCK_FAILURE:status=XR_XIR_CLOCK_FAILURE;break;
        case XR_PARSE_OUT_OF_MEMORY:status=XR_XIR_OUT_OF_MEMORY;break;
        default:status=XR_XIR_BAD_STRUCTURE;break;
        }
        goto done;
    }
    if(request->kind==XLSP_SOURCE_FOLDING)status=xlsp_source_syntax_folding_json(syntax,out);
    else {
        status=xlsp_source_syntax_outline(syntax,&outline);
        if(status==XR_XIR_OK)status=xlsp_source_outline_json(outline,out);
    }
done:
    xlsp_source_outline_free(outline);
    xlsp_source_syntax_free(syntax);
    xr_compile_session_free(session);
    return status;
}
static XrXirStatus hierarchy_selection(XlspHierarchyRequest *request,XrJsonValue **out) {
    if(!request->selected||request->count!=1)return XR_XIR_BAD_STRUCTURE;
    const XrJsonValue *positions=NULL;
    XrXirStatus status=hierarchy_member(xlsp_source_params_view(request->params),
        "positions",true,&positions);
    if(status!=XR_XIR_OK)return status;
    const XlspSourceDocument *document=&request->documents[0];
    return xlsp_source_selection_json(request->context.resources,document->text,document->length,positions,out);
}
XR_FUNC XrXirStatus xlsp_hierarchy_request_run(XlspHierarchyRequest *request,
    const _Atomic bool *cancel, XlspSourceRequestResult *out) {
    if (!request || request->started || !out || out->json || out->token_update || out->stage || out->cancelled ||
        out->status != XR_XIR_OK) return XR_XIR_BAD_STRUCTURE;
    request->started = true;
    XlspSourceRequestResult result = {.stage = hierarchy_stage(request->kind)};
    XlspSourceScope *scope = NULL; XrCompileRequestControl control = {0};
    XrCompileRequestControl *current = cancel ? &control : NULL;
    if (hierarchy_cancelled(cancel)) { result.cancelled = true; goto done; }
    if (cancel) {
        const XrCompileRequestOptions options = {.cancel_poll=hierarchy_cancel_poll,
            .cancel_context=cancel,.deadline_mode=XR_COMPILE_DEADLINE_NONE};
        result.status = xir_compile_resource_status(xr_compile_request_control_init(&options,&control));
        if (result.status != XR_XIR_OK) goto done;
    }
    if(current){result.status=xir_compile_resource_status(xr_compile_request_control_poll(current));if(result.status!=XR_XIR_OK)goto done;}
    if(request->kind>=XLSP_SOURCE_SEMANTIC_FULL&&!hierarchy_rename_kind(request->kind)){
        result.status=request->kind==XLSP_SOURCE_FORMATTING ?
            xlsp_source_format_json(request->context.resources,&request->documents[0],&request->formatting,&result.json,current) :
            request->kind==XLSP_SOURCE_ON_TYPE ?
            xlsp_source_on_type_json(request->context.resources,&request->documents[0],&request->on_type,&result.json,current) :
            request->kind==XLSP_SOURCE_SELECTION ? hierarchy_selection(request,&result.json) :
            hierarchy_local_syntax(request->kind) ? hierarchy_syntax(request,&result.json,current) :
            request->kind==XLSP_SOURCE_INLAY_BINDINGS ? hierarchy_parameters(request,&result.json,current) :
            request->kind==XLSP_SOURCE_DOCUMENT_LINKS ? hierarchy_links(request,&result.json) :
            hierarchy_semantic(request,&result,current);
        if(result.status==XR_XIR_OK)result.stage=0;
        goto done;
    }
    const char *roots[MAX_WORKSPACE_FOLDERS];
    for (size_t i = 0; i < request->root_count; ++i) roots[i] = request->roots[i];
    XlspSourceScopeOptions options = {&request->context, request->documents, request->count, roots,
        request->root_count, UINT32_C(50)*1024*1024, &request->config};
    result.status = xlsp_source_scope_capture(&options, &scope);
    if (result.status != XR_XIR_OK) goto done;
    if(current){result.status=xir_compile_resource_status(xr_compile_request_control_poll(current));if(result.status!=XR_XIR_OK)goto done;}
    size_t entry = SIZE_MAX;
    result.status = xlsp_source_scope_find(scope, request->uri, request->uri_length, &entry);
    if (result.status != XR_XIR_OK) goto done;
    if(hierarchy_rename_kind(request->kind))result.status=hierarchy_rename_run(request,scope,entry,&result.json,current);
    else if (request->kind == XLSP_SOURCE_BINDING_LENSES) result.status = hierarchy_lenses(request, scope, entry, &result.json,current);
    else {
        XlspSourceHierarchyRequest input = {&request->context, xlsp_source_scope_documents(scope),
            xlsp_source_scope_known_versions(scope), xlsp_source_scope_count(scope), entry,
            xlsp_source_params_view(request->params), request->revision, request->mode};
        result.status = request->kind == XLSP_SOURCE_TYPE_HIERARCHY ? xlsp_source_type_hierarchy_json(&input, &result.json,current) :
            xlsp_source_call_hierarchy_json(&input, &result.json,current);
    }
    if (result.status == XR_XIR_OK) result.stage = 0;
done:
    xlsp_source_scope_free(scope);
    if(current&&control.initialized){
        XrXirStatus polled=xir_compile_resource_status(xr_compile_request_control_poll(current));
        if(result.status==XR_XIR_OK&&polled!=XR_XIR_OK){result.status=polled;result.stage=hierarchy_stage(request->kind);}
        if(control.status==XR_COMPILE_RESOURCE_CANCELLED)result.cancelled=true;
    }
    if (hierarchy_cancelled(cancel)) result.cancelled = true;
    if (result.cancelled || result.status != XR_XIR_OK) {
        xjson_free(result.json); result.json = NULL;
        xlsp_token_update_free(result.token_update);result.token_update=NULL;
    }
    *out = result; return XR_XIR_OK;
}
XR_FUNC XrJsonValue *xlsp_source_scope_query(XrLspServer *server, const XrJsonValue *params,
    unsigned mode, XlspSourceScopeQueryKind kind) {
    if (!server) return NULL;
    uint64_t revision = server->source_revision;
    uint64_t generation = server->doc_table ? server->doc_table->generation : 0;
    XlspHierarchyRequest *request = NULL; XlspSourceRequestResult result = {0};
    XrXirStatus status = xlsp_hierarchy_request_capture(server, params, mode, kind, &request);
    if (status == XR_XIR_OK) status = xlsp_hierarchy_request_run(request, NULL, &result);
    if (status == XR_XIR_OK) status = result.status;
    xlsp_hierarchy_request_free(request);
    if (status == XR_XIR_OK && (server->shutdown_received || server->source_revision != revision ||
        (server->doc_table ? server->doc_table->generation : 0) != generation)) status = XR_XIR_UNRESOLVED;
    if(status==XR_XIR_OK&&result.token_update&&!xlsp_token_update_commit(server,result.token_update))
        status=XR_XIR_UNRESOLVED;
    xlsp_token_update_free(result.token_update);result.token_update=NULL;
    if (status != XR_XIR_OK) { xjson_free(result.json); result.json = NULL; }
    server->source_failure.status = (unsigned)status;
    server->source_failure.stage = status == XR_XIR_OK ? 0u : hierarchy_stage(kind);
    return result.json;
}
