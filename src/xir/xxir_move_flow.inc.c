/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_move_flow.inc.c - Availability of actual consumed owners across the CFG
 *
 * KEY CONCEPT:
 *   Successful takes clear one owner. Committed child arguments stay consumed
 *   on panic edges; whole-Cell writes restore only that Cell's own generation.
 */
static bool move_call_parameter(const XrXirModule *module,const XrXirFunction *function,
    const XrXirInstruction *op,uint32_t argument) {
    if(op->op==XR_XIR_CALL||op->op==XR_XIR_INVOKE||xr_xir_op_is_spawn(op->op)) {
        uint32_t prefix=op->op==XR_XIR_SCOPED_GO?1u:0u;
        if(argument<prefix)return false;
        argument-=prefix;
        const XrXirFunction *callee=&module->functions[op->immediate];
        return xr_xir_declared_parameter_mode(module->types,callee->parameters[argument],
            callee->consume_parameters,callee->consume_parameter_count,argument)==XR_PARAM_MOVE;
    }
    if(op->op==XR_XIR_CALL_INDIRECT||op->op==XR_XIR_INVOKE_INDIRECT) {
        const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,
            xr_xir_operand_type(function,(uint32_t)op->immediate));
        return signature&&argument<signature->parameter_count&&signature->parameters[argument].mode==XR_PARAM_MOVE;
    }
    return false;
}
static bool move_consumes(const XrXirModule *module,const XrXirFunction *function,
    const XrXirInstruction *op,uint32_t argument) {
    return (argument==1&&op->op==XR_XIR_CELL_WRITE_TAKE)||
        (!argument&&(op->op==XR_XIR_CELL_TAKE||op->op==XR_XIR_VALUE_TAKE||op->op==XR_XIR_CELL_NEW_TAKE))||
        move_call_parameter(module,function,op,argument);
}
static uint8_t move_after(const InitializationNode *node,uint32_t value,bool cell,
    const uint8_t *aliases,const XrXirModule *module) {
    uint8_t state=node->state;const XrXirInstruction *op=node->op;
    if(!op)return state;
    if(node->function->parameter_count+node->instruction==value)
        state=cell?(uint8_t)(1u|(state&4u)|((state&2u)?4u:0u)):1u;
    if(cell&&(op->op==XR_XIR_CELL_WRITE||op->op==XR_XIR_CELL_WRITE_TAKE||op->op==XR_XIR_CELL_LOCAL_WRITE)&&
        (aliases[op->args[0]]&1))state=(uint8_t)(1u|(state&4u));
    uint32_t count=operand_count(node->function,op,module);
    for(uint32_t a=0;a<count;++a) {
        uint32_t id=xr_xir_op_uses_operand_table(op->op)?node->function->operands[op->args[0]+a]:op->args[a];
        if(move_consumes(module,node->function,op,a)&&(cell?(aliases[id]&1)!=0:id==value))
            state=cell?(uint8_t)(2u|(state&4u)):0u;
    }
    return state;
}
/* A child can panic after argument commit. No handler may depend on the caller
 * retaining a consumed call argument, even if another preparation can fail. */
static uint8_t move_failed(const InitializationNode *node,uint32_t value,bool cell,
    const uint8_t *aliases,const XrXirModule *module) {
    if(!node->op)return node->state;
    const XrXirInstruction *op=node->op;uint8_t state=node->state;
    for(uint32_t a=0;a<operand_count(node->function,op,module);++a) {
        if(!move_call_parameter(module,node->function,op,a))continue;
        uint32_t id=node->function->operands[op->args[0]+a];
        if(cell?(aliases[id]&1)!=0:id==value)state=cell?(uint8_t)(2u|(state&4u)):0u;
    }
    return state;
}
/* Identity and contained-reference bits are distinct: reading a Cell takes a
 * logical value snapshot, while a closure inside that value can still capture
 * the Cell itself. Monotone bits describe references, never NoAlias permission. */
static void move_aliases(const XrXirModule *module,const XrXirFunction *function,
    uint32_t cell,uint8_t *aliases) {
    aliases[cell]=1;bool changed=true;
    while(changed) {
        changed=false;
        for(uint32_t i=0;i<function->instruction_count;++i) {
            const XrXirInstruction *op=&function->instructions[i];
            uint32_t destination=function->parameter_count+i;uint8_t bits=0;
            uint32_t count=operand_count(function,op,module);
            if(op->op==XR_XIR_PHI) {
                for(uint32_t p=0;p<count;p+=2)bits|=aliases[function->operands[op->args[0]+p+1]];
            } else for(uint32_t p=0;p<count;++p) {
                uint32_t id=xr_xir_op_uses_operand_table(op->op)?function->operands[op->args[0]+p]:op->args[p];
                bits|=aliases[id];
            }
            if(op->op==XR_XIR_CELL_WRITE||op->op==XR_XIR_CELL_WRITE_TAKE||op->op==XR_XIR_CELL_LOCAL_WRITE) {
                uint8_t stored=count>1&&aliases[op->args[1]]?2u:0u;
                uint32_t owner=op->args[0];
                if((aliases[owner]|stored)!=aliases[owner]){aliases[owner]|=stored;changed=true;}
            }
            if(xr_xir_place_kind(function,destination)!=XR_XIR_PLACE_NONE)bits=bits?1u:0u;
            else if(op->type==XR_XIR_UNIT||op->type==XR_XIR_STRING||scalar(op->type))bits=0;
            else if(op->op==XR_XIR_CELL_READ||op->op==XR_XIR_CELL_TAKE)bits&=2u;
            else if(op->op!=XR_XIR_COPY&&op->op!=XR_XIR_OWNED_RETAIN&&op->op!=XR_XIR_VALUE_TAKE&&
                op->op!=XR_XIR_PHI&&bits)bits=2;
            if((aliases[destination]|bits)!=aliases[destination]){aliases[destination]|=bits;changed=true;}
        }
    }
}
static XrXirStatus move_source(const XrXirFunction *function,uint32_t value,bool cell,
    VerifyContext *context) {
    XrXirType type=xr_xir_operand_type(function,value);uint32_t flags=0;
    if(cell) {
        if(value<function->parameter_count)return XR_XIR_BAD_VALUE;
        XrXirOp producer=function->instructions[value-function->parameter_count].op;
        if(producer!=XR_XIR_CELL_NEW&&producer!=XR_XIR_CELL_NEW_TAKE)return XR_XIR_BAD_VALUE;
        type=xr_xir_cell_element(context->module->types,type);
    } else {
        if(xr_xir_type_is_cell(context->module->types,type)||xr_xir_place_kind(function,value)!=XR_XIR_PLACE_NONE)
            return XR_XIR_BAD_VALUE;
        if(value<function->parameter_count&&xr_xir_declared_parameter_mode(context->module->types,type,
            function->consume_parameters,function->consume_parameter_count,value)!=XR_PARAM_MOVE)return XR_XIR_BAD_VALUE;
    }
    XrXirStatus status=xr_xir_compile_view_type_flags(&context->remaining,context->module->types,type,&flags);
    return status!=XR_XIR_OK?status:flags?XR_XIR_BAD_TYPE:XR_XIR_OK;
}
static XrXirStatus move_availability(InitializationGraph *graph,uint32_t value,bool cell,
    uint8_t *aliases,VerifyContext *context) {
    const XrXirFunction *function=graph->views[0].function;
    uint8_t entry=value<function->parameter_count;
    for(uint32_t n=0;n<graph->node_count;++n)graph->nodes[n].state=1;
    bool changed=true;
    while(changed) {
        changed=false;
        for(uint32_t n=0;n<graph->node_count;++n) {
            uint8_t state=n?1u:entry;
            for(uint32_t e=graph->nodes[n].head;e!=UINT32_MAX;e=graph->edges[e].next) {
                const InitializationEdge *edge=&graph->edges[e];
                const InitializationNode *source=&graph->nodes[edge->source];
                uint8_t predecessor=edge->transfer?move_after(source,value,cell,aliases,context->module):
                    move_failed(source,value,cell,aliases,context->module);
                /* Bit zero is must-live. May-empty captures of the current and
                 * earlier generations join separately, so a loop's fresh Cell
                 * cannot revive a closure that retained its empty predecessor. */
                state=(uint8_t)((state&predecessor&1u)|((state|predecessor)&6u));
            }
            if(state!=graph->nodes[n].state){graph->nodes[n].state=state;changed=true;}
        }
    }
    for(uint32_t n=0;n<graph->node_count;++n) {
        const InitializationNode *node=&graph->nodes[n];const XrXirInstruction *op=node->op;
        if(!op)continue;
        context->location.instruction=node->instruction;context->location.block=node->block;
        bool write=cell&&(op->op==XR_XIR_CELL_WRITE||op->op==XR_XIR_CELL_WRITE_TAKE||op->op==XR_XIR_CELL_LOCAL_WRITE);
        bool executing=op->op==XR_XIR_CALL||op->op==XR_XIR_INVOKE||op->op==XR_XIR_CALL_INDIRECT||
            op->op==XR_XIR_INVOKE_INDIRECT||xr_xir_op_is_spawn(op->op)||op->op==XR_XIR_GENERATOR_NEW;
        bool available=(node->state&1u)!=0,captured_unavailable=!available||(node->state&6u)!=0;
        if(cell&&captured_unavailable&&(op->op==XR_XIR_CALL_INDIRECT||op->op==XR_XIR_INVOKE_INDIRECT)&&
            aliases[(uint32_t)op->immediate])return XR_XIR_BAD_VALUE;
        if(cell&&op->op==XR_XIR_CELL_TAKE&&(aliases[op->args[0]]&1)) {
            uint32_t frontier=function->blocks[node->block].frontier;
            while(frontier) {
                const XrXirInstruction *cleanup=&function->instructions[frontier-1];
                for(uint32_t p=0;p<cleanup->args[1];++p)
                    if(aliases[function->operands[cleanup->args[0]+p]])return XR_XIR_BAD_VALUE;
                frontier=function->blocks[graph->nodes[frontier-1].block].frontier;
            }
        }
        uint32_t count=operand_count(function,op,context->module);
        for(uint32_t a=0;a<count;++a) {
            if(op->op==XR_XIR_PHI) {
                uint32_t predecessor=function->operands[op->args[0]+a],id=function->operands[op->args[0]+a+1];++a;
                if(cell?!(aliases[id]&1):id!=value)continue;
                uint32_t last=function->blocks[predecessor].first+function->blocks[predecessor].count-1;
                if(!(move_after(&graph->nodes[last],value,cell,aliases,context->module)&1u))return XR_XIR_BAD_VALUE;
                continue;
            }
            uint32_t id=xr_xir_op_uses_operand_table(op->op)?function->operands[op->args[0]+a]:op->args[a];
            if(write&&!a)continue;
            if(cell?(((aliases[id]&1)&&!available)||
                (executing&&(aliases[id]&2)&&captured_unavailable)):(!available&&id==value))return XR_XIR_BAD_VALUE;
            if(move_consumes(context->module,function,op,a))
                for(uint32_t b=0;b<a;++b) {
                    uint32_t other=xr_xir_op_uses_operand_table(op->op)?function->operands[op->args[0]+b]:op->args[b];
                    if(move_consumes(context->module,function,op,b)&&id==other)return XR_XIR_BAD_VALUE;
                }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus move_check(const XrXirFunction *function,VerifyContext *context) {
    InitializationGraph graph={0};uint8_t *aliases=NULL;XrXirStatus status=XR_XIR_OK;
    uint32_t values=function->parameter_count+function->instruction_count;
    for(uint32_t i=0;i<function->instruction_count&&status==XR_XIR_OK;++i) {
        const XrXirInstruction *op=&function->instructions[i];uint32_t count=operand_count(function,op,context->module);
        for(uint32_t a=0;a<count&&status==XR_XIR_OK;++a) {
            if(!move_consumes(context->module,function,op,a))continue;
            uint32_t value=xr_xir_op_uses_operand_table(op->op)?function->operands[op->args[0]+a]:op->args[a];
            bool cell=op->op==XR_XIR_CELL_TAKE;
            status=move_source(function,value,cell,context);
            if(status!=XR_XIR_OK)break;
            if(!graph.nodes) {
                status=initialization_allocate(&graph,function,NULL,&context->remaining);
                if(status==XR_XIR_OK)status=initialization_connect(&graph);
                if(status==XR_XIR_OK)aliases=xir_compile_calloc(&context->remaining,values,sizeof(*aliases),&status);
            }
            if(status==XR_XIR_OK) {
                memset(aliases,0,values);if(cell)move_aliases(context->module,function,value,aliases);
                status=move_availability(&graph,value,cell,aliases,context);
            }
        }
    }
    xr_compile_resources_free(aliases);initialization_free(&graph);return status;
}
