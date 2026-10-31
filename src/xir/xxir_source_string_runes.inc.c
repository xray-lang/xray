/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_string_runes.inc.c - Module-local owned UTF-8 Rune producers
 *
 * KEY CONCEPT:
 *   A real generator captures one immutable String and yields its Unicode
 *   scalars through the ordinary typed Iterator and executor protocol.
 */
static bool source_string_runes_signature(SourceContext *ctx,AstNode *node,
    SourceValue receiver,const XrNativeMemberDeclaration *member,XrXirType *type) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    if(!node || node->type!=AST_CALL_EXPR)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Rune producer requires its actual call");
    const CallExprNode *call=&node->as.call_expr;
    if(!native || !member || receiver.type!=XR_XIR_STRING || member!=xr_native_declaration_member(native,"runes") ||
        !member->is_public || !member->is_method || member->is_static ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->parameter_count ||
        member->operation!=XR_NATIVE_OPERATION_STRING_RUNES || member->result!=XR_NATIVE_TERM_ITERATOR_RUNE ||
        member->allocation!=XR_NATIVE_ALLOCATION_MAY_HEAP || member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        !member->failures || strcmp(member->failures,"allocation,retain,limit") ||
        call->arg_count || call->type_arg_count || call->default_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Rune producer requires its exact String receiver declaration");
    return source_iterator_type(ctx,XR_XIR_RUNE,XR_XIR_NEVER,type);
}
static bool source_string_runes_byte(SourceContext *ctx,SourceValue bytes,
    SourceValue index,SourceValue *value) {
    SourceValue byte;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_GET,XR_XIR_U8,
        {bytes.id,index.id},{0},0,{0}},&byte) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_I64,
        {byte.id},{0},0,{0}},value);
}
static bool source_string_runes_scalar(SourceContext *ctx,SourceValue bytes,
    SourceValue index,SourceValue lead,uint32_t width) {
    SourceValue scalar,mask,radix;
    if(!source_bits_constant(ctx,XR_XIR_I64,width==1?127:width==2?31:width==3?15:7,&mask) ||
        !source_bits_binary(ctx,XR_XIR_AND_INT,lead,mask,&scalar) ||
        !source_bits_constant(ctx,XR_XIR_I64,64,&radix) ||
        !source_bits_constant(ctx,XR_XIR_I64,63,&mask)) return false;
    for(uint32_t p=1;p<width;++p) {
        SourceValue offset,position,byte,tail,shifted;
        if(!source_request_poll(ctx,NULL) ||
            !source_bits_constant(ctx,XR_XIR_I64,p,&offset) ||
            !source_bits_binary(ctx,XR_XIR_ADD_INT,index,offset,&position) ||
            !source_string_runes_byte(ctx,bytes,position,&byte) ||
            !source_bits_binary(ctx,XR_XIR_AND_INT,byte,mask,&tail) ||
            !source_bits_binary(ctx,XR_XIR_MUL_INT,scalar,radix,&shifted) ||
            !source_bits_binary(ctx,XR_XIR_ADD_INT,shifted,tail,&scalar)) return false;
    }
    SourceValue rune;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_INTEGER_TO_RUNE,XR_XIR_RUNE,
        {scalar.id},{0},0,{0}},&rune) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GENERATOR_YIELD,XR_XIR_UNIT,
        {rune.id},{0},0,{0}},NULL);
}
static bool source_string_runes_body(SourceContext *ctx) {
    SourceValue text={.id=0,.type=XR_XIR_STRING,.place={0}},bytes,length,zero;
    XrXirType array;
    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},&array) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_STRING_COPY_BYTES,array,{text.id},{0},0,{0}},&bytes) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,{bytes.id},{0},0,{0}},&length) ||
        !source_bits_constant(ctx,XR_XIR_I64,0,&zero)) return false;
    SourceCounter cursor;
    if(!source_counter_open(ctx,zero,length,false,&cursor)) return false;
    SourceValue lead;
    if(!source_string_runes_byte(ctx,bytes,cursor.index,&lead)) return false;
    /* Input is an owned copy of an already valid String. Continuation bytes
     * are skipped; every leading byte yields exactly one scalar. */
    static const int64_t lower[]={0,192,224,240},upper[]={128,224,240,256};
    for(uint32_t width=1;width<=4;++width) {
        SourceValue limit,condition;SourceGuard above,below;
        if(!source_request_poll(ctx,NULL) ||
            !source_bits_constant(ctx,XR_XIR_I64,lower[width-1],&limit) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GE_INT,XR_XIR_BOOL,
            {lead.id,limit.id},{0},0,{0}},&condition) || !source_guard_open(ctx,condition,false,&above) ||
            !source_bits_constant(ctx,XR_XIR_I64,upper[width-1],&limit) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,
            {lead.id,limit.id},{0},0,{0}},&condition) || !source_guard_open(ctx,condition,false,&below) ||
            !source_string_runes_scalar(ctx,bytes,cursor.index,lead,width) ||
            !source_guard_close(ctx,&below) || !source_guard_close(ctx,&above)) return false;
    }
    return source_counter_close(ctx,&cursor) && finish_body(ctx);
}
static bool source_string_runes_producer(SourceContext *ctx,AstNode *node,
    XrXirType result,uint32_t *output) {
    uint32_t module=ctx->module;
    if(module>=ctx->module_count || ctx->bodies[ctx->function].module!=module)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Rune producer has no current module owner");
    if(!ctx->string_rune_producers) {
        ctx->string_rune_producers=source_alloc(ctx,ctx->module_count,sizeof(*ctx->string_rune_producers));
        if(!ctx->string_rune_producers) return false;
    }
    if(ctx->string_rune_producers[module]) {
        uint32_t found=ctx->string_rune_producers[module]-1;
        if(found>=ctx->function_count || ctx->identities[found].module!=module ||
            ctx->functions[found].result!=result)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Rune producer changed its owned signature");
        *output=found;return true;
    }
    uint32_t outer=ctx->function,index=ctx->function_count;
    if(index>=ctx->function_capacity || index>=ctx->compile.limits.functions)
        return source_fail(ctx,node,XR_XIR_BUDGET,"Rune producer function capacity exhausted");
    XrXirType *parameter=source_alloc(ctx,1,sizeof(*parameter));char *name=source_alloc(ctx,40,1);
    if(!parameter || !name) return false;
    *parameter=XR_XIR_STRING;
    if(source_format(ctx,name,40,"$string_runes%u",index)!=XR_DIAG_OK)
        return source_fail(ctx,node,XR_XIR_BUDGET,"Rune producer name exhausted");
    SourceName declaration={0};declaration.name=name;declaration.node=node;
    if(!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,0,
        source_query_range(ctx,node,NULL)) ||
        !source_query_syntax_role(ctx,declaration.declaration,XR_XIR_SOURCE_SYNTAX_NONE,0)) return false;
    SourceFunction *body=&ctx->bodies[index];body->node=node;body->module=module;
    body->type_owner=NULL;body->parameters=parameter;body->declaration=declaration.declaration;
    ctx->functions[index]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),parameter,1,
        result,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER};
    ctx->generics[index]=(XrXirGeneric){0};
    ctx->identities[index]=(XrXirFunctionIdentity){.module=module,.promises=XR_XIR_FUNCTION_GENERATOR,
        .method_kind=XR_XIR_NON_MEMBER};
    ++ctx->function_count;
    bool returned=ctx->returned;SourceLoop *loop=ctx->loop;
    SourceName *locals=ctx->locals,*scope=ctx->scope;SourceFact *facts=ctx->facts;
    SourceEpoch *epochs=ctx->epochs;SourceTypeScope type_scope=ctx->type_scope;
    uint32_t alternatives=ctx->flow_alternatives,debug_function=ctx->debug_function;
    XrXirSourceDebugPoint debug_point=ctx->debug_point;
    bool debug_entry=ctx->debug_entry,debug_jump_entry=ctx->debug_jump_entry;
    SourceExpressionPlan *expression=ctx->active_expression,*argument=ctx->effect_argument;
    ctx->function=index;ctx->returned=false;ctx->loop=NULL;ctx->locals=ctx->scope=NULL;
    ctx->facts=NULL;ctx->epochs=NULL;ctx->flow_alternatives=0;ctx->type_scope=(SourceTypeScope){0};
    ctx->debug_point=(XrXirSourceDebugPoint){0};ctx->debug_function=index;
    ctx->debug_entry=ctx->debug_jump_entry=false;ctx->active_expression=ctx->effect_argument=NULL;
    bool ok=source_string_runes_body(ctx);
    ctx->function=outer;ctx->returned=returned;ctx->loop=loop;
    ctx->locals=locals;ctx->scope=scope;ctx->facts=facts;ctx->epochs=epochs;
    ctx->flow_alternatives=alternatives;ctx->type_scope=type_scope;ctx->debug_function=debug_function;
    ctx->debug_point=debug_point;ctx->debug_entry=debug_entry;ctx->debug_jump_entry=debug_jump_entry;
    ctx->active_expression=expression;ctx->effect_argument=argument;
    if(!ok) return false;
    XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration.declaration-1];
    XrXirSourceType *parameters=source_alloc(ctx,1,sizeof(*parameters));if(!parameters)return false;
    *parameters=source_query_type(ctx,XR_XIR_STRING);
    const char *signature=source_owned_text(ctx,"(value: string) -> Iterator<rune>");if(!signature)return false;
    record=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration.declaration-1];
    record->signature=signature;
    record->parameters=parameters;record->parameter_count=1;record->type=source_query_type(ctx,result);
    record->exported=false;
    ctx->string_rune_producers[module]=index+1;*output=index;return true;
}
static bool source_string_runes_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *output) {
    XrXirType result;uint32_t producer;
    if(!source_string_runes_signature(ctx,node,receiver,member,&result) ||
        !source_string_runes_producer(ctx,node,result,&producer)) return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_GENERATOR_NEW,result,{0},{0},producer,{0}},
        &receiver,1,output) && source_string_value_reference(ctx,node->as.call_expr.callee,member,result);
}
