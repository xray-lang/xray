/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_core_calls_owner.inc.c - Governed intrinsic call observations
 */
#include "xxir_core_source_identity.h"
#include "../module/xmodule_fingerprint.h"
#include "../shared/xr_core_intrinsic.h"
#include "../module/xmodule_identity.h"
#include "xir_core_declarations_source.inc.c"

static bool query_call_core_name(SourceQueryCopy *copy,uint32_t target,const char *name) {
    const XrXirSourceDeclarationSyntax *syntax=&copy->snapshot->syntax.declarations[target-1];
    XrXirSourceRange range=syntax->name;
    size_t bytes=query_length(copy,name),offset=0;
    if(bytes<2)return false;
    --bytes;
    if(syntax->role!=XR_XIR_SOURCE_SYNTAX_NAME||syntax->flags||range.line<=0||range.column<=0||
        range.end_line!=range.line||(uint64_t)range.end_column!=(uint64_t)range.column+bytes)return false;
    int line=1,column=1;
    while(offset<sizeof(xir_core_declaration_source)-1&&(line!=range.line||column!=range.column)) {
        if(!((copy)->status == XR_XIR_OK))return false;
        if(xir_core_declaration_source[offset++]=='\n'){++line;column=1;}else ++column;
    }
    if(offset+bytes>=sizeof(xir_core_declaration_source)-1||!((copy)->status == XR_XIR_OK))return false;
    if(memcmp(xir_core_declaration_source+offset,name,bytes))return false;
    char next=xir_core_declaration_source[offset+bytes];
    return next=='('||next=='<';
}
static XrXirStatus query_call_core(SourceQueryCopy *copy,uint32_t target) {
    const XrXirSourceView *view=&copy->snapshot->view;
    if(!target||target>view->declaration_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDeclaration *callee=&view->declarations[target-1];
    uint32_t id=callee->native_identity;
    if(callee->kind!=XR_XIR_SOURCE_INTRINSIC||callee->parent||callee->target||
        (id!=XR_CORE_BUILTIN_ASSERT&&id!=XR_CORE_BUILTIN_ASSERT_EQUAL&&id!=XR_CORE_BUILTIN_ASSERT_PANICS)||
        callee->range.module>=view->module_count)return XR_XIR_BAD_STRUCTURE;
    if(!((copy)->status == XR_XIR_OK))return copy->status;
    const XrCoreIntrinsicDesc *descriptor=xr_core_intrinsic_by_id((XrCoreBuiltinId)id);
    if(!descriptor||!query_call_text_equal(copy,callee->name,descriptor->source_name)||
        callee->parameter_count!=descriptor->max_arity||!callee->type.known||callee->type.type!=XR_XIR_UNIT||
        callee->type.generic_owner||callee->generic_parameter_count!=(id==XR_CORE_BUILTIN_ASSERT?0u:1u)||
        !query_call_core_name(copy,target,descriptor->source_name))
        return copy->status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:copy->status;
    const XrXirSourceQueryModule *module=&view->modules[callee->range.module];
    if(module->path)return XR_XIR_BAD_STRUCTURE;
    XrFingerprint expected={{0}};
    xr_module_source_fingerprint(xir_core_declaration_source,&expected);
    if(!((copy)->status == XR_XIR_OK))return copy->status;
    if(memcmp(&module->fingerprint,&expected,sizeof(expected)))return XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,XIR_CORE_SOURCE_NAMESPACE,NULL};
    char *identity=NULL;
    XrModuleStatus status=xr_compile_module_identity_from_logical(copy->snapshot->context.resources,&authority,"",&identity);
    if(status!=XR_MODULE_OK)return status==XR_MODULE_BUDGET?XR_XIR_BUDGET:
        status==XR_MODULE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    bool matches=query_call_text_equal(copy,module->identity,identity);
    xr_compile_resources_free(identity);
    return copy->status!=XR_XIR_OK?copy->status:matches?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
