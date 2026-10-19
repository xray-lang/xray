/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_stringbuilder.inc.c - Closed text normalization to an ordinary Class
 *
 * The public six-arm declaration lowers to an ordinary append(String) method.
 * Receiver and argument evaluation finish before that method reads its field.
 */
#include "xxir_source_stringbuilder_query.inc.c"
static bool source_stringbuilder_type(SourceContext *ctx,XrXirType type) {
    SourceName *owner=source_prelude_name(ctx,"StringBuilder");
    return owner && owner->kind==SOURCE_NOMINAL && owner->type==type;
}
static bool source_stringbuilder_invoke(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const char *name,const SourceValue *argument,SourceValue *value) {
    SourceName *method=source_method_find(ctx,receiver.type,name);
    const XrXirFunctionIdentity *identity=method?source_method_identity(ctx,node,method):NULL;
    const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,receiver.type);
    if(!identity || identity->method_kind!=XR_XIR_READ_METHOD || identity->member_access ||
        identity->module+1!=ctx->prelude_module || !type || identity->nominal_owner!=type->nominal.declaration+1)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"StringBuilder method has no ordinary canonical owner");
    const XrXirFunction *function=&ctx->functions[method->index];
    if(function->parameter_count!=(argument?2u:1u) || function->parameters[0]!=receiver.type ||
        (argument && (argument->type!=XR_XIR_STRING || function->parameters[1]!=XR_XIR_STRING)))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"StringBuilder physical method signature differs");
    SourceValue arguments[2]={receiver,argument?*argument:(SourceValue){0}};
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,function->result,{0},{0},method->index,{0}},
        arguments,argument?2:1,value);
}
static bool source_stringbuilder_call(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    const CallExprNode *call=&node->as.call_expr;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING_BUILDER);
    if(!source_native_admit(ctx,native)) return false;
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,call->callee->as.member_access.name);
    if(!member || !member->is_public || !member->is_method || member->is_static || member->receiver!=XR_NATIVE_RECEIVER_READ ||
        call->type_arg_count || call->default_arg_count || call->arg_count!=(int)member->parameter_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"StringBuilder requires its exact declared member arguments");
    bool append=member->operation==XR_NATIVE_OPERATION_STRING_BUILDER_APPEND;
    if(!append && member->operation!=XR_NATIVE_OPERATION_STRING_BUILDER_TO_STRING && member->operation!=XR_NATIVE_OPERATION_STRING_BUILDER_CLEAR)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"StringBuilder member has no admitted recipe");
    SourceValue text={0};
    if(append) {
        if(!call->arguments || (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"StringBuilder append requires an ordinary text operand");
        AstNode *argument=call->arguments[0];
        while(argument && argument->type==AST_GROUPING) {
            if(!source_work(ctx,node)) return false;
            argument=argument->as.grouping;
        }
        if(argument && argument->type==AST_LITERAL_NULL) {
            uint32_t literal=0;
            if(!source_literal_append(ctx,node,"null",4,&literal) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},literal,{0}},&text)) return false;
        } else {
            SourceValue input={0};
            if(!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){false,XR_XIR_UNIT,false,false},&input)) return false;
            if(input.type!=XR_XIR_STRING && input.type!=XR_XIR_RUNE && input.type!=XR_XIR_I64 && input.type!=XR_XIR_F64 && input.type!=XR_XIR_BOOL)
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"StringBuilder append accepts only its six declared text arms; general union receiving is not admitted here");
            if(!source_text_conversion(ctx,call->arguments[0],input,&text)) return false;
        }
    }
    XrXirType result=member->result==XR_NATIVE_TERM_STRING?XR_XIR_STRING:receiver.type;
    return source_stringbuilder_reference(ctx,call->callee,member,result) &&
        source_stringbuilder_invoke(ctx,node,receiver,member->name,append?&text:NULL,value);
}
static bool source_stringbuilder_length(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    SourceValue text={0};
    return source_stringbuilder_invoke(ctx,node,receiver,"toString",NULL,&text) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_STRING_LEN,XR_XIR_I64,{text.id},{0},0,{0}},value);
}
