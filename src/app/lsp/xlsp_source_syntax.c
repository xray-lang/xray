/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_syntax.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../base/xarena.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/parser/xast_walk.h"
#include <limits.h>
#include <string.h>
typedef struct XlspSyntaxArgumentRecord {
    uintptr_t address;
    uint32_t node_id;
    AstNodeType type;
    XrNameSpan start,end,variable,variable_end;
    bool has_variable;
} XlspSyntaxArgumentRecord;
typedef struct XlspSyntaxCallRecord {
    uintptr_t address;
    uint32_t node_id;
    AstNodeType type;
    XrNameSpan start,end,closing,selection,selection_end;
    XrNameSpan *boundaries;
    XlspSyntaxArgumentRecord *arguments;
    size_t boundary_count,argument_count;
    bool has_callee,live;
} XlspSyntaxCallRecord;
struct XlspSyntaxSnapshot {
    XrCompileState *state; /* Retained by the opened embedded arena. */
    XrArena arena;
    char *uri,*text;
    size_t uri_length,length;
    int64_t version;
    AstNode *ast;
    XlspSyntaxDiagnostic *diagnostics,*last;
    XrParseStatus status;
    XlspSyntaxCallRecord *calls;
    size_t call_count,call_capacity;
};
static XrCompileResourceStatus syntax_call_observe(void *,const XrParseCallSyntax *);
static XrCompileResourceStatus syntax_calls_finalize(XlspSyntaxSnapshot *);
static XrParseStatus syntax_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_PARSE_OK:
        status==XR_COMPILE_RESOURCE_BUDGET?XR_PARSE_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_PARSE_OUT_OF_MEMORY:XR_PARSE_BAD_ARGUMENT;
}
void xlsp_source_syntax_free(XlspSyntaxSnapshot *s) {
    if(!s)return;
    for(size_t i=0;i<s->call_count;++i) {
        xr_compile_state_free(s->calls[i].boundaries);xr_compile_state_free(s->calls[i].arguments);
    }
    xr_compile_state_free(s->calls);
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
    xr_parser_set_call_syntax_callback(&parser,syntax_call_observe,s);
    status=xr_compile_parse_recoverable(&parser,&s->ast);
    xr_compile_parser_close(&parser);
    if(status==XR_PARSE_OK||status==XR_PARSE_RECOVERED) {
        XrCompileResourceStatus finalized=syntax_calls_finalize(s);
        if(finalized!=XR_COMPILE_RESOURCE_OK)status=syntax_status(finalized);
    }
    if(status==XR_PARSE_OK||status==XR_PARSE_RECOVERED){s->status=status;*out=s;s=NULL;}
done:
    xlsp_source_syntax_free(s);return status;
}
const AstNode *xlsp_source_syntax_ast(const XlspSyntaxSnapshot *s){return s?s->ast:NULL;}
XrCompileState *xlsp_source_syntax_state(const XlspSyntaxSnapshot *s){return s?s->state:NULL;}
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
static XrJsonValue *syntax_position_json(XrLspPosition position) {
    XrJsonValue *out=xjson_new_object();if(!out)return NULL;
    if(!syntax_member(out,"line",xjson_new_number(position.line))||
        !syntax_member(out,"character",xjson_new_number(position.character))){xjson_free(out);return NULL;}
    return out;
}
XR_FUNC XrXirStatus xlsp_source_syntax_diagnostics_json(const XlspSyntaxSnapshot *s,XrJsonValue **out) {
    if(!s||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrJsonValue *array=xjson_new_array();if(!array)return XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=XR_XIR_OK;XrCompileResources *resources=xr_compile_state_resources(s->state);
    for(const XlspSyntaxDiagnostic *d=s->diagnostics;d;d=d->next) {
        status=syntax_work(s,sizeof(*d));if(status!=XR_XIR_OK)break;
        XrLspPosition start={0},end={0};
        status=xlsp_source_position(resources,s->text,s->length,d->line,d->column,&start);
        if(status==XR_XIR_OK)status=xlsp_source_position(resources,s->text,s->length,d->end_line,d->end_column,&end);
        if(status!=XR_XIR_OK)break;
        XrJsonValue *diag=xjson_new_object(),*range=xjson_new_object();
        if(!diag||!range){xjson_free(diag);xjson_free(range);status=XR_XIR_OUT_OF_MEMORY;break;}
        if(!syntax_member(range,"start",syntax_position_json(start))||!syntax_member(range,"end",syntax_position_json(end))) {
            xjson_free(range);xjson_free(diag);status=XR_XIR_OUT_OF_MEMORY;break;
        }
        if(!syntax_member(diag,"range",range)||!syntax_member(diag,"severity",xjson_new_number(1))||
            !syntax_member(diag,"source",xjson_new_string("xray"))||!syntax_member(diag,"message",xjson_new_string(d->message))) {
            xjson_free(diag);status=XR_XIR_OUT_OF_MEMORY;break;
        }
        int count=array->as.array.count;
        if(count==array->as.array.capacity&&array->as.array.capacity>INT_MAX/2){xjson_free(diag);status=XR_XIR_BUDGET;break;}
        xjson_array_push(array,diag);
        if(array->as.array.count!=count+1){xjson_free(diag);status=XR_XIR_OUT_OF_MEMORY;break;}
    }
    if(status==XR_XIR_OK){*out=array;array=NULL;}
    xjson_free(array);return status;
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
#include "xlsp_source_call_syntax.inc.c"
#include "xlsp_source_argument_syntax.inc.c"
