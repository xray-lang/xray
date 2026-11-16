/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Eligibility is checked syntax; const aliases copy the already bound fact. */
#include "shared/xr_literal_quote_core.h"
#include "shared/xerror_codes.h"
#include <inttypes.h>
/* This text may be built after a semantic failure. It owns one temporary
 * compiler allocation; the diagnostic receiver copies it before release. */
static char *source_map_bare_key_hint(SourceContext *ctx,const AstNode *key,XrXirStatus *status) {
    const char prefix[]="if you intended a string key, write \"";
    size_t length=strlen(key->as.variable.name);
    if(length>SIZE_MAX-sizeof(prefix)-1){*status=XR_XIR_OVERFLOW;return NULL;}
    char *hint=xir_compile_alloc(&ctx->compile,length+sizeof(prefix)+1,status);
    if(!hint)return NULL;
    memcpy(hint,prefix,sizeof(prefix)-1);
    memcpy(hint+sizeof(prefix)-1,key->as.variable.name,length);
    hint[sizeof(prefix)-1+length]='"';hint[sizeof(prefix)+length]=0;
    return hint;
}
static bool source_collection_key_copy(SourceContext *ctx,XrXirConstantKey input,XrXirConstantKey *output) {
    XrXirConstantKey key=input;
    if(key.kind==XR_XIR_KEY_STRING) {
        char *bytes=key.string.length?source_alloc(ctx,key.string.length,1):NULL;
        if(key.string.length && (!bytes || !source_copy_bytes(ctx,NULL,bytes,key.string.bytes,key.string.length)))return false;
        key.string.bytes=bytes;
    }
    *output=key;return true;
}
static bool source_collection_key(SourceContext *ctx,AstNode *node,SourceValue value,XrXirConstantKey *output) {
    *output=(XrXirConstantKey){0};
    while(node && node->type==AST_GROUPING)node=node->as.grouping;
    if(!node)return true;
    SourceName *binding=NULL;
    if(node->type==AST_VARIABLE) {
        binding=visible_name(ctx,node->as.variable.name);
        if(binding && binding->kind==SOURCE_IMPORT)binding=imported_declaration(ctx,binding,binding->imported);
    } else if(node->type==AST_MEMBER_ACCESS && node->as.member_access.object->type==AST_VARIABLE) {
        SourceName *owner=visible_name(ctx,node->as.member_access.object->as.variable.name);
        if(owner && owner->kind==SOURCE_MODULE)binding=imported_declaration(ctx,owner,node->as.member_access.name);
    }
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    if(binding && !binding->mutable && source_name_ready(binding) && binding->type==value.type) {
        *output=binding->constant_key;return true;
    }
    bool comptime=node->type==AST_COMPTIME_EXPR;
    bool scalar=node->type==AST_LITERAL_INT || node->type==AST_LITERAL_RUNE || node->type==AST_LITERAL_TRUE || node->type==AST_LITERAL_FALSE;
    if(node->type==AST_UNARY_NEG) {
        SourceInteger integer={0};if(!source_direct_integer(ctx,node,&integer))return false;
        scalar=integer.present;
    }
    bool text=node->type==AST_LITERAL_STRING;
    bool variant=node->type==AST_MEMBER_ACCESS || node->type==AST_NEW_EXPR || node->type==AST_CALL_EXPR;
    if(!comptime && !scalar && !text && !variant)return true;
    uint32_t parameters=ctx->functions[ctx->function].parameter_count;
    SourceFunction *body=&ctx->bodies[ctx->function];
    if(value.id<parameters || value.id-parameters>=body->count)return true;
    XrXirInstruction instruction=body->recipes[value.id-parameters].instruction;
    if((text || comptime) && instruction.op==XR_XIR_CONST_STRING && value.type==XR_XIR_STRING) {
        if(instruction.immediate<0 || (uint64_t)instruction.immediate>=ctx->literal_count)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"constant key has no literal owner");
        *output=(XrXirConstantKey){.kind=XR_XIR_KEY_STRING,.string=ctx->literals[instruction.immediate]};
    } else if((scalar || comptime) && (instruction.op==XR_XIR_CONST_INT || instruction.op==XR_XIR_CONST_BOOL ||
        instruction.op==XR_XIR_CONST_RUNE || (comptime && instruction.op==XR_XIR_CONST_FLOAT)))
        *output=(XrXirConstantKey){.kind=XR_XIR_KEY_SCALAR,.bits=(uint64_t)instruction.immediate};
    else if((variant || comptime) && instruction.op==XR_XIR_ENUM_NEW && !instruction.args[1])
        *output=(XrXirConstantKey){.kind=XR_XIR_KEY_ENUM,.bits=(uint64_t)instruction.immediate};
    return true;
}
typedef struct SourceCollectionText {char *bytes;size_t used,capacity;} SourceCollectionText;
static int source_collection_write(void *opaque,const void *bytes,size_t count) {
    SourceCollectionText *text=opaque;
    if(count>SIZE_MAX-text->used)return 0;
    if(text->bytes) {
        if(text->used>text->capacity || count>text->capacity-text->used)return 0;
        if(count)memcpy(text->bytes+text->used,bytes,count);
    }
    text->used+=count;return 1;
}
static bool source_collection_key_text(SourceContext *ctx,XrValueFormatSink sink,const XrHirCollectionEntry *entry) {
    XrXirConstantKey key=entry->constant;
    if(key.kind==XR_XIR_KEY_STRING)return xr_literal_quote_bytes(sink,key.string.bytes,key.string.length)!=0;
    if(key.kind==XR_XIR_KEY_ENUM) {
        const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,entry->key);
        if(!type || type->kind!=XR_XIR_TYPE_NOMINAL || type->nominal.declaration>=ctx->nominals.count)return false;
        XrXirNominalDeclaration declaration=ctx->nominals.declarations[type->nominal.declaration];
        if(key.bits>=declaration.variant_count)return false;
        XrXirLiteral name=declaration.variants[key.bits].name;
        return xr_value_format_write(sink,declaration.name.bytes,declaration.name.length) &&
            xr_value_format_text(sink,".") && xr_value_format_write(sink,name.bytes,name.length);
    }
    char buffer[96];int count;
    if(entry->key==XR_XIR_BOOL)return xr_value_format_text(sink,key.bits?"true":"false")!=0;
    if(entry->key==XR_XIR_RUNE)count=snprintf(buffer,sizeof(buffer),"U+%04" PRIX64,key.bits);
    else if(entry->key==XR_XIR_U64)count=snprintf(buffer,sizeof(buffer),"%" PRIu64,key.bits);
    else if(xr_xir_type_is_integer(entry->key))count=snprintf(buffer,sizeof(buffer),"%" PRId64,(int64_t)key.bits);
    else {
        double value;
        if(entry->key==XR_XIR_F32){uint32_t bits=(uint32_t)key.bits;float number;memcpy(&number,&bits,sizeof(number));value=number;}
        else memcpy(&value,&key.bits,sizeof(value));
        count=snprintf(buffer,sizeof(buffer),"%.17g",value);
    }
    return count>=0 && (size_t)count<sizeof(buffer) && xr_value_format_write(sink,buffer,(size_t)count);
}
static bool source_collection_duplicate(SourceContext *ctx,AstNode *node,const XrHirCollectionEntry *entries,XrHirCollectionError error) {
    MapLiteralNode *map=&node->as.map_literal;
    if(error.first>=(uint32_t)map->count || error.second>=(uint32_t)map->count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"duplicate key has no source item");
    SourceCollectionText text={0};XrValueFormatSink sink={&text,source_collection_write};
    for(unsigned pass=0;pass<2;++pass) {
        if(!xr_value_format_text(sink,"E0325: duplicate key ") || !source_collection_key_text(ctx,sink,&entries[error.second]) ||
            !xr_value_format_text(sink," in Map literal"))return source_fail(ctx,node,XR_XIR_BUDGET,"duplicate key diagnostic exceeds format limit");
        if(!pass) {
            if(text.used==SIZE_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"duplicate key diagnostic exceeds format limit");
            text.capacity=text.used;text.bytes=source_alloc(ctx,text.used+1,1);if(!text.bytes)return false;text.used=0;
        }
    }
    XrNameSpan first=map->entry_spans?map->entry_spans[error.first].key:(XrNameSpan){map->keys[error.first]->line,map->keys[error.first]->column};
    XrNameSpan second=map->entry_spans?map->entry_spans[error.second].key:(XrNameSpan){map->keys[error.second]->line,map->keys[error.second]->column};
    const XrModuleSpec *source=&ctx->graph->specs[ctx->module];
    XrFrontendReport *report=NULL;uint32_t id=0;
    XrCompileResourceStatus status=xr_frontend_report_new(ctx->compile.resources,&report);
    if(status==XR_COMPILE_RESOURCE_OK)status=xr_frontend_report_source(report,source->source_path,source->source_text,source->source_length,NULL,&id);
    if(status==XR_COMPILE_RESOURCE_OK) {
        XrFrontendLocation primary=xr_frontend_report_location(report,id,second.line,second.column,NULL);
        XrFrontendLabel label={"the key first appears here",xr_frontend_report_location(report,id,first.line,first.column,NULL)};
        status=xr_frontend_report_add(report,XR_ERR_MAP_LITERAL_DUPLICATE_KEY,text.bytes,primary,&label,1);
    }
    if(status!=XR_COMPILE_RESOURCE_OK) {
        xr_frontend_report_free(report);return source_fail(ctx,node,xir_compile_resource_status(status),"duplicate key diagnostic allocation failed");
    }
    xr_frontend_report_seal(report,false);xr_frontend_report_free(ctx->report);ctx->report=report;
    source_fail(ctx,map->keys[error.second],XR_XIR_BAD_TYPE,text.bytes);
    ctx->diagnostic.line=second.line;ctx->diagnostic.column=second.column;return false;
}
