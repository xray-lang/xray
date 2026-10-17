/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_rename_request.inc.c - Owned rename parameters and complete corpus
 *
 * KEY CONCEPT:
 *   Parameters borrow only the request's immutable DOM, never editor producers.
 *   Source alone authenticates the complete binding edits on the original ledger.
 */
static XrXirStatus hierarchy_rename_params(XlspHierarchyRequest *request) {
    XrCompileResources *resources=request->context.resources;
    const XrJsonValue *params=xlsp_source_params_view(request->params),*position=NULL;
    XrXirStatus status=hierarchy_member(resources,params,"position",true,&position);
    if(status==XR_XIR_OK)status=hierarchy_position(resources,position,&request->rename_position);
    if(status!=XR_XIR_OK||request->kind==XLSP_SOURCE_PREPARE_RENAME)return status;
    status=hierarchy_member(resources,params,"newName",true,&request->rename_name);
    if(status!=XR_XIR_OK)return status;
    const XrJsonValue *name=request->rename_name;
    if(name->type!=XR_JSON_STRING||!name->as.string||!name->string_len||name->string_len>INT32_MAX)
        return XR_XIR_BAD_STRUCTURE;
    status=hierarchy_resource(xr_compile_resources_work(resources,name->string_len));
    if(status!=XR_XIR_OK)return status;
    return memchr(name->as.string,0,name->string_len)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}
static XrXirStatus hierarchy_rename_run(XlspHierarchyRequest *request,XlspSourceScope *scope,
    size_t entry,XrJsonValue **out) {
    const XrJsonValue *name=request->rename_name;
    XlspSourceRenameInput input={entry,request->rename_position,name?name->as.string:NULL,
        name?name->string_len:0,request->kind==XLSP_SOURCE_PREPARE_RENAME,
        xlsp_source_scope_known_versions(scope)};
    return xlsp_source_rename_json(&request->context,xlsp_source_scope_documents(scope),
        xlsp_source_scope_count(scope),&input,out);
}
