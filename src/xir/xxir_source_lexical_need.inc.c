/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_lexical_need.inc.c - Retain extents for potential callable owners
 */
typedef struct SourceLexicalNeed {
    SourceContext *context;
    uint8_t *seen;
    bool all;
} SourceLexicalNeed;
enum {
    SOURCE_LEXICAL_TYPE_VISITING=1,
    SOURCE_LEXICAL_TYPE_UNUSED,
    SOURCE_LEXICAL_TYPE_REQUIRED
};

/* Callable refinement preserves type shape. Constructors and generic bodies
 * retain every row before refinement or later concrete instantiation. */
static bool source_lexical_all_needed(SourceContext *ctx) {
    if (ctx->linkage_kind==XR_XIR_LIBRARY || ctx->interactive || ctx->history_input) return true;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (ctx->generics && ctx->generics[f].parameter_count) return true;
        const XrXirFunction *function=&ctx->functions[f];
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!(i%256) && !source_request_poll(ctx,NULL)) return true;
            XrXirOp op=function->instructions[i].op;
            if (op==XR_XIR_FUNCTION_REF || op==XR_XIR_FUNCTION_WEAKEN ||
                op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT) return true;
        }
    }
    return false;
}

/* Classify the admitted concrete type graph once. Recursive descriptions keep
 * extents. Unused callable
 * signatures in the pool do not create values. Opaque and unresolved values
 * keep their extents, including instantiated fields that remain unknown. */
static bool source_lexical_type_needed(SourceLexicalNeed *need,XrXirType type,uint32_t depth) {
    SourceContext *ctx=need->context;
    if ((uint32_t)type>=XR_XIR_TYPE_PARAMETER_BASE || type==XR_XIR_ERROR) return true;
    const XrXirTypeNode *node=xr_xir_type_node(&ctx->types,type);
    if (!node) return (uint32_t)type>=XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (node->kind==XR_XIR_TYPE_CALLABLE) return true;
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (need->seen[index]>=SOURCE_LEXICAL_TYPE_UNUSED)
        return need->seen[index]==SOURCE_LEXICAL_TYPE_REQUIRED;
    if (need->seen[index] || depth>=128 || !source_request_poll(ctx,NULL)) return true;
    need->seen[index]=SOURCE_LEXICAL_TYPE_VISITING;
    bool required=source_lexical_type_needed(need,node->element,depth+1) ||
        source_lexical_type_needed(need,node->result,depth+1);
    for (uint32_t p=0;!required && p<xr_xir_type_parameter_slots(node);++p)
        required=source_lexical_type_needed(need,node->parameters[p].type,depth+1);
    for (uint32_t a=0;!required && a<node->nominal.argument_count;++a)
        required=source_lexical_type_needed(need,node->nominal.arguments[a],depth+1);
    if (required || node->kind!=XR_XIR_TYPE_NOMINAL) {
        need->seen[index]=required ? SOURCE_LEXICAL_TYPE_REQUIRED : SOURCE_LEXICAL_TYPE_UNUSED;
        return required;
    }
    XrXirTypeDescription description={0};
    XrXirStatus status=xr_xir_compile_type_description(&ctx->compile,&ctx->types,type,&description);
    if (status!=XR_XIR_OK) {
        source_fail(ctx,NULL,status,"lexical value lost its nominal type description");
        return true;
    }
    for (uint32_t f=0;f<description.field_count;++f) {
        if (!(f%64) && !source_request_poll(ctx,NULL)) return true;
        XrXirFieldDescription field={0};
        status=xr_xir_compile_description_field(&ctx->compile,&description,f,&field);
        if (status!=XR_XIR_OK) {
            source_fail(ctx,NULL,status,"lexical value lost its actual field type");
            return true;
        }
        if (!field.resolved || source_lexical_type_needed(need,field.type,depth+1)) {
            need->seen[index]=SOURCE_LEXICAL_TYPE_REQUIRED;
            return true;
        }
    }
    need->seen[index]=SOURCE_LEXICAL_TYPE_UNUSED;
    return false;
}

static bool source_lexical_need_prepare(SourceContext *ctx,SourceLexicalNeed *need) {
    *need=(SourceLexicalNeed){.context=ctx,.all=source_lexical_all_needed(ctx)};
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (need->all) return true;
    need->seen=ctx->types.count ? source_alloc(ctx,ctx->types.count,sizeof(*need->seen)) : NULL;
    if (ctx->types.count && !need->seen) return false;
    for (uint32_t s=0;s<ctx->slot_count;++s) {
        if (!(s%64) && !source_request_poll(ctx,NULL)) return false;
        if (source_lexical_type_needed(need,ctx->slots[s].type,0)) { need->all=true;break; }
    }
    return ctx->diagnostic.status==XR_XIR_OK;
}

static bool source_lexical_function_needed(SourceLexicalNeed *need,uint32_t f) {
    if (need->all) return true;
    SourceContext *ctx=need->context;
    const XrXirFunction *function=&ctx->functions[f];
    if (source_lexical_type_needed(need,function->result,0)) return true;
    for (uint32_t p=0;p<function->parameter_count;++p) {
        if (!(p%64) && !source_request_poll(ctx,NULL)) return true;
        if (source_lexical_type_needed(need,function->parameters[p],0)) return true;
    }
    for (uint32_t i=0;i<function->instruction_count;++i) {
        if (!(i%256) && !source_request_poll(ctx,NULL)) return true;
        if (source_lexical_type_needed(need,function->instructions[i].type,0)) return true;
    }
    return false;
}
