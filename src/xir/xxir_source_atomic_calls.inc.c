/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_atomic_calls.inc.c - Governed Atomic construction and member calls
 *
 * KEY CONCEPT:
 *   Declaration authority and definition-context proof precede operation recipes.
 */
static bool source_atomic_prove(SourceContext *ctx, AstNode *node, XrXirType element, uint32_t marker) {
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirStatus status=xr_xir_compile_type_constraints(&ctx->compile,&module,ctx->function,
        element,(XrXirConstraint){.markers=marker});
    if (status==XR_XIR_OK) status=xr_xir_compile_type_access(&ctx->compile,&module,ctx->function,element);
    return status==XR_XIR_OK || source_fail(ctx,node,status,"Atomic operation requires its definition-context constraint");
}
static bool source_atomic_construct(SourceContext *ctx,AstNode *node,AstNode *input,
    SourceExpectedType expected,SourceValue *value) {
    if (!input || source_native_type_shadowed(ctx,"Atomic"))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"constructor does not bind the governed Atomic declaration");
    const XrNativeTypeDeclaration *native=source_native_find(ctx,"Atomic");
    if (!native || native->id!=XR_NATIVE_DECLARATION_ATOMIC || !source_native_atomic_declaration(ctx))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic declaration authority is not admitted");
    SourceValue initial={0};XrXirType type=XR_XIR_UNIT;
    if (!source_plan_expression(ctx,input,expected,&initial) ||
        (expected.present && initial.type!=expected.type)) return false;
    if (!source_atomic_prove(ctx,node,initial.type,XR_XIR_CONSTRAINT_ATOMIC_VALUE) ||
        !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=initial.type},&type)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_ATOMIC_NEW,.type=type,.args={initial.id,0}},value) &&
        source_query_target_reference(ctx,source_query_range(ctx,node,NULL),ctx->atomic_declaration,XR_XIR_SOURCE_CALL);
}
static bool source_atomic_inferred_construct(SourceContext *ctx,AstNode *node,
    SourceExpectedType context,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    if (call->type_arg_count<0 || call->type_arg_count>1 || call->default_arg_count || call->arg_count!=1 || !call->arguments ||
        (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic construction requires one ordinary initial value");
    SourceExpectedType expected={false,XR_XIR_UNIT,false};
    if (call->type_arg_count) {
        if (!call->type_args || !source_type(ctx,call->type_args[0],&expected.type)) return false;
        expected.present=true;
    } else if (context.present && xr_xir_type_is_atomic(&ctx->types,context.type))
        expected=(SourceExpectedType){true,xr_xir_atomic_element(&ctx->types,context.type),false};
    return source_atomic_construct(ctx,node,call->arguments[0],expected,value);
}
static bool source_atomic_explicit_construct(SourceContext *ctx,AstNode *node,SourceValue *value) {
    NewExprNode *call=&node->as.new_expr;
    if (call->is_type_namespace || call->module_name || call->type_arg_count!=1 || !call->type_args ||
        call->arg_count!=1 || !call->arguments || (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic<T> construction requires one element type and initial value");
    XrXirType element=XR_XIR_UNIT;
    return source_type(ctx,call->type_args[0],&element) &&
        source_atomic_construct(ctx,node,call->arguments[0],(SourceExpectedType){true,element,false},value);
}
static XrXirOp source_atomic_operation(XrNativeOperation operation) {
    switch (operation) {
    case XR_NATIVE_OPERATION_ATOMIC_LOAD:return XR_XIR_ATOMIC_LOAD;
    case XR_NATIVE_OPERATION_ATOMIC_STORE:return XR_XIR_ATOMIC_STORE;
    case XR_NATIVE_OPERATION_ATOMIC_ADD:return XR_XIR_ATOMIC_ADD;
    case XR_NATIVE_OPERATION_ATOMIC_SUB:return XR_XIR_ATOMIC_SUB;
    case XR_NATIVE_OPERATION_ATOMIC_FETCH_ADD:return XR_XIR_ATOMIC_FETCH_ADD;
    case XR_NATIVE_OPERATION_ATOMIC_FETCH_SUB:return XR_XIR_ATOMIC_FETCH_SUB;
    case XR_NATIVE_OPERATION_ATOMIC_SWAP:return XR_XIR_ATOMIC_SWAP;
    case XR_NATIVE_OPERATION_ATOMIC_COMPARE_EXCHANGE:return XR_XIR_ATOMIC_COMPARE_EXCHANGE;
    case XR_NATIVE_OPERATION_ATOMIC_TOGGLE:return XR_XIR_ATOMIC_TOGGLE;
    case XR_NATIVE_OPERATION_ATOMIC_TO_STRING:return XR_XIR_ATOMIC_TO_STRING;
    default:return XR_XIR_INVALID;
    }
}
static XrXirSourceType source_atomic_schema_type(SourceContext *ctx,XrNativeTypeTerm term) {
    if (term==XR_NATIVE_TERM_ELEMENT)
        return (XrXirSourceType){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,ctx->atomic_declaration,true};
    if (term==XR_NATIVE_TERM_BOOL) return (XrXirSourceType){XR_XIR_BOOL,0,true};
    if (term==XR_NATIVE_TERM_STRING) return (XrXirSourceType){XR_XIR_STRING,0,true};
    if (term==XR_NATIVE_TERM_ORDERING) {
        XrXirType ordering=XR_XIR_UNIT;
        if (!source_ordering_type(ctx,&ordering) || !source_nullable_type(ctx,ordering,&ordering))
            return (XrXirSourceType){0};
        return (XrXirSourceType){ordering,0,true};
    }
    if (term==XR_NATIVE_TERM_TUPLE_ELEMENT_BOOL) {
        XrXirCallableParameter *fields=source_alloc(ctx,2,sizeof(*fields));
        XrXirType type=XR_XIR_UNIT;
        if (!fields) return (XrXirSourceType){0};
        fields[0]=(XrXirCallableParameter){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,0};
        fields[1]=(XrXirCallableParameter){XR_XIR_BOOL,0};
        if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,
            .parameters=fields,.parameter_count=2},&type)) return (XrXirSourceType){0};
        return (XrXirSourceType){type,ctx->atomic_declaration,true};
    }
    return (XrXirSourceType){XR_XIR_UNIT,0,term==XR_NATIVE_TERM_UNIT};
}
static bool source_atomic_member_reference(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member) {
    if (!member->id || member->id>10)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Atomic member identity is invalid");
    uint32_t index=member->id-1;
    if (!ctx->atomic_members[index]) {
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);
        if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,ctx->atomic_declaration,
            (XrXirSourceRange){ctx->atomic_module,(int)member->line,(int)member->column,
                (int)member->line,(int)member->column+(int)source_text_size(ctx,member->name)})) return false;
        ctx->atomic_members[index]=symbol.declaration;
        XrXirSourceType *parameters=member->parameter_count ? source_alloc(ctx,member->parameter_count,sizeof(*parameters)) : NULL;
        if (member->parameter_count && !parameters) return false;
        for (uint32_t p=0;p<member->parameter_count;++p) {
            if (!source_work(ctx,node)) return false;
            parameters[p]=source_atomic_schema_type(ctx,member->parameters[p].type);
            if (!parameters[p].known) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic parameter schema is not admitted");
        }
        XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration-1];
        record->native_identity=member->id;record->signature=source_owned_text(ctx,member->signature);
        if (!record->signature) return false;
        record->exported=true;record->parameters=parameters;record->parameter_count=member->parameter_count;
        record->type=source_atomic_schema_type(ctx,member->result);
        if (!record->type.known) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic result schema is not admitted");
        record->generic_parent=ctx->atomic_declaration;record->generic_parent_count=1;
    }
    return source_query_target_reference(ctx,source_query_range(ctx,node,NULL),ctx->atomic_members[index],XR_XIR_SOURCE_CALL);
}
static bool source_atomic_call(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;
    if (!source_native_atomic_declaration(ctx)) return false;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ATOMIC);
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,call->callee->as.member_access.name);
    XrXirOp operation=member ? source_atomic_operation(member->operation) : XR_XIR_INVALID;
    if (!member || !member->is_public || !member->lowered || operation==XR_XIR_INVALID)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic member has no admitted operation contract");
    uint32_t required=xr_xir_atomic_required_operands(operation)-1;
    uint32_t maximum=operation==XR_XIR_ATOMIC_TO_STRING ? required : required+1;
    if (call->arg_count<0 || (uint32_t)call->arg_count<required || (uint32_t)call->arg_count>maximum ||
        call->type_arg_count || call->default_arg_count || (call->arg_count && !call->arguments))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic method requires its declared value operands and optional ordering");
    XrXirType element=xr_xir_atomic_element(&ctx->types,receiver.type);
    uint32_t marker=operation==XR_XIR_ATOMIC_TOGGLE ? XR_XIR_CONSTRAINT_ATOMIC_BOOLEAN :
        operation>=XR_XIR_ATOMIC_ADD && operation<=XR_XIR_ATOMIC_FETCH_SUB ? XR_XIR_CONSTRAINT_ATOMIC_NUMBER : XR_XIR_CONSTRAINT_ATOMIC_VALUE;
    if (!source_atomic_prove(ctx,node,element,marker)) return false;
    SourceValue operands[4]={receiver};
    for (uint32_t p=0;p<(uint32_t)call->arg_count;++p) {
        if (call->arg_accesses && call->arg_accesses[p]!=XR_CALL_ARG_PLAIN)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic method operands are ordinary values");
        if (p<required) {
            if (!source_plan_expression(ctx,call->arguments[p],(SourceExpectedType){true,element,false},&operands[p+1])) return false;
            if (operands[p+1].type!=element) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic method operand must have the exact element type");
        } else {
            XrXirType expected=XR_XIR_UNIT;
            if (!source_ordering_type(ctx,&expected) || !source_nullable_type(ctx,expected,&expected) ||
                !source_plan_expression(ctx,call->arguments[p],(SourceExpectedType){true,expected,false},&operands[p+1])) return false;
            XrXirType ordering=operands[p+1].type;
            if (!xr_xir_type_is_nullable(&ctx->types,ordering) ||
                !xr_xir_nominal_native_ordering(&ctx->types,xr_xir_nullable_element(&ctx->types,ordering)))
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Atomic ordering requires the governed Nullable<Ordering> declaration");
        }
    }
    XrXirType result=member->result==XR_NATIVE_TERM_UNIT ? XR_XIR_UNIT :
        member->result==XR_NATIVE_TERM_BOOL ? XR_XIR_BOOL : member->result==XR_NATIVE_TERM_STRING ? XR_XIR_STRING : element;
    if (operation==XR_XIR_ATOMIC_COMPARE_EXCHANGE) {
        XrXirCallableParameter *fields=source_alloc(ctx,2,sizeof(*fields));if (!fields) return false;
        fields[0]=(XrXirCallableParameter){element,0};fields[1]=(XrXirCallableParameter){XR_XIR_BOOL,0};
        if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2},&result)) return false;
    }
    return source_recipe_group(ctx,(XrXirInstruction){.op=operation,.type=result},operands,(uint32_t)call->arg_count+1,value) &&
        source_atomic_member_reference(ctx,node,member);
}
