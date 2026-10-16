/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_math.inc.c - Governed private math leaves with owned query facts
 *
 * KEY CONCEPT:
 *   Lexical lookup precedes this exact standard module authority check. Native
 *   display identities never become declarations that user code can import.
 */
#include "xxir_math_ir.h"
#include "xir_math_declarations.inc.c"
static const SourceMathDeclaration *source_math_primitive(SourceContext *ctx,const char *name) {
    const XrModuleSpec *spec=&ctx->graph->specs[ctx->module];
    if (spec->authority.kind!=XR_MODULE_IDENTITY_STDLIB || !spec->authority.namespace_id ||
        !source_text_same(ctx,NULL,spec->authority.namespace_id,"math") || !spec->logical_path ||
        !source_text_same(ctx,NULL,spec->logical_path,"math/math.xr")) return NULL;
    for (size_t i=0;i<sizeof(source_math_declarations)/sizeof(*source_math_declarations);++i) {
        if (!source_work(ctx,NULL)) return NULL;
        if (source_text_same(ctx,NULL,name,source_math_declarations[i].name)) return &source_math_declarations[i];
    }
    return NULL;
}
static bool source_math_query(SourceContext *ctx,AstNode *callee,const SourceMathDeclaration *native,
    XrXirMathSignature signature) {
    uint32_t identity=UINT32_C(0x10000000)+native->operation;
    uint32_t declaration=source_primitive_query_find(ctx,XR_XIR_SOURCE_INTRINSIC,0,identity);
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (!declaration) {
        if (!ctx->math_module) {
            const char *name=source_owned_text(ctx,"xray-native:stdlib/math");
            const char *path=source_owned_text(ctx,"stdlib/defs/core.def");
            if (!name || !path) return false;
            XrXirSourceQueryModule *modules=source_query_append(ctx,ctx->query.modules,
                &ctx->query.module_count,&ctx->query_module_capacity,sizeof(*modules));
            if (!modules) return false;
            ctx->query.modules=modules;ctx->math_module=ctx->query.module_count;
            modules[ctx->math_module-1]=(XrXirSourceQueryModule){name,path,source_math_fingerprint};
        }
        SourceName symbol={0};symbol.name=source_owned_text(ctx,native->name);
        XrXirSourceRange range={ctx->math_module-1,(int)native->line,(int)native->column,
            (int)native->line,(int)(native->column+source_text_size(ctx,native->name))};
        if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_INTRINSIC,0,range)) return false;
        declaration=symbol.declaration;
        const char *text=source_owned_text(ctx,native->signature);
        XrXirSourceType *parameters=signature.arity ? source_alloc(ctx,signature.arity,sizeof(*parameters)) : NULL;
        if (!text || (signature.arity && !parameters)) return false;
        for (uint32_t i=0;i<signature.arity;++i) parameters[i]=(XrXirSourceType){signature.parameter,0,true};
        XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
        record->native_identity=identity;record->signature=text;
        record->type=(XrXirSourceType){signature.result,0,true};
        record->parameters=parameters;record->parameter_count=signature.arity;
    }
    return source_query_target_token_reference(ctx,callee,declaration,XR_XIR_SOURCE_CALL);
}
static bool source_math_call(SourceContext *ctx,AstNode *node,const SourceMathDeclaration *native,SourceValue *value) {
    XrXirMathSignature signature;
    if (!xr_xir_math_ir_signature(native->opcode,native->operation,&signature))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"math declaration projection is inconsistent");
    XrXirType parameters[2]={signature.parameter,signature.parameter};SourceValue values[2]={{0}};
    uint32_t supplied=0;
    if (!source_native_read_arguments(ctx,node,parameters,signature.arity,signature.arity,values,&supplied,
            "math primitive requires its governed fixed signature") ||
        !source_math_query(ctx,node->as.call_expr.callee,native,signature)) return false;
    XrXirInstruction operation={native->opcode,signature.result,{0},{0},native->operation,{0}};
    for (uint32_t i=0;i<supplied;++i) operation.args[i]=values[i].id;
    return source_recipe_record(ctx,operation,value);
}
