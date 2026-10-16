/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression_source.inc.c - Source-owned result types for public data entries
 *
 * KEY CONCEPT:
 *   Inferred data shapes are spelled from actual owned Source facts. The
 *   ordinary compiler then creates and verifies the public execution entry.
 */
#include "../../xir/xxir_types.h"
static const char *expression_primitive(XrXirType type) {
    switch (type) {
    case XR_XIR_UNIT:return "()";case XR_XIR_BOOL:return "bool";case XR_XIR_STRING:return "string";
    case XR_XIR_I8:return "i8";case XR_XIR_I16:return "i16";case XR_XIR_I32:return "i32";case XR_XIR_I64:return "i64";
    case XR_XIR_U8:return "u8";case XR_XIR_U16:return "u16";case XR_XIR_U32:return "u32";case XR_XIR_U64:return "u64";
    case XR_XIR_F32:return "f32";case XR_XIR_F64:return "f64";case XR_XIR_RUNE:return "rune";default:return NULL;
    }
}
static bool expression_type(XdapJsonWriter *text,const XrXirTypes *types,XrXirType type,uint32_t depth) {
    if (depth==32) { text->status=XR_XIR_BUDGET;return false; }
    if (xr_compile_resources_work(text->resources,1)!=XR_COMPILE_RESOURCE_OK) { text->status=XR_XIR_BUDGET;return false; }
    const char *primitive=expression_primitive(type);
    if (primitive) return xdap_json_text(text,primitive,strlen(primitive));
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if (!node || node->parameter_span) { text->status=XR_XIR_BAD_TYPE;return false; }
    if (node->kind==XR_XIR_TYPE_ARRAY)
        return xdap_json_text(text,"Array<",6) && expression_type(text,types,node->element,depth+1) && xdap_json_text(text,">",1);
    if (node->kind==XR_XIR_TYPE_NULLABLE)
        return expression_type(text,types,node->element,depth+1) && xdap_json_text(text,"?",1);
    if (node->kind!=XR_XIR_TYPE_TUPLE) { text->status=XR_XIR_BAD_TYPE;return false; }
    if (!xdap_json_text(text,"(",1)) return false;
    for (uint32_t p=0;p<node->parameter_count;++p) {
        if (p && !xdap_json_text(text,",",1)) return false;
        if (!expression_type(text,types,node->parameters[p].type,depth+1)) return false;
    }
    return (node->parameter_count!=1 || xdap_json_text(text,",",1)) && xdap_json_text(text,")",1);
}
static XrXirStatus expression_function(const XrXirSourceProduct *product,uint32_t count,uint32_t *output) {
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(product);
    const XrXirSourceView *facts=xr_xir_compile_source_product_view(product);
    const XrXirCompileContext *context=xr_xir_compile_source_product_context(product);uint32_t function=UINT32_MAX;
    if (!debug || !facts || !context) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<debug->function_count;++f) {
        if (xr_compile_resources_work(context->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
        const XrXirSourceDebugFunction *entry=&debug->functions[f];
        if (!entry->declaration || entry->declaration>facts->declaration_count) continue;
        const XrXirSourceDeclaration *declaration=&facts->declarations[entry->declaration-1];
        if (declaration->kind!=XR_XIR_SOURCE_FUNCTION) continue;
        if (function!=UINT32_MAX || entry->parameter_count!=count || declaration->parent) return XR_XIR_BAD_STRUCTURE;
        function=f;
    }
    if (function==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    *output=function;return XR_XIR_OK;
}
#include "xdap_expression_data.inc.c"
static XrXirStatus expression_export(ExpressionState *state,const ExpressionData *data,
    XrXirSourceProduct **product,uint32_t *function) {
    const XdapExpressionRequest *request=state->request;
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(*product);
    const XrXirSourceView *facts=xr_xir_compile_source_product_view(*product);
    const XrXirSourceDeclaration *declaration=&facts->declarations[debug->functions[*function].declaration-1];
    XdapJsonWriter text=xdap_json_writer(request->lexical->launch->resources,request->source_limit);
    if (!declaration->type.known || declaration->type.generic_owner) text.status=XR_XIR_BAD_TYPE;
    xdap_json_text(&text,"export fn evaluator(",20);
    xdap_json_text(&text,data->parameters.bytes,data->parameters.length);
    xdap_json_text(&text,") -> ",5);expression_type(&text,facts->types,declaration->type.type,0);
    xdap_json_text(&text," {\n",3);xdap_json_text(&text,data->body.bytes,data->body.length);
    xdap_json_text(&text,"return (",8);xdap_json_text(&text,request->text,request->length);xdap_json_text(&text,"\n) }\n",5);
    XrXirStatus status=text.status;XrSourceCompileDiagnostic diagnostic={0};
    xr_xir_compile_source_product_free(*product);*product=NULL;
    if (status==XR_XIR_OK) {
        XrSourceCompilerText input={NULL,"dap-managed-expression",text.bytes,text.length};
        XrSourceCompileStatus built=xr_source_compiler_build_text(request->compiler,&input,product,&diagnostic);
        status=built==XR_SOURCE_COMPILE_OK ? XR_XIR_OK : built==XR_SOURCE_COMPILE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            built==XR_SOURCE_COMPILE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
    }
    if (status==XR_XIR_OK) status=expression_intrinsic_proof(state,*product);
    if (status==XR_XIR_OK) status=expression_function(*product,data->count,function);
    xr_source_compile_diagnostic_free(&diagnostic);xdap_json_writer_free(&text);return status;
}
