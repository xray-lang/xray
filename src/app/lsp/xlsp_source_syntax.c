/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_syntax.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../base/xarena.h"
#include "../../frontend/parser/xast_nodes.h"
#include <limits.h>
#include <string.h>
struct XlspSyntaxSnapshot {
    XrCompileState *state; /* Retained by the opened embedded arena. */
    XrArena arena;
    char *uri,*text;
    size_t uri_length,length;
    int64_t version;
    AstNode *ast;
    XlspSyntaxDiagnostic *diagnostics,*last;
    XrParseStatus status;
};
static XrParseStatus syntax_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_PARSE_OK:
        status==XR_COMPILE_RESOURCE_BUDGET?XR_PARSE_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_PARSE_OUT_OF_MEMORY:XR_PARSE_BAD_ARGUMENT;
}
void xlsp_source_syntax_free(XlspSyntaxSnapshot *s) {
    if(!s)return;
    XlspSyntaxDiagnostic *item=s->diagnostics;
    while(item){XlspSyntaxDiagnostic *next=(XlspSyntaxDiagnostic *)item->next;xr_compile_state_free((char *)item->message);xr_compile_state_free(item);item=next;}
    xr_arena_destroy(&s->arena);xr_compile_state_free(s->uri);xr_compile_state_free(s->text);xr_compile_state_free(s);
}
static void syntax_diagnostic(void *context,int line,int column,int end_line,int end_column,const char *message) {
    XlspSyntaxSnapshot *s=context;XlspSyntaxDiagnostic *item=NULL;
    if(xr_compile_state_calloc(s->state,1,sizeof(*item),(void **)&item)!=XR_COMPILE_RESOURCE_OK)return;
    char *copy=NULL;
    if(xr_compile_state_strdup(s->state,message,&copy)!=XR_COMPILE_RESOURCE_OK){xr_compile_state_free(item);return;}
    if(xr_compile_state_work(s->state,sizeof(*item))!=XR_COMPILE_RESOURCE_OK){xr_compile_state_free(copy);xr_compile_state_free(item);return;}
    *item=(XlspSyntaxDiagnostic){line,column,end_line,end_column,copy,NULL};
    if(s->last)s->last->next=item;else s->diagnostics=item;
    s->last=item;
}
XrParseStatus xlsp_source_syntax_build(XrCompilerSession *session,const XlspSourceDocument *document,XlspSyntaxSnapshot **out) {
    if(!session||!document||!document->uri||!document->text||!out||*out||
        document->uri_length==SIZE_MAX||document->length==SIZE_MAX)return XR_PARSE_BAD_ARGUMENT;
    XrCompileState *state=xr_compile_session_compile_state(session);XlspSyntaxSnapshot *s=NULL;
    XrParseStatus status=syntax_status(xr_compile_state_calloc(state,1,sizeof(*s),(void **)&s));
    if(status!=XR_PARSE_OK)return status;
    s->state=state;s->uri_length=document->uri_length;s->length=document->length;s->version=document->version;
    if(xr_compile_state_work(state,document->uri_length)!=XR_COMPILE_RESOURCE_OK||
        xr_compile_state_work(state,document->length)!=XR_COMPILE_RESOURCE_OK){status=syntax_status(xr_compile_state_status(state));goto done;}
    if(memchr(document->uri,0,document->uri_length)||memchr(document->text,0,document->length)){status=XR_PARSE_BAD_ARGUMENT;goto done;}
    char *path=NULL;
    XrXirStatus uri_status=xlsp_source_uri_path(xr_compile_state_resources(state),document->uri,document->uri_length,&path);
    xr_compile_resources_free(path);
    if(uri_status!=XR_XIR_OK) {
        status=uri_status==XR_XIR_BUDGET?XR_PARSE_BUDGET:uri_status==XR_XIR_OUT_OF_MEMORY?XR_PARSE_OUT_OF_MEMORY:XR_PARSE_BAD_ARGUMENT;
        if(status==XR_PARSE_BUDGET)(void)xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BUDGET);
        else if(status==XR_PARSE_OUT_OF_MEMORY)(void)xr_compile_state_fail(state,XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        goto done;
    }
    status=syntax_status(xr_compile_state_strndup(state,document->uri,document->uri_length,&s->uri));
    if(status!=XR_PARSE_OK)goto done;
    status=syntax_status(xr_compile_state_strndup(state,document->text,document->length,&s->text));
    if(status!=XR_PARSE_OK)goto done;
    XrArenaBacking backing;
    if(xr_compiler_arena_state_backing(state,&backing)!=XR_ARENA_OK){status=XR_PARSE_BAD_ARGUMENT;goto done;}
    (void)xr_arena_open(&s->arena,XR_ARENA_SEGMENT_SIZE,&backing);
    status=syntax_status(xr_compiler_arena_capture_status(&s->arena,state));
    if(status!=XR_PARSE_OK)goto done;
    Parser parser={0};status=xr_compile_parser_open(&parser,session,s->text,s->uri,&s->arena);
    if(status!=XR_PARSE_OK)goto done;
    xr_parser_set_error_callback(&parser,syntax_diagnostic,s,100);
    status=xr_compile_parse_recoverable(&parser,&s->ast);
    xr_compile_parser_close(&parser);
    if(status==XR_PARSE_OK||status==XR_PARSE_RECOVERED){s->status=status;*out=s;s=NULL;}
done:
    xlsp_source_syntax_free(s);return status;
}
const AstNode *xlsp_source_syntax_ast(const XlspSyntaxSnapshot *s){return s?s->ast:NULL;}
const XlspSyntaxDiagnostic *xlsp_source_syntax_diagnostics(const XlspSyntaxSnapshot *s){return s?s->diagnostics:NULL;}
XrParseStatus xlsp_source_syntax_status(const XlspSyntaxSnapshot *s){return s?s->status:XR_PARSE_BAD_ARGUMENT;}
static XrXirStatus syntax_work(const XlspSyntaxSnapshot *s,size_t amount) {
    XrCompileResourceStatus paid=xr_compile_state_work(s->state,amount);
    return paid==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:paid==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        paid==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static bool syntax_member(XrJsonValue *object,const char *key,XrJsonValue *child) {
    if(!child)return false;
    int before=object->as.object.count;xjson_object_set_new(object,key,child);
    if(object->as.object.count==before){xjson_free(child);return false;}
    if(object->as.object.count!=before+1)return false;
    XrJsonMember *member=&object->as.object.members[before];
    return member->key&&member->value==child&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static XrXirStatus syntax_push(const XlspSyntaxSnapshot *s,const AstNode ***stack,size_t *count,size_t *capacity,const AstNode *node) {
    if(!node)return XR_XIR_OK;
    XrXirStatus status=syntax_work(s,1);if(status!=XR_XIR_OK)return status;
    if(*count==*capacity) {
        if(*capacity>SIZE_MAX/2/sizeof(**stack))return XR_XIR_BUDGET;
        size_t next=*capacity?*capacity*2:16;
        XrCompileResourceStatus grown=xr_compile_state_resize(s->state,(void **)stack,next*sizeof(**stack));
        if(grown!=XR_COMPILE_RESOURCE_OK)return grown==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
            grown==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
        *capacity=next;
    }
    (*stack)[(*count)++]=node;return XR_XIR_OK;
}
XrXirStatus xlsp_source_syntax_folding_json(const XlspSyntaxSnapshot *s,XrJsonValue **out) {
    if(!s||!out||*out)return XR_XIR_BAD_STRUCTURE;
    const AstNode **stack=NULL;size_t count=0,capacity=0;XrJsonValue *result=xjson_new_array();
    if(!result)return XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=syntax_push(s,&stack,&count,&capacity,s->ast);
    while(count&&status==XR_XIR_OK) {
        status=syntax_work(s,sizeof(AstNode));if(status!=XR_XIR_OK)break;
        const AstNode *node=stack[--count];bool fold=false;
#define PUSH(n) do {if(status==XR_XIR_OK)status=syntax_push(s,&stack,&count,&capacity,(n));} while(0)
        switch(node->type) {
        case AST_PROGRAM:
            for(int i=node->as.program.count;i>0&&status==XR_XIR_OK;--i)PUSH(node->as.program.statements[i-1]);break;
        case AST_FUNCTION_DECL:fold=node->as.function_decl.body!=NULL;PUSH(node->as.function_decl.body);break;
        case AST_CLASS_DECL:case AST_STRUCT_DECL:case AST_UNION_DECL:
            fold=true;for(int i=node->as.class_decl.method_count;i>0&&status==XR_XIR_OK;--i)PUSH(node->as.class_decl.methods[i-1]);break;
        case AST_BLOCK:
            for(int i=node->as.block.count;i>0&&status==XR_XIR_OK;--i)PUSH(node->as.block.statements[i-1]);break;
        case AST_IF_STMT:fold=true;PUSH(node->as.if_stmt.else_branch);PUSH(node->as.if_stmt.then_branch);break;
        case AST_WHILE_STMT:fold=true;PUSH(node->as.while_stmt.body);break;
        case AST_FOR_STMT:fold=true;PUSH(node->as.for_stmt.body);break;
        case AST_TRY_CATCH:
            fold=true;for(int i=node->as.try_catch.catch_count;i>0&&status==XR_XIR_OK;--i) {
                XrCatchClause *clause=node->as.try_catch.catch_clauses[i-1];if(clause)PUSH(clause->body);
            }PUSH(node->as.try_catch.try_body);break;
        case AST_MATCH_EXPR:
            fold=true;for(int i=node->as.match_expr.arm_count;i>0&&status==XR_XIR_OK;--i)PUSH(node->as.match_expr.arms[i-1]);break;
        default:break;
        }
#undef PUSH
        if(status!=XR_XIR_OK||!fold||node->end_line<=0)continue;
        XrLspPosition first={0},last={0};XrCompileResources *resources=xr_compile_state_resources(s->state);
        status=xlsp_source_position(resources,s->text,s->length,node->line,1,&first);
        if(status==XR_XIR_OK)status=xlsp_source_position(resources,s->text,s->length,node->end_line,node->end_column,&last);
        if(status!=XR_XIR_OK||last.line<=first.line)continue;
        XrJsonValue *range=xjson_new_object();
        if(!range){status=XR_XIR_OUT_OF_MEMORY;break;}
        if(!syntax_member(range,"startLine",xjson_new_number(first.line))||
            !syntax_member(range,"endLine",xjson_new_number(last.line))||
            !syntax_member(range,"kind",xjson_new_string("region"))) {xjson_free(range);status=XR_XIR_OUT_OF_MEMORY;break;}
        if(result->as.array.count==result->as.array.capacity&&result->as.array.capacity>INT_MAX/2) {xjson_free(range);status=XR_XIR_BUDGET;break;}
        int previous=result->as.array.count;xjson_array_push(result,range);
        if(result->as.array.count!=previous+1){xjson_free(range);status=XR_XIR_OUT_OF_MEMORY;break;}
    }
    xr_compile_state_free(stack);
    if(status==XR_XIR_OK){*out=result;result=NULL;}
    xjson_free(result);return status;
}

#include "xlsp_source_outline_impl.h"
