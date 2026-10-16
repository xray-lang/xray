/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression.c - Pure Source expressions without a debugger interpreter
 */
#include "xdap_expression.h"
#include "xdap_json.h"
#include "xdap_value.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast_walk.h"
#include "../../shared/xnative_declaration.h"
#include <string.h>
typedef struct ExpressionBinding { const char *name;size_t length;XrXirValue value; } ExpressionBinding;
typedef struct ExpressionState {
    const XdapExpressionRequest *request;
    ExpressionBinding bindings[128];
    uint32_t count,depth,intrinsic_calls;
    XrXirStatus status;
    bool runtime;
} ExpressionState;
static XrXirStatus expression_value_status(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK ? XR_XIR_OK : status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
}
static bool expression_binding(ExpressionState *state,const char *name) {
    const XdapLexicalView *view=state->request->lexical;size_t length=strlen(name);
    for (uint32_t i=0;i<state->count;++i) {
        if (xr_compile_resources_work(view->launch->resources,length+1)!=XR_COMPILE_RESOURCE_OK) { state->status=XR_XIR_BUDGET;return false; }
        if (state->bindings[i].length==length && !memcmp(state->bindings[i].name,name,length)) return true;
    }
    for (uint32_t b=view->environment;b;b=view->function->bindings[b-1].parent) {
        if (xr_compile_resources_work(view->launch->resources,length+1)!=XR_COMPILE_RESOURCE_OK) { state->status=XR_XIR_BUDGET;return false; }
        const XrXirSourceDebugBinding *binding=&view->function->bindings[b-1];
        const char *candidate=view->source->declarations[binding->declaration-1].name;
        if (strcmp(candidate,name)) continue;
        if (state->count==128) { state->status=XR_XIR_BUDGET;return false; }
        ExpressionBinding fresh={0};
        state->status=expression_value_status(xdap_lexical_read(view,b,&fresh.value,&fresh.name,&fresh.length));
        if (state->status!=XR_XIR_OK) return false;
        if (fresh.value.type!=XR_XIR_BOOL && !xr_xir_type_is_integer((XrXirType)fresh.value.type)) state->runtime=true;
        state->bindings[state->count++]=fresh;return true;
    }
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(view->launch->source);
    if (!debug) { state->status=XR_XIR_BAD_STRUCTURE;return false; }
    for (uint32_t g=0;g<debug->global_count;++g) {
        ExpressionBinding fresh={0};XrXirValueStatus read=xdap_global_read(view,g,&fresh.value,&fresh.name,&fresh.length);
        if (read==XR_XIR_VALUE_BAD_ARGUMENT || read==XR_XIR_VALUE_UNSUPPORTED) continue;
        state->status=expression_value_status(read);if (state->status!=XR_XIR_OK) return false;
        if (fresh.length==length && !memcmp(fresh.name,name,length)) {
            if (state->count==128) { xr_xir_value_drop(&fresh.value);state->status=XR_XIR_BUDGET;return false; }
            if (fresh.value.type!=XR_XIR_BOOL && !xr_xir_type_is_integer((XrXirType)fresh.value.type)) state->runtime=true;
            state->bindings[state->count++]=fresh;return true;
        }
        xr_xir_value_drop(&fresh.value);
    }
    state->status=XR_XIR_UNRESOLVED;return false;
}
static bool expression_native_work(void *context,uint64_t units) {
    return xr_compile_resources_work(context,units)==XR_COMPILE_RESOURCE_OK;
}
#include "xdap_expression_intrinsic.inc.c"
/* This is syntax admission only. Source still resolves the actual receiver
 * and proves its declared operation; no callable value or provider is used. */
static bool expression_call_allowed(const AstNode *node,XrCompileResources *resources,XrXirStatus *status,XrCoreBuiltinId *intrinsic) {
    const CallExprNode *call=&node->as.call_expr;
    *intrinsic=XR_CORE_BUILTIN_NONE;
    if (!call->callee || call->type_arg_count || call->default_arg_count || call->arg_count<1 || call->arg_count>2 || !call->arguments) {
        *status=XR_XIR_BAD_TYPE;return false;
    }
    if (call->callee->type==AST_VARIABLE) {
        const char *name=call->callee->as.variable.name;const XrCoreIntrinsicDesc *descriptor=NULL;
        if (!name) { *status=XR_XIR_BAD_STRUCTURE;return false; }
        XrCoreIntrinsicQueryStatus query=xr_core_intrinsic_by_source_name_work(resources,expression_native_work,name,strlen(name),&descriptor);
        if (query!=XR_CORE_INTRINSIC_QUERY_OK) { *status=query==XR_CORE_INTRINSIC_QUERY_WORK_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;return false; }
        if (!descriptor || descriptor->id!=XR_CORE_BUILTIN_LEN || !xr_core_intrinsic_descriptor_validate(descriptor) ||
            descriptor->effect!=XR_CORE_INTRINSIC_EFFECT_READS_VALUE || call->arg_count!=1) {
            *status=XR_XIR_BAD_TYPE;return false;
        }
        *intrinsic=descriptor->id;
    } else {
        if (call->callee->type!=AST_MEMBER_ACCESS || !call->callee->as.member_access.name) { *status=XR_XIR_BAD_TYPE;return false; }
        const XrNativeTypeDeclaration *type=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
        XrNativeDeclarationWork work={resources,expression_native_work};const XrNativeMemberDeclaration *member=NULL;
        XrNativeDeclarationStatus found=xr_native_declaration_find_member(&work,type,call->callee->as.member_access.name,&member);
        if (found!=XR_NATIVE_DECLARATION_OK) {
            *status=found==XR_NATIVE_DECLARATION_WORK_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;return false;
        }
        if (!member->is_public || !member->is_method || member->is_static || member->receiver!=XR_NATIVE_RECEIVER_READ ||
            member->operation<XR_NATIVE_OPERATION_STRING_CONTAINS || member->operation>XR_NATIVE_OPERATION_STRING_LAST_INDEX_OF) {
            *status=XR_XIR_BAD_TYPE;return false;
        }
    }
    for (int i=0;i<call->arg_count;++i) {
        if (!expression_native_work(resources,1)) { *status=XR_XIR_BUDGET;return false; }
        if (call->arg_accesses && call->arg_accesses[i]!=XR_CALL_ARG_PLAIN) { *status=XR_XIR_BAD_TYPE;return false; }
    }
    return true;
}
static bool expression_node_allowed(const AstNode *node) {
    return node && (node->type==AST_VARIABLE || node->type==AST_LITERAL_INT || node->type==AST_LITERAL_TRUE || node->type==AST_LITERAL_FALSE ||
        node->type==AST_LITERAL_FLOAT || node->type==AST_LITERAL_RUNE || node->type==AST_LITERAL_STRING || node->type==AST_LITERAL_NULL ||
        node->type==AST_ARRAY_LITERAL || node->type==AST_TUPLE_LITERAL || node->type==AST_INDEX_GET || node->type==AST_MEMBER_ACCESS ||
        node->type==AST_CALL_EXPR || node->type==AST_GROUPING || node->type==AST_TERNARY || node->type==AST_FORCE_UNWRAP || node->type==AST_NULLISH_COALESCE ||
        (node->type>=AST_BINARY_ADD && node->type<=AST_BINARY_OR) ||
        (node->type>=AST_UNARY_NEG && node->type<=AST_UNARY_BNOT));
}
static bool expression_visit(AstNode *node,void *context) {
    ExpressionState *state=context;
    if (!node) return true;
    if (state->depth==128) { state->status=XR_XIR_BUDGET;return false; }
    if (xr_compile_resources_work(state->request->lexical->launch->resources,1)!=XR_COMPILE_RESOURCE_OK) { state->status=XR_XIR_BUDGET;return false; }
    if (node->type==AST_VARIABLE) return expression_binding(state,node->as.variable.name);
    XrCoreBuiltinId intrinsic=XR_CORE_BUILTIN_NONE;
    if (node->type==AST_CALL_EXPR && !expression_call_allowed(node,state->request->lexical->launch->resources,&state->status,&intrinsic)) return false;
    if (intrinsic!=XR_CORE_BUILTIN_NONE) {
        if (!expression_intrinsic_visible(state,node->as.call_expr.callee->as.variable.name)) return false;
        if (state->intrinsic_calls==UINT32_MAX) { state->status=XR_XIR_BUDGET;return false; }
        ++state->intrinsic_calls;
    }
    if (!expression_node_allowed(node)) { state->status=XR_XIR_BAD_TYPE;return false; }
    if (node->type==AST_LITERAL_FLOAT || node->type==AST_LITERAL_RUNE || node->type==AST_LITERAL_STRING || node->type==AST_LITERAL_NULL ||
        node->type==AST_ARRAY_LITERAL || node->type==AST_TUPLE_LITERAL || node->type==AST_INDEX_GET || node->type==AST_MEMBER_ACCESS || node->type==AST_FORCE_UNWRAP || node->type==AST_NULLISH_COALESCE || node->type==AST_CALL_EXPR) state->runtime=true;
    ++state->depth;bool ok=true;
    if (intrinsic!=XR_CORE_BUILTIN_NONE) {
        for (int i=0;i<node->as.call_expr.arg_count && ok;++i) ok=expression_visit(node->as.call_expr.arguments[i],state);
    } else ok=xr_ast_for_each_child(node,expression_visit,state);
    --state->depth;return ok;
}
typedef struct ExpressionSyntax { XrCompileResources *resources;uint32_t depth;XrXirStatus status; } ExpressionSyntax;
static bool expression_syntax(AstNode *node,void *context) {
    ExpressionSyntax *syntax=context;
    if (!node) return true;
    if (syntax->depth==128 || xr_compile_resources_work(syntax->resources,1)!=XR_COMPILE_RESOURCE_OK) { syntax->status=XR_XIR_BUDGET;return false; }
    if (!expression_node_allowed(node)) { syntax->status=XR_XIR_BAD_TYPE;return false; }
    XrCoreBuiltinId intrinsic=XR_CORE_BUILTIN_NONE;
    if (node->type==AST_CALL_EXPR && !expression_call_allowed(node,syntax->resources,&syntax->status,&intrinsic)) return false;
    ++syntax->depth;bool ok=true;
    if (intrinsic!=XR_CORE_BUILTIN_NONE) {
        for (int i=0;i<node->as.call_expr.arg_count && ok;++i) ok=expression_syntax(node->as.call_expr.arguments[i],syntax);
    } else ok=xr_ast_for_each_child(node,expression_syntax,syntax);
    --syntax->depth;return ok;
}
/* A debugger parse is a separate request owner on the original ledger. Its
 * sticky failure must not poison the target compiler's later requests. The
 * compiler's own pre-existing failure remains authoritative and is never reset. */
static XrParseStatus expression_parse(XrSourceCompiler *compiler,const char *text,const char *file,AstNode **output) {
    XrCompileResourceStatus status=xr_compile_session_resource_status(xr_source_compiler_session(compiler));
    if (status!=XR_COMPILE_RESOURCE_OK) return status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_PARSE_OUT_OF_MEMORY :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_PARSE_BUDGET : XR_PARSE_BAD_ARGUMENT;
    XrCompilerSession *parser=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(xr_source_compiler_resources(compiler),&parser);
    if (opened!=XR_COMPILER_SESSION_OK) return opened==XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_PARSE_OUT_OF_MEMORY :
        opened==XR_COMPILER_SESSION_BUDGET ? XR_PARSE_BUDGET : XR_PARSE_BAD_ARGUMENT;
    XrParseStatus parsed=xr_compile_parse_expression_string(parser,text,file,output);
    xr_compile_session_free(parser);return parsed;
}
XR_FUNC XrXirStatus xdap_expression_validate(XrSourceCompiler *compiler,const char *text,size_t length) {
    if (!compiler || !text || !length) return XR_XIR_BAD_TYPE;
    XrCompileResources *resources=xr_source_compiler_resources(compiler);
    if (length>(UINT64_MAX-1)/2 || xr_compile_resources_work(resources,(uint64_t)length*2+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    if (memchr(text,0,length)) return XR_XIR_BAD_TYPE;
    char *owned=NULL;XrCompileResourceStatus allocated=xr_compile_resources_alloc(resources,length+1,(void **)&owned);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    memcpy(owned,text,length);owned[length]=0;AstNode *program=NULL;
    XrParseStatus parsed=expression_parse(compiler,owned,"<breakpoint-expression>",&program);
    ExpressionSyntax syntax={resources,0,XR_XIR_OK};
    if (parsed!=XR_PARSE_OK) syntax.status=parsed==XR_PARSE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : parsed==XR_PARSE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
    else if (program->type!=AST_PROGRAM || program->as.program.count!=1 || !expression_syntax(program->as.program.statements[0],&syntax)) {
        if (syntax.status==XR_XIR_OK) syntax.status=XR_XIR_BAD_TYPE;
    }
    xr_program_destroy(program);xr_compile_resources_free(owned);return syntax.status;
}
#include "xdap_expression_source.inc.c"
#include "xdap_expression_runtime.inc.c"
#include "xdap_expression_receive.inc.c"
static XrXirStatus expression_compiled(ExpressionState *state,XrXirValue *output) {
    const XdapExpressionRequest *request=state->request;const XdapLexicalView *view=request->lexical;
    XdapJsonWriter source=xdap_json_writer(view->launch->resources,request->source_limit);
    ExpressionData data={0};XrXirValue scalar_arguments[128]={{0}};
    XrXirStatus status=state->runtime ? expression_data_prepare(state,&data) : XR_XIR_OK;
    const char *prefix=request->expected && state->runtime ? "export fn evaluator(" : "const evaluator = fn(";
    xdap_json_text(&source,prefix,strlen(prefix));
    if (state->runtime) xdap_json_text(&source,data.parameters.bytes,data.parameters.length);
    else for (uint32_t i=0;i<state->count && source.status==XR_XIR_OK;++i) {
        const ExpressionBinding *binding=&state->bindings[i];XrXirType type=(XrXirType)binding->value.type;
        if (type!=XR_XIR_BOOL && !xr_xir_type_is_integer(type)) { source.status=XR_XIR_BAD_TYPE;break; }
        const char *name=expression_primitive(type);
        if (i) xdap_json_text(&source,",",1);xdap_json_text(&source,binding->name,binding->length);
        xdap_json_text(&source,":",1);xdap_json_text(&source,name,strlen(name));scalar_arguments[i]=binding->value;
    }
    xdap_json_text(&source,")",1);
    if (request->expected) {
        xdap_json_text(&source," -> ",4);
        expression_type(&source,xr_xir_compile_type_arena_types(xr_xir_value_arena(request->expected)),
            (XrXirType)request->expected->type,0);
    }
    xdap_json_text(&source," {\n",3);
    if (state->runtime) xdap_json_text(&source,data.body.bytes,data.body.length);
    xdap_json_text(&source,"return (",8);xdap_json_text(&source,request->text,request->length);xdap_json_text(&source,"\n) }\n",5);
    if (status==XR_XIR_OK) status=source.status;
    XrXirSourceProduct *product=NULL;XrSourceCompileDiagnostic diagnostic={0};
    if (status==XR_XIR_OK) {
        XrSourceCompilerText input={NULL,"dap-expression",source.bytes,source.length};
        XrSourceCompileStatus built=xr_source_compiler_build_text(request->compiler,&input,&product,&diagnostic);
        if (built!=XR_SOURCE_COMPILE_OK) status=built==XR_SOURCE_COMPILE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            built==XR_SOURCE_COMPILE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
    }
    if (status==XR_XIR_OK) {
        uint32_t function=UINT32_MAX;uint32_t count=state->runtime ? data.count : state->count;
        status=expression_intrinsic_proof(state,product);
        if (status==XR_XIR_OK) status=expression_function(product,count,&function);
        if (status==XR_XIR_OK && state->runtime && !request->expected) status=expression_export(state,&data,&product,&function);
        if (status==XR_XIR_OK && state->runtime) status=expression_runtime(state,product,function,data.arguments,data.count,output);
        else if (status==XR_XIR_OK) {
            XrXirCtfeLimits limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES};
            XrXirCtfeStatus evaluated=xr_xir_compile_source_product_scalar(product,function,scalar_arguments,state->count,limits,output);
            status=evaluated==XR_XIR_CTFE_OK ? XR_XIR_OK : evaluated==XR_XIR_CTFE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
                evaluated==XR_XIR_CTFE_BUDGET || evaluated==XR_XIR_CTFE_STEP_LIMIT || evaluated==XR_XIR_CTFE_FRAME_LIMIT ||
                evaluated==XR_XIR_CTFE_OUTPUT_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
        }
    }
    if (status==XR_XIR_OK && request->expected) status=expression_receive(state,output);
    xr_source_compile_diagnostic_free(&diagnostic);xr_xir_compile_source_product_free(product);
    expression_data_free(&data);xdap_json_writer_free(&source);return status;
}
XR_FUNC XrXirStatus xdap_expression_evaluate(const XdapExpressionRequest *request,XrXirValue *output) {
    if (!request || !request->compiler || !request->lexical || !request->text || !request->length || !request->source_limit ||
        !request->lexical->launch || !request->lexical->source || !request->lexical->function || !request->lexical->snapshot ||
        !output || output->type || output->reserved || output->payload ||
        xr_source_compiler_resources(request->compiler)!=request->lexical->launch->resources) return XR_XIR_BAD_STRUCTURE;
    if (request->expected && !xr_xir_value_valid(request->expected)) return XR_XIR_BAD_STRUCTURE;
    if (request->length>(UINT64_MAX-1)/2 ||
        xr_compile_resources_work(request->lexical->launch->resources,(uint64_t)request->length*2+1)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_BUDGET;
    if (memchr(request->text,0,request->length)) return XR_XIR_BAD_TYPE;
    if (request->length==SIZE_MAX) return XR_XIR_BUDGET;
    char *text=NULL;XrCompileResourceStatus allocated=xr_compile_resources_alloc(request->lexical->launch->resources,request->length+1,(void **)&text);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    memcpy(text,request->text,request->length);text[request->length]=0;
    AstNode *program=NULL;XrParseStatus parsed=expression_parse(request->compiler,text,"<debug-expression>",&program);
    if (parsed!=XR_PARSE_OK) { xr_compile_resources_free(text);return parsed==XR_PARSE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : parsed==XR_PARSE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE; }
    XdapExpressionRequest owned=*request;owned.text=text;ExpressionState state={.request=&owned};
    if (request->expected && request->expected->type!=XR_XIR_BOOL &&
        !xr_xir_type_is_integer((XrXirType)request->expected->type)) state.runtime=true;
    if (program->type!=AST_PROGRAM || program->as.program.count!=1) state.status=XR_XIR_BAD_STRUCTURE;
    AstNode *expression=state.status==XR_XIR_OK ? program->as.program.statements[0] : NULL;
    if (state.status==XR_XIR_OK && expression_visit(expression,&state)) {
        while (expression->type==AST_GROUPING) expression=expression->as.grouping;
        bool direct=expression->type==AST_VARIABLE && (!request->expected ||
            (state.bindings[0].value.type==request->expected->type &&
             xr_xir_value_arena(&state.bindings[0].value)==xr_xir_value_arena(request->expected)));
        if (direct) { *output=state.bindings[0].value;state.bindings[0].value=(XrXirValue){0}; }
        else state.status=expression_compiled(&state,output);
    }
    for (uint32_t i=0;i<state.count;++i) xr_xir_value_drop(&state.bindings[i].value);
    xr_program_destroy(program);xr_compile_resources_free(text);return state.status;
}
