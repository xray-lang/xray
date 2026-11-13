/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xxir_instruction_flow.h"
typedef struct SourceLexicalFlow {
    SourceContext *context;
    uint32_t function,count,throw_index;
    const SourceLexicalMaps *maps;
    XirLexicalFunction *directory;
    XirLexicalEvent *events;
} SourceLexicalFlow;

static uint32_t source_lexical_common_scope(const XirLexicalFunction *directory,
    uint32_t left,uint32_t right) {
    while (left!=right && left!=UINT32_MAX && right!=UINT32_MAX) {
        if (left>right) left=directory->scopes[left].parent;
        else right=directory->scopes[right].parent;
    }
    return left==right ? left : UINT32_MAX;
}

static XrXirStatus source_lexical_flow_edge(void *opaque,const XirInstructionFlowEdge *edge) {
    SourceLexicalFlow *flow=opaque;
    SourceContext *ctx=flow->context;
    const SourceFunction *body=&ctx->bodies[flow->function];
    const XrXirFunction *function=&ctx->functions[flow->function];
    const XrXirInstruction *op=&function->instructions[edge->source];
    bool panic=edge->kind==XIR_INSTRUCTION_FLOW_PANIC;
    bool error=edge->kind==XIR_INSTRUCTION_FLOW_TARGET &&
        (((op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT ||
           op->op==XR_XIR_INVOKE_DEFAULT) && edge->ordinal==1) ||
         op->op==XR_XIR_CLEANUP_ERROR || op->op==XR_XIR_THROW);
    uint32_t value=error && (op->op==XR_XIR_CLEANUP_ERROR || op->op==XR_XIR_THROW) ?
        op->args[0] : UINT32_MAX;
    if (!panic) {
        while (flow->throw_index<body->throw_count &&
            body->debug_throws[flow->throw_index].instruction<edge->source) ++flow->throw_index;
        if (flow->throw_index<body->throw_count &&
            body->debug_throws[flow->throw_index].instruction==edge->source) {
            error=true;value=body->debug_throws[flow->throw_index].operand;
        }
    }
    if (!panic && !error) return XR_XIR_OK;
    if (flow->count==UINT32_MAX) return XR_XIR_BUDGET;
    if (flow->events) {
        const SourceLexicalInstruction source=body->lexical_instructions[edge->source];
        if (source.scope>=flow->maps->scope_count || flow->maps->scopes[source.scope]==UINT32_MAX)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t scope=flow->maps->scopes[source.scope],target=UINT32_MAX,frontier=0;
        if (edge->target_block!=UINT32_MAX) {
            if (edge->target>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
            uint32_t actual=body->lexical_instructions[edge->target].scope;
            if (actual>=flow->maps->scope_count || flow->maps->scopes[actual]==UINT32_MAX)
                return XR_XIR_BAD_STRUCTURE;
            target=source_lexical_common_scope(flow->directory,scope,flow->maps->scopes[actual]);
            frontier=function->blocks[edge->target_block].frontier;
        }
        flow->events[flow->count]=(XirLexicalEvent){
            .block=edge->source_block,.instruction=edge->source,.ordinal=edge->ordinal,
            .scope=scope,.target=target,.binding=UINT32_MAX,.value=value,
            .frontier=function->blocks[edge->source_block].frontier,.target_frontier=frontier,
            .statement=source.statement,.parent_statement=source.parent_statement,
            .position=panic ? XIR_LEXICAL_PANIC_EDGE : XIR_LEXICAL_ERROR_EDGE,
            .kind=panic ? XIR_LEXICAL_PANIC : XIR_LEXICAL_THROW};
    }
    ++flow->count;
    return XR_XIR_OK;
}

static XrXirStatus source_lexical_flow(SourceLexicalFlow *flow) {
    const XrXirFunction *function=&flow->context->functions[flow->function];
    if (function->instruction_count && !flow->context->bodies[flow->function].lexical_instructions)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t b=0;b<function->block_count;++b) {
        XrXirBlock block=function->blocks[b];
        for (uint32_t i=block.first;i-block.first<block.count;++i) {
            if (!source_request_poll(flow->context,NULL)) return flow->context->diagnostic.status;
            XrXirStatus status=xr_xir_instruction_flow_visit(function,i,b,
                XIR_INSTRUCTION_FLOW_TARGET | XIR_INSTRUCTION_FLOW_PANIC,source_lexical_flow_edge,flow);
            if (status!=XR_XIR_OK) return status;
            XirInstructionFlowPoint point={function,&function->instructions[i],i,b};
            if (xr_xir_instruction_cleanup_exit(point.op->op)) {
                status=xr_xir_cleanup_panic_edges(&point,true,source_lexical_flow_edge,flow);
                if (status!=XR_XIR_OK) return status;
            } else if (!block.panic && xr_xir_instruction_has_panic_port(point.op->op)) {
                XirInstructionFlowEdge edge={i,UINT32_MAX,b,UINT32_MAX,0,XIR_INSTRUCTION_FLOW_PANIC};
                status=source_lexical_flow_edge(flow,&edge);
                if (status!=XR_XIR_OK) return status;
            }
            if (point.op->op==XR_XIR_THROW) {
                XirInstructionFlowEdge edge={i,UINT32_MAX,b,UINT32_MAX,0,XIR_INSTRUCTION_FLOW_TARGET};
                status=source_lexical_flow_edge(flow,&edge);
                if (status!=XR_XIR_OK) return status;
            }
        }
    }
    return XR_XIR_OK;
}
