/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_handler_owner.inc.c - Own bounded error selector observations
 */
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
static XrXirStatus source_selector_validate(SourceQueryCopy *copy,const XrXirFunction *body,
    const XrXirSourceDebugHandler *handler,const XrXirSourceDebugSelector *selector) {
    for (uint32_t i=selector->first;i<selector->accepted;++i) {
        if (!query_work(copy,sizeof(XrXirInstruction))) return copy->status;
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
                if (!query_work(copy,2)) return copy->status;
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
static XrXirStatus source_debug_handler_validate(SourceQueryCopy *copy,
    const XrXirSourceDebugFunction *function,const XrXirFunction *body) {
    if (!!function->handler_count!=!!function->handlers || !!function->route_count!=!!function->routes ||
        function->handler_count>body->instruction_count || function->route_count>body->instruction_count ||
        ((function->handler_count || function->route_count) && (!function->points || !body->instructions))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t h=0;h<function->handler_count;++h) {
        if (!query_work(copy,sizeof(*function->handlers))) return copy->status;
        const XrXirSourceDebugHandler *handler=&function->handlers[h];
        if (!handler->selector_count || !handler->selectors || handler->selector_count>body->instruction_count ||
            handler->error<body->parameter_count || handler->error-body->parameter_count>=body->instruction_count ||
            (handler->parent && (handler->parent<=h+1 || handler->parent>function->handler_count))) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *input=&body->instructions[handler->error-body->parameter_count];
        if (input->op!=XR_XIR_PHI || input->type!=XR_XIR_ERROR) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t s=0;s<handler->selector_count;++s) {
            if (!query_work(copy,sizeof(*handler->selectors))) return copy->status;
            const XrXirSourceDebugSelector *selector=&handler->selectors[s];
            if (selector->first<=handler->error-body->parameter_count || selector->first>selector->accepted ||
                selector->accepted>=body->instruction_count ||
                !(function->points[selector->accepted].flags&XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT) ||
                (selector->rejected!=UINT32_MAX && (selector->rejected<=selector->accepted || selector->rejected>=body->instruction_count)) ||
                (s && handler->selectors[s-1].rejected!=selector->first)) return XR_XIR_BAD_STRUCTURE;
            if (selector->rejected==UINT32_MAX) {
                if (s+1!=handler->selector_count || selector->first!=selector->accepted ||
                    handler->parent || handler->propagation!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            } else {
                const XrXirInstruction *test=&body->instructions[selector->first];
                if (test->op!=XR_XIR_ERROR_IS || test->args[0]!=handler->error) return XR_XIR_BAD_STRUCTURE;
            }
            XrXirStatus status=source_selector_validate(copy,body,handler,selector);if (status!=XR_XIR_OK) return status;
        }
        uint32_t rejected=handler->selectors[handler->selector_count-1].rejected;
        if (rejected!=handler->propagation) return XR_XIR_BAD_STRUCTURE;
        if (rejected!=UINT32_MAX) {
            const XrXirInstruction *op=&body->instructions[rejected];
            if (handler->parent) {
                if (op->op!=XR_XIR_JUMP && op->op!=XR_XIR_CLEANUP_ERROR) return XR_XIR_BAD_STRUCTURE;
                const XrXirSourceDebugHandler *parent=&function->handlers[handler->parent-1];
                if (!body->blocks || op->targets[0]>=body->block_count || parent->error<body->parameter_count ||
                    body->blocks[op->targets[0]].first!=parent->error-body->parameter_count) return XR_XIR_BAD_STRUCTURE;
            } else if (op->op!=XR_XIR_THROW) return XR_XIR_BAD_STRUCTURE;
            if (op->op!=XR_XIR_JUMP && op->args[0]!=handler->error) return XR_XIR_BAD_STRUCTURE;
        }
    }
    for (uint32_t r=0;r<function->route_count;++r) {
        if (!query_work(copy,sizeof(*function->routes))) return copy->status;
        const XrXirSourceDebugRoute *route=&function->routes[r];
        if (route->instruction>=body->instruction_count || !route->handler || route->handler>function->handler_count ||
            (r && function->routes[r-1].instruction>=route->instruction)) return XR_XIR_BAD_STRUCTURE;
        XrXirOp op=body->instructions[route->instruction].op;
        bool invocation=op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT || op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_TASK_AWAIT;
        bool thrown=false;uint32_t low=0,high=function->throw_count;
        while (!invocation && low<high) {
            if (!query_work(copy,1)) return copy->status;
            uint32_t middle=low+(high-low)/2,position=function->throw_sites[middle].instruction;
            if (position<route->instruction) low=middle+1;
            else if (position>route->instruction) high=middle;
            else { thrown=true;break; }
        }
        if (!invocation && !thrown) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static void source_debug_handler_copy(SourceQueryCopy *copy,const XrXirSourceDebugFunction *input,
    XrXirSourceDebugFunction *output) {
    if (input->route_count) output->routes=query_copy(copy,input->routes,input->route_count,sizeof(*input->routes));
    if (!input->handler_count) return;
    XrXirSourceDebugHandler *handlers=query_copy(copy,input->handlers,input->handler_count,sizeof(*handlers));
    output->handlers=handlers;
    for (uint32_t h=0;handlers && h<input->handler_count && copy->status==XR_XIR_OK;++h)
        handlers[h].selectors=query_copy(copy,input->handlers[h].selectors,input->handlers[h].selector_count,sizeof(*handlers[h].selectors));
}
