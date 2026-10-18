/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_index.h"
#include "xlsp_source_buffer.h"
#include "../../frontend/parser/xast_nodes.h"
#include <limits.h>
#include <string.h>
struct XlspSourceIndex {
    XrCompileResources *resources;
    XlspSourceIndexSymbol *items;
    char *uri;
    size_t count,capacity;
    int64_t version;
    XrParseStatus parse_status;
};
typedef struct IndexFrame {const AstNode *node;bool allow_export;} IndexFrame;
static XrXirStatus index_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus index_work(XlspSourceIndex *index,size_t bytes) {
    return index_resource(xr_compile_resources_work(index->resources,bytes));
}
static XrXirStatus index_copy(XlspSourceIndex *index,const char *text,size_t length,char **out) {
    if(!text||length==SIZE_MAX)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=index_work(index,length+1);if(status!=XR_XIR_OK)return status;
    char *copy=NULL;status=index_resource(xr_compile_resources_alloc(index->resources,length+1,(void **)&copy));
    if(status==XR_XIR_OK){memcpy(copy,text,length);copy[length]=0;*out=copy;}return status;
}
XR_FUNC void xlsp_source_index_free(XlspSourceIndex *index) {
    if(!index)return;
    for(size_t i=0;i<index->count;++i)xr_compile_resources_free((char *)index->items[i].name);
    xr_compile_resources_free(index->items);xr_compile_resources_free(index->uri);xr_compile_resources_free(index);
}
XR_FUNC const XlspSourceIndexSymbol *xlsp_source_index_symbols(const XlspSourceIndex *index,size_t *count) {
    if(count)*count=index?index->count:0;return index?index->items:NULL;
}
XR_FUNC const char *xlsp_source_index_uri(const XlspSourceIndex *index){return index?index->uri:NULL;}
XR_FUNC int64_t xlsp_source_index_version(const XlspSourceIndex *index){return index?index->version:0;}
XR_FUNC XrParseStatus xlsp_source_index_parse_status(const XlspSourceIndex *index){return index?index->parse_status:XR_PARSE_BAD_ARGUMENT;}
static XrXirStatus index_push(XlspSourceIndex *index,IndexFrame **stack,size_t *count,size_t *capacity,const AstNode *node,bool exported) {
    if(!node)return XR_XIR_OK;
    XrXirStatus status=index_work(index,sizeof(IndexFrame));if(status!=XR_XIR_OK)return status;
    if(*count==*capacity) {
        size_t next=*capacity?*capacity*2:16;
        if(next<*capacity||next>SIZE_MAX/sizeof(**stack))return XR_XIR_BUDGET;
        status=index_resource(xr_compile_resources_resize(index->resources,(void **)stack,next*sizeof(**stack)));
        if(status!=XR_XIR_OK)return status;*capacity=next;
    }
    (*stack)[(*count)++]=(IndexFrame){node,exported};return XR_XIR_OK;
}
static const char *index_name(const AstNode *node,XrLspSymbolKind *kind) {
    switch(node->type) {
    case AST_VAR_DECL:case AST_CONST_DECL:*kind=node->type==AST_CONST_DECL?XR_SYMBOL_CONSTANT:XR_SYMBOL_VARIABLE;return node->as.var_decl.name;
    case AST_FUNCTION_DECL:*kind=XR_SYMBOL_FUNCTION;return node->as.function_decl.name;
    case AST_CLASS_DECL:case AST_STRUCT_DECL:case AST_UNION_DECL:*kind=XR_SYMBOL_CLASS;return node->as.class_decl.name;
    case AST_METHOD_DECL:*kind=node->as.method_decl.is_constructor?XR_SYMBOL_CONSTRUCTOR:XR_SYMBOL_METHOD;return node->as.method_decl.name;
    case AST_FIELD_DECL:*kind=XR_SYMBOL_FIELD;return node->as.field_decl.name;
    case AST_ENUM_DECL:*kind=XR_SYMBOL_ENUM;return node->as.enum_decl.name;
    case AST_ENUM_MEMBER:*kind=XR_SYMBOL_ENUM_MEMBER;return node->as.enum_member.name;
    case AST_INTERFACE_DECL:*kind=XR_SYMBOL_INTERFACE;return node->as.interface_decl.name;
    case AST_TYPE_ALIAS:*kind=XR_SYMBOL_TYPE_PARAMETER;return node->as.type_alias.name;
    default:return NULL;
    }
}
static XrXirStatus index_append(XlspSourceIndex *index,const XlspSourceDocument *document,IndexFrame frame) {
    XrLspSymbolKind kind=0;const char *name=index_name(frame.node,&kind);if(!name)return XR_XIR_OK;
    size_t length=0;
    for(;;) {
        XrXirStatus paid=index_work(index,1);if(paid!=XR_XIR_OK)return paid;
        if(!name[length])break;if(length==SIZE_MAX-1)return XR_XIR_BUDGET;++length;
    }
    if(!length||frame.node->line<=0||frame.node->column<=0||length>(size_t)(INT_MAX-frame.node->column))return XR_XIR_BAD_STRUCTURE;
    XlspSourceIndexSymbol symbol={0};symbol.kind=kind;symbol.exported=frame.allow_export&&frame.node->is_exported;
    XrXirStatus status=xlsp_source_position(index->resources,document->text,document->length,frame.node->line,frame.node->column,&symbol.selection.start);
    if(status==XR_XIR_OK)status=xlsp_source_position(index->resources,document->text,document->length,frame.node->line,frame.node->column+(int)length,&symbol.selection.end);
    if(status!=XR_XIR_OK)return status;
    if(index->count>=INT_MAX)return XR_XIR_BUDGET;
    if(index->count==index->capacity) {
        size_t next=index->capacity?index->capacity*2:16;if(next<index->capacity||next>SIZE_MAX/sizeof(*index->items))return XR_XIR_BUDGET;
        status=index_resource(xr_compile_resources_resize(index->resources,(void **)&index->items,next*sizeof(*index->items)));
        if(status!=XR_XIR_OK)return status;index->capacity=next;
    }
    char *copy=NULL;status=index_copy(index,name,length,&copy);if(status!=XR_XIR_OK)return status;
    status=index_work(index,sizeof(symbol));if(status!=XR_XIR_OK){xr_compile_resources_free(copy);return status;}
    symbol.name=copy;index->items[index->count++]=symbol;return XR_XIR_OK;
}
static XrXirStatus index_children(XlspSourceIndex *index,IndexFrame frame,IndexFrame **stack,size_t *count,size_t *capacity) {
    const AstNode *node=frame.node;XrXirStatus status=XR_XIR_OK;
#define INDEX_PUSH(n,e) do {if(status==XR_XIR_OK)status=index_push(index,stack,count,capacity,(n),(e));} while(0)
#define INDEX_ARRAY(n,a,e) do {int total=(n);if(total<0||(total&&!(a)))return XR_XIR_BAD_STRUCTURE;for(int i=total;i>0&&status==XR_XIR_OK;--i)INDEX_PUSH((a)[i-1],(e));} while(0)
    switch(node->type) {
    case AST_PROGRAM:INDEX_ARRAY(node->as.program.count,node->as.program.statements,true);break;
    case AST_BLOCK:INDEX_ARRAY(node->as.block.count,node->as.block.statements,false);break;
    case AST_FUNCTION_DECL:INDEX_PUSH(node->as.function_decl.body,false);break;
    case AST_CLASS_DECL:case AST_STRUCT_DECL:case AST_UNION_DECL:
        INDEX_ARRAY(node->as.class_decl.method_count,node->as.class_decl.methods,false);
        INDEX_ARRAY(node->as.class_decl.field_count,node->as.class_decl.fields,false);break;
    case AST_ENUM_DECL:INDEX_ARRAY(node->as.enum_decl.member_count,node->as.enum_decl.members,false);break;
    default:break;
    }
#undef INDEX_ARRAY
#undef INDEX_PUSH
    return status;
}
static XrXirStatus index_project(XlspSourceIndex *index,const XlspSourceDocument *document,const AstNode *ast) {
    IndexFrame *stack=NULL;size_t count=0,capacity=0;
    XrXirStatus status=index_push(index,&stack,&count,&capacity,ast,false);
    while(count&&status==XR_XIR_OK) {
        status=index_work(index,sizeof(AstNode));if(status!=XR_XIR_OK)break;
        IndexFrame frame=stack[--count];status=index_append(index,document,frame);
        if(status==XR_XIR_OK)status=index_children(index,frame,&stack,&count,&capacity);
    }
    xr_compile_resources_free(stack);return status;
}
XR_FUNC XrXirStatus xlsp_source_index_build(XrCompileResources *resources,const XlspSourceDocument *document,XlspSourceIndex **out) {
    if(!resources||!document||!out||*out||!document->uri||document->uri_length==SIZE_MAX||document->version<INT32_MIN||document->version>INT32_MAX)return XR_XIR_BAD_STRUCTURE;
    XlspSourceIndex *index=NULL;XlspSyntaxSnapshot *syntax=NULL;XlspSourceBuffer *buffer=NULL;XrCompilerSession *session=NULL;
    XrXirStatus status=index_resource(xr_compile_resources_calloc(resources,1,sizeof(*index),(void **)&index));if(status!=XR_XIR_OK)return status;
    index->resources=resources;index->version=document->version;
    status=xlsp_source_buffer_new(resources,document->text,document->length,(int)document->version,&buffer);if(status!=XR_XIR_OK)goto done;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    XlspSourceDocument captured=*document;captured.text=xlsp_source_buffer_text(buffer);
    index->parse_status=xlsp_source_syntax_build(session,&captured,&syntax);
    if(index->parse_status!=XR_PARSE_OK&&index->parse_status!=XR_PARSE_RECOVERED) {
        status=index->parse_status==XR_PARSE_BUDGET?XR_XIR_BUDGET:index->parse_status==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;
    }
    status=index_copy(index,document->uri,document->uri_length,&index->uri);
    if(status==XR_XIR_OK)status=index_project(index,&captured,xlsp_source_syntax_ast(syntax));
    if(status==XR_XIR_OK){*out=index;index=NULL;}
done:
    xlsp_source_syntax_free(syntax);xlsp_source_buffer_free(buffer);xr_compile_session_free(session);xlsp_source_index_free(index);return status;
}
