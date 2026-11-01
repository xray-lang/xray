/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_string_split.inc.c - Ordinary owned single-pattern String splitting
 *
 * KEY CONCEPT:
 *   A module-local throwing body uses the existing Rune-indexed String and
 *   owned Array operations. Receiver and separator are evaluated once before
 *   validation; partial results retire through ordinary frame ownership.
 */
static bool source_string_split_slice(SourceContext *ctx,SourceValue text,
    SourceValue first,SourceValue last,SourceValue *output) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    const XrNativeIntrinsicDeclaration *slice=xr_native_intrinsic_for_member(native,
        xr_native_declaration_member(native,"slice"));
    if(!slice || slice->implementation!=XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_SLICE)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"split lost its canonical String slice declaration");
    SourceValue arguments[]={text,first,last};
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL_INTRINSIC,XR_XIR_STRING,
        {0},{0},slice->id,{0}},arguments,3,output);
}
static bool source_string_split_body(SourceContext *ctx,XrXirType result,XrXirType error) {
    SourceValue text={.id=0,.type=XR_XIR_STRING},pattern={.id=1,.type=XR_XIR_STRING};
    SourceValue zero,width,length,empty;
    if(!source_bits_constant(ctx,XR_XIR_I64,0,&zero) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_STRING_LEN,XR_XIR_I64,{pattern.id},{0},0,{0}},&width) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{width.id,zero.id},{0},0,{0}},&empty))return false;
    SourceGuard invalid;
    if(!source_guard_open(ctx,empty,false,&invalid))return false;
    SourceValue failure;
    if(!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_ENUM_NEW,error,{0},{0},0,{0}},NULL,0,&failure) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{failure.id},{0},0,{0}},NULL))return false;
    ctx->returned=true;
    if(!source_guard_close(ctx,&invalid) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_STRING_LEN,XR_XIR_I64,{text.id},{0},0,{0}},&length))return false;
    SourceValue array,storage,place,cursor;XrXirType array_cell,index_cell;
    if(!source_cell_type(ctx,result,&array_cell) || !source_cell_type(ctx,XR_XIR_I64,&index_cell) ||
        !source_recipe_group(ctx,(XrXirInstruction){XR_XIR_ARRAY_NEW,result,{0},{0},0,{0}},NULL,0,&array) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,array_cell,{array.id},{0},0,{0}},&storage) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,result,{storage.id},{0},0,{0}},&place) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,index_cell,{zero.id},{0},0,{0}},&cursor))return false;
    uint32_t head=ctx->bodies[ctx->function].block_count;
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{head},0,{0}},NULL) || !begin_block(ctx))return false;
    SourceValue start,position,done;
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{cursor.id},{0},0,{0}},&start))return false;
    SourceValue search[]={text,pattern,start};
    if(!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_STRING_INDEX_OF,XR_XIR_I64,{0},{0},0,{0}},search,3,&position) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{position.id,zero.id},{0},0,{0}},&done))return false;
    SourceGuard tail;
    if(!source_guard_open(ctx,done,false,&tail))return false;
    SourceValue field,answer;
    if(!source_string_split_slice(ctx,text,start,length,&field) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_PUSH,XR_XIR_UNIT,{place.id,field.id},{0},0,{0}},NULL) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,result,{storage.id},{0},0,{0}},&answer) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{answer.id},{0},0,{0}},NULL))return false;
    ctx->returned=true;
    if(!source_guard_close(ctx,&tail) || !source_string_split_slice(ctx,text,start,position,&field) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_PUSH,XR_XIR_UNIT,{place.id,field.id},{0},0,{0}},NULL))return false;
    SourceValue next;
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{position.id,width.id},{0},0,{0}},&next) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{cursor.id,next.id},{0},0,{0}},NULL) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{head},0,{0}},NULL))return false;
    ctx->returned=true;return finish_body(ctx);
}
static bool source_string_split_producer(SourceContext *ctx,AstNode *node,XrXirType result,
    XrXirType error,uint32_t *output) {
    uint32_t module=ctx->module;
    if(module>=ctx->module_count || ctx->bodies[ctx->function].module!=module)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"split has no actual module owner");
    if(!ctx->string_split_producers) {
        ctx->string_split_producers=source_alloc(ctx,ctx->module_count,sizeof(*ctx->string_split_producers));
        if(!ctx->string_split_producers)return false;
    }
    if(ctx->string_split_producers[module]) {
        uint32_t found=ctx->string_split_producers[module]-1;
        if(found>=ctx->function_count || ctx->identities[found].module!=module ||
            ctx->functions[found].result!=result || ctx->functions[found].error_type!=error)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"split helper changed its owned signature");
        *output=found;return true;
    }
    uint32_t outer=ctx->function,index=ctx->function_count;
    if(index>=ctx->function_capacity || index>=ctx->compile.limits.functions)
        return source_fail(ctx,node,XR_XIR_BUDGET,"split helper function capacity exhausted");
    XrXirType *parameters=source_alloc(ctx,2,sizeof(*parameters));char *name=source_alloc(ctx,40,1);
    if(!parameters || !name)return false;
    parameters[0]=parameters[1]=XR_XIR_STRING;
    if(source_format(ctx,name,40,"$string_split%u",index)!=XR_DIAG_OK)
        return source_fail(ctx,node,XR_XIR_BUDGET,"split helper name exhausted");
    SourceName declaration={.name=name,.node=node};
    if(!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,0,source_query_range(ctx,node,NULL)) ||
        !source_query_syntax_role(ctx,declaration.declaration,XR_XIR_SOURCE_SYNTAX_NONE,0))return false;
    SourceFunction *body=&ctx->bodies[index];body->node=node;body->module=module;
    body->parameters=parameters;body->declaration=declaration.declaration;
    ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)source_text_size(ctx,name),
        .parameters=parameters,.parameter_count=2,.result=result,.error_type=error};
    ctx->generics[index]=(XrXirGeneric){0};
    ctx->identities[index]=(XrXirFunctionIdentity){.module=module,.promises=XR_XIR_FUNCTION_NO_SUSPEND};
    ++ctx->function_count;
    bool returned=ctx->returned;SourceLoop *loop=ctx->loop;
    SourceName *locals=ctx->locals,*scope=ctx->scope;SourceFact *facts=ctx->facts;SourceEpoch *epochs=ctx->epochs;
    SourceTypeScope type_scope=ctx->type_scope;uint32_t alternatives=ctx->flow_alternatives,debug_function=ctx->debug_function;
    XrXirSourceDebugPoint debug_point=ctx->debug_point;bool debug_entry=ctx->debug_entry,debug_jump=ctx->debug_jump_entry;
    SourceExpressionPlan *expression=ctx->active_expression,*argument=ctx->effect_argument;
    ctx->function=index;ctx->returned=false;ctx->loop=NULL;ctx->locals=ctx->scope=NULL;ctx->facts=NULL;ctx->epochs=NULL;
    ctx->type_scope=(SourceTypeScope){0};ctx->flow_alternatives=0;ctx->debug_function=index;
    ctx->debug_point=(XrXirSourceDebugPoint){0};ctx->debug_entry=ctx->debug_jump_entry=false;
    ctx->active_expression=ctx->effect_argument=NULL;
    bool ok=source_string_split_body(ctx,result,error);
    ctx->function=outer;ctx->returned=returned;ctx->loop=loop;ctx->locals=locals;ctx->scope=scope;
    ctx->facts=facts;ctx->epochs=epochs;ctx->type_scope=type_scope;ctx->flow_alternatives=alternatives;
    ctx->debug_function=debug_function;ctx->debug_point=debug_point;ctx->debug_entry=debug_entry;ctx->debug_jump_entry=debug_jump;
    ctx->active_expression=expression;ctx->effect_argument=argument;
    if(!ok)return false;
    XrXirSourceType *query_parameters=source_alloc(ctx,2,sizeof(*query_parameters));
    const char *signature=source_owned_text(ctx,"(text: string, separator: string) throws(StringPatternError) -> Array<string>");
    if(!query_parameters || !signature)return false;
    query_parameters[0]=query_parameters[1]=source_query_type(ctx,XR_XIR_STRING);
    XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration.declaration-1];
    record->signature=signature;record->parameters=query_parameters;record->parameter_count=2;record->type=source_query_type(ctx,result);
    ctx->string_split_producers[module]=index+1;*output=index;return true;
}
static bool source_string_split_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *output) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    const CallExprNode *call=&node->as.call_expr;
    if(receiver.type!=XR_XIR_STRING || !native || member!=xr_native_declaration_member(native,"split") ||
        !member->is_public || !member->is_method || member->is_static || member->receiver!=XR_NATIVE_RECEIVER_READ ||
        member->operation!=XR_NATIVE_OPERATION_STRING_SPLIT || member->result!=XR_NATIVE_TERM_ARRAY_STRING ||
        member->allocation!=XR_NATIVE_ALLOCATION_MAY_HEAP || member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        !member->failures || strcmp(member->failures,"allocation,retain,limit") ||
        member->parameter_count!=1 || !member->parameters || member->parameters[0].type!=XR_NATIVE_TERM_STRING ||
        member->parameters[0].optional || member->parameters[0].variadic || call->arg_count!=1 || !call->arguments ||
        call->type_arg_count || call->default_arg_count || (call->arg_accesses && call->arg_accesses[0]!=XR_CALL_ARG_PLAIN))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"split requires its exact ordinary String declaration");
    SourceValue arguments[2]={receiver};
    if(!source_plan_expression(ctx,call->arguments[0],(SourceExpectedType){true,XR_XIR_STRING,false,false},arguments+1))return false;
    SourceName *error=source_prelude_name(ctx,"StringPatternError");
    if(!error || error->kind!=SOURCE_NOMINAL || error->index>=ctx->nominals.count ||
        ctx->nominals.declarations[error->index].native.native_id)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"split lost its ordinary canonical error declaration");
    XrXirType error_type=error->type,result;uint32_t target;
    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING},&result) ||
        !source_string_split_producer(ctx,node,result,error_type,&target) ||
        !source_string_value_reference(ctx,call->callee,member,result))return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,result,{0},{0},target,{0}},arguments,2,output);
}
