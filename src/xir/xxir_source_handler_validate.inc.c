/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_handler_validate.inc.c - One structural boundary for selector observations
 */
#ifndef XXIR_SOURCE_HANDLER_VALIDATE_INC_C
#define XXIR_SOURCE_HANDLER_VALIDATE_INC_C
static bool source_handler_validation_work(XrCompileResources *resources,uint64_t work) {
    return xr_compile_resources_work(resources,work)==XR_COMPILE_RESOURCE_OK;
}
static bool source_selector_value(const XrXirFunction *body,const XrXirSourceDebugHandler *handler,
    const XrXirSourceDebugSelector *selector,uint32_t instruction,uint32_t value) {
    return value==handler->error || (value>=body->parameter_count &&
        value-body->parameter_count>=selector->first && value-body->parameter_count<instruction);
}
static bool source_selector_edge(const XrXirFunction *body,const XrXirSourceDebugSelector *selector,
    uint32_t instruction,uint32_t block) {
    if (!body->blocks || block>=body->block_count) return false;
    uint32_t target=body->blocks[block].first;
    return target>instruction && ((target>=selector->first && target<=selector->accepted) || target==selector->rejected);
}
static XrXirStatus source_selector_validate(XrCompileResources *resources,const XrXirFunction *body,
    const XrXirSourceDebugHandler *handler,const XrXirSourceDebugSelector *selector) {
    for (uint32_t i=selector->first;i<selector->accepted;++i) {
        if (!source_handler_validation_work(resources,sizeof(XrXirInstruction))) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&body->instructions[i];uint32_t values=0;
        switch (op->op) {
        case XR_XIR_CONST_INT:case XR_XIR_CONST_FLOAT:case XR_XIR_CONST_BOOL:
        case XR_XIR_CONST_RUNE:case XR_XIR_CONST_STRING:break;
        case XR_XIR_ERROR_IS:case XR_XIR_ERROR_NARROW:
            if (op->args[0]!=handler->error) return XR_XIR_BAD_STRUCTURE;
            values=1;break;
        case XR_XIR_ENUM_TAG:case XR_XIR_ENUM_GET:values=1;break;
        case XR_XIR_EQ_INT:case XR_XIR_LE_INT:case XR_XIR_GE_INT:case XR_XIR_EQUAL:values=2;break;
        case XR_XIR_BRANCH:
            if (!source_selector_edge(body,selector,i,op->targets[1])) return XR_XIR_BAD_STRUCTURE;
            values=1;
            /* Both outgoing edges must remain in this explicit selector. */
            /* fall through */
        case XR_XIR_JUMP:
            if (!source_selector_edge(body,selector,i,op->targets[0])) return XR_XIR_BAD_STRUCTURE;
            break;
        case XR_XIR_PHI:
            if (!op->args[1] || op->args[1]%2 || !body->operands || !body->blocks ||
                op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t a=0;a<op->args[1];a+=2) {
                if (!source_handler_validation_work(resources,2)) return XR_XIR_BUDGET;
                uint32_t block=body->operands[op->args[0]+a],value=body->operands[op->args[0]+a+1];
                if (block>=body->block_count || body->blocks[block].first<selector->first || body->blocks[block].first>=i ||
                    !source_selector_value(body,handler,selector,i,value)) return XR_XIR_BAD_STRUCTURE;
            }
            break;
        default:return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t a=0;a<values;++a)
            if (!source_selector_value(body,handler,selector,i,op->args[a])) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus source_handler_edge_validate(XrCompileResources *resources,const XrXirFunction *body,
    const XrXirSourceDebugHandler *handler,uint32_t edge,uint32_t error,uint32_t input) {
    if (edge>=body->instruction_count || (uint64_t)error>=(uint64_t)body->parameter_count+body->instruction_count ||
        (error<body->parameter_count && !body->parameters) || xr_xir_operand_type(body,error)!=XR_XIR_ERROR ||
        handler->error<body->parameter_count || handler->error-body->parameter_count>=body->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&body->instructions[edge],*phi=&body->instructions[handler->error-body->parameter_count];
    if ((op->op!=XR_XIR_JUMP && op->op!=XR_XIR_CLEANUP_ERROR) ||
        (op->op==XR_XIR_CLEANUP_ERROR && op->args[0]!=error) || !body->blocks ||
        op->targets[0]>=body->block_count || body->blocks[op->targets[0]].first!=handler->error-body->parameter_count ||
        phi->op!=XR_XIR_PHI || phi->type!=XR_XIR_ERROR || !phi->args[1] || phi->args[1]%2 ||
        !body->operands || phi->args[0]>body->operand_count || phi->args[1]>body->operand_count-phi->args[0] ||
        input>=phi->args[1]/2) return XR_XIR_BAD_STRUCTURE;
    if (!source_handler_validation_work(resources,2)) return XR_XIR_BUDGET;
    uint32_t at=phi->args[0]+input*2,block=body->operands[at];
    if (body->operands[at+1]!=error || block>=body->block_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirBlock *predecessor=&body->blocks[block];
    return predecessor->count && (uint64_t)predecessor->first+predecessor->count-1==edge ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus source_debug_handler_validate(XrCompileResources *resources,
    const XrXirSourceDebugFunction *function,const XrXirFunction *body) {
    if (!!function->handler_count!=!!function->handlers || !!function->route_count!=!!function->routes ||
        function->handler_count>body->instruction_count || function->route_count>body->instruction_count ||
        ((function->handler_count || function->route_count) && (!function->points || !body->instructions))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t h=0;h<function->handler_count;++h) {
        if (!source_handler_validation_work(resources,sizeof(*function->handlers))) return XR_XIR_BUDGET;
        const XrXirSourceDebugHandler *handler=&function->handlers[h];
        if (!handler->selector_count || !handler->selectors || handler->selector_count>body->instruction_count ||
            handler->error<body->parameter_count || handler->error-body->parameter_count>=body->instruction_count ||
            (handler->parent && (handler->parent<=h+1 || handler->parent>function->handler_count))) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *input=&body->instructions[handler->error-body->parameter_count];
        if (input->op!=XR_XIR_PHI || input->type!=XR_XIR_ERROR) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t s=0;s<handler->selector_count;++s) {
            if (!source_handler_validation_work(resources,sizeof(*handler->selectors))) return XR_XIR_BUDGET;
            const XrXirSourceDebugSelector *selector=&handler->selectors[s];
            if (selector->first<=handler->error-body->parameter_count || selector->first>selector->accepted ||
                selector->accepted>=body->instruction_count ||
                !(function->points[selector->accepted].flags&XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT) ||
                (selector->rejected!=UINT32_MAX && (selector->rejected<=selector->accepted || selector->rejected>=body->instruction_count)) ||
                (s && handler->selectors[s-1].rejected!=selector->first)) return XR_XIR_BAD_STRUCTURE;
            if (selector->rejected==UINT32_MAX) {
                if (s+1!=handler->selector_count || selector->first!=selector->accepted ||
                    handler->parent || handler->propagation!=UINT32_MAX || handler->propagation_input!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            } else {
                const XrXirInstruction *test=&body->instructions[selector->first];
                if (test->op!=XR_XIR_ERROR_IS || test->args[0]!=handler->error) return XR_XIR_BAD_STRUCTURE;
            }
            XrXirStatus status=source_selector_validate(resources,body,handler,selector);if (status!=XR_XIR_OK) return status;
        }
        uint32_t rejected=handler->selectors[handler->selector_count-1].rejected;
        if (rejected!=handler->propagation) return XR_XIR_BAD_STRUCTURE;
        if (rejected!=UINT32_MAX) {
            const XrXirInstruction *op=&body->instructions[rejected];
            if (handler->parent) {
                if (op->op!=XR_XIR_JUMP && op->op!=XR_XIR_CLEANUP_ERROR) return XR_XIR_BAD_STRUCTURE;
                XrXirStatus status=source_handler_edge_validate(resources,body,&function->handlers[handler->parent-1],rejected,handler->error,handler->propagation_input);
                if (status!=XR_XIR_OK) return status;
            } else if (op->op!=XR_XIR_THROW || handler->propagation_input!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            if (op->op!=XR_XIR_JUMP && op->args[0]!=handler->error) return XR_XIR_BAD_STRUCTURE;
        }
    }
    for (uint32_t r=0;r<function->route_count;++r) {
        if (!source_handler_validation_work(resources,sizeof(*function->routes))) return XR_XIR_BUDGET;
        const XrXirSourceDebugRoute *route=&function->routes[r];
        if (route->instruction>=body->instruction_count || !route->handler || route->handler>function->handler_count ||
            (r && function->routes[r-1].instruction>=route->instruction)) return XR_XIR_BAD_STRUCTURE;
        XrXirOp op=body->instructions[route->instruction].op;
        bool invocation=op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT || op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_TASK_AWAIT;
        bool thrown=false;uint32_t low=0,high=function->throw_count;
        while (!invocation && low<high) {
            if (!source_handler_validation_work(resources,1)) return XR_XIR_BUDGET;
            uint32_t middle=low+(high-low)/2,position=function->throw_sites[middle].instruction;
            if (position<route->instruction) low=middle+1;
            else if (position>route->instruction) high=middle;
            else { thrown=function->throw_sites[middle].operand==route->error;break; }
        }
        if (!invocation && !thrown) return XR_XIR_BAD_STRUCTURE;
        if (invocation) {
            const XrXirInstruction *call=&body->instructions[route->instruction];
            if (!body->blocks || call->targets[1]>=body->block_count || route->error<body->parameter_count ||
                route->error-body->parameter_count>=body->instruction_count) return XR_XIR_BAD_STRUCTURE;
            const XrXirBlock *error=&body->blocks[call->targets[1]];
            const XrXirInstruction *value=&body->instructions[route->error-body->parameter_count];
            if (error->count!=2 || error->first!=route->error-body->parameter_count ||
                (uint64_t)error->first+1!=route->edge || value->op!=XR_XIR_INVOKE_ERROR ||
                value->immediate!=route->instruction) return XR_XIR_BAD_STRUCTURE;
        } else if (route->edge!=route->instruction) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=source_handler_edge_validate(resources,body,&function->handlers[route->handler-1],route->edge,route->error,route->input);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

#endif // XXIR_SOURCE_HANDLER_VALIDATE_INC_C
