/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_initialization.inc.c - Shared initialization flow for executable and detached regions
 *
 * KEY CONCEPT:
 *   Detached entries inherit a checkpoint without adding executable control flow.
 *   A panic edge leaves every instruction of a protected block before that
 *   instruction's own effect, so handlers see only completed writes.
 */
typedef struct InitializationNode {
    const XrXirFunction *function;
    const XrXirInstruction *op;
    uint32_t head, instruction, block;
    uint8_t state;
} InitializationNode;
typedef struct InitializationEdge { uint32_t source, next; bool transfer; } InitializationEdge;
typedef struct InitializationView {
    const XrXirFunction *function;
    const XrXirInitializationRegion *region;
    uint32_t first, offset;
} InitializationView;
typedef struct InitializationGraph {
    InitializationView *views;
    InitializationNode *nodes;
    InitializationEdge *edges;
    uint32_t view_count, node_count, edge_count;
} InitializationGraph;

static void initialization_free(InitializationGraph *graph) {
    xr_compile_resources_free(graph->views); xr_compile_resources_free(graph->nodes); xr_compile_resources_free(graph->edges);
}
static void initialization_edge(InitializationGraph *graph, uint32_t source, uint32_t target, bool transfer) {
    uint32_t index = graph->edge_count++;
    graph->edges[index] = (InitializationEdge){source,graph->nodes[target].head,transfer};
    graph->nodes[target].head = index;
}
static XrXirStatus initialization_allocate(InitializationGraph *graph, const XrXirFunction *function,
    const XrXirInitializationRegion *regions, XrXirCompileContext *remaining) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint64_t views = 1, nodes = (uint64_t)function->instruction_count+1;
    for (const XrXirInitializationRegion *r=regions; r; r=r->next) {
        if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
        if (r->first_instruction > r->function.instruction_count || r->first_block > r->function.block_count)
            return XR_XIR_BAD_STRUCTURE;
        ++views; nodes += (uint64_t)r->function.instruction_count-r->first_instruction+1;
        if (views>UINT32_MAX || nodes>UINT32_MAX) return XR_XIR_BUDGET;
    }
    uint64_t edges = nodes*3+views;
    uint64_t bytes = views*sizeof(*graph->views)+nodes*sizeof(*graph->nodes)+edges*sizeof(*graph->edges);
    if (views>UINT32_MAX || nodes>UINT32_MAX || edges>UINT32_MAX || bytes>SIZE_MAX ||
        !(bytes <= SIZE_MAX)) return XR_XIR_BUDGET;
    graph->views=xir_compile_calloc(remaining, (size_t)views,sizeof(*graph->views), &allocation_status);
    graph->nodes=xir_compile_calloc(remaining, (size_t)nodes,sizeof(*graph->nodes), &allocation_status);
    graph->edges=xir_compile_calloc(remaining, (size_t)edges,sizeof(*graph->edges), &allocation_status);
    if (!graph->views || !graph->nodes || !graph->edges) return allocation_status;
    graph->view_count=(uint32_t)views; graph->node_count=(uint32_t)nodes;
    graph->views[0]=(InitializationView){function,NULL,0,0};
    uint32_t index=1, offset=function->instruction_count+1;
    for (const XrXirInitializationRegion *r=regions; r; r=r->next) {
        graph->views[index++]=(InitializationView){&r->function,r,r->first_instruction,offset};
        offset+=r->function.instruction_count-r->first_instruction+1;
    }
    for (uint32_t n=0;n<graph->node_count;++n) graph->nodes[n].head=UINT32_MAX;
    return XR_XIR_OK;
}
static XrXirStatus initialization_connect(InitializationGraph *graph, XrXirCompileContext *remaining) {
    for (uint32_t v=0;v<graph->view_count;++v) {
        const InitializationView *view=&graph->views[v];
        const XrXirFunction *function=view->function;
        uint32_t owner=view->region ? view->region->first_block : 0;
        for (uint32_t i=view->first;i<function->instruction_count;++i) {
            if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&function->instructions[i];
            if (!op->op || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            uint32_t node=view->offset+i-view->first;
            while (owner+1<function->block_count && function->blocks[owner+1].first<=i) ++owner;
            graph->nodes[node].block=owner;
            graph->nodes[node].function=function; graph->nodes[node].op=op; graph->nodes[node].instruction=i;
            if (!op_rules[op->op].terminal) initialization_edge(graph,node,node+1,true);
            for (uint32_t e=0;e<op_rules[op->op].edges;++e) {
                uint32_t block=op->targets[e];
                if (view->region && block<view->region->first_block) continue;
                if (block>=function->block_count) return XR_XIR_BAD_STRUCTURE;
                uint32_t target=function->blocks[block].first;
                if (target<view->first || target>function->instruction_count ||
                    (!view->region && target==function->instruction_count)) return XR_XIR_BAD_STRUCTURE;
                initialization_edge(graph,node,view->offset+target-view->first,true);
            }
            uint32_t handler=function->blocks[owner].panic;
            if (handler && !(view->region && handler<view->region->first_block)) {
                if (handler>=function->block_count) return XR_XIR_BAD_STRUCTURE;
                uint32_t target=function->blocks[handler].first;
                if (target<view->first || target>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
                initialization_edge(graph,node,view->offset+target-view->first,false);
            }
        }
        if (!view->region) continue;
        const InitializationView *parent=NULL;
        for (uint32_t p=0;p<graph->view_count;++p) {
            if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
            if (graph->views[p].region==view->region->parent) { parent=&graph->views[p]; break; }
        }
        uint32_t checkpoint=view->region->checkpoint;
        if (!parent || checkpoint<parent->first || checkpoint>parent->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        initialization_edge(graph,parent->offset+checkpoint-parent->first,view->offset,false);
    }
    return XR_XIR_OK;
}
static XrXirStatus initialization_solve(InitializationGraph *graph, uint32_t declaration,
    uint32_t place, XrXirCompileContext *remaining) {
    for (uint32_t n=0;n<graph->node_count;++n) graph->nodes[n].state=1;
    bool changed=true;
    while (changed) {
        changed=false;
        for (uint32_t n=0;n<graph->node_count;++n) {
            if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
            uint8_t state=n ? 1 : 0;
            for (uint32_t e=graph->nodes[n].head;e!=UINT32_MAX;e=graph->edges[e].next) {
                if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
                const InitializationEdge *edge=&graph->edges[e];
                const InitializationNode *source=&graph->nodes[edge->source];
                uint8_t incoming=source->state;
                if (edge->transfer && source->op) {
                    if (source->instruction==declaration && source->op->op==XR_XIR_LOCAL_UNINIT) incoming=0;
                    if (local_write(source->op->op) && source->op->args[0]==place) incoming=3;
                }
                state=(uint8_t)((state&incoming&1)|((state|incoming)&2));
            }
            if (graph->nodes[n].state!=state) {graph->nodes[n].state=state;changed=true;}
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus initialization_reads(const InitializationGraph *graph, uint32_t place,
    bool readonly, VerifyContext *context) {
    for (uint32_t n=0;n<graph->node_count;++n) {
        if (!xir_compile_work(&context->remaining,1)) return XR_XIR_BUDGET;
        const InitializationNode *node=&graph->nodes[n];
        const XrXirInstruction *op=node->op;
        if (!op) continue;
        context->location.instruction=node->instruction; context->location.block=node->block;
        if (local_write(op->op) && op->args[0]==place) {
            if (readonly && (node->state&2)) {
                context->location.reason=XR_XIR_DIAGNOSTIC_READONLY_WRITE;
                return XR_XIR_BAD_VALUE;
            }
            continue;
        }
        if (op->op==XR_XIR_PHI) continue;
        uint32_t count=operand_count(node->function,op,context->module);
        if (!xir_compile_work(&context->remaining,count)) return XR_XIR_BUDGET;
        for (uint32_t a=0;a<count;++a) {
            uint32_t id=xr_xir_op_uses_operand_table(op->op) ? node->function->operands[op->args[0]+a] : op->args[a];
            if (!a && (op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_INDEX_PLACE)) continue;
            for (;;) {
                XrXirPlaceKind kind=xr_xir_place_kind(node->function,id);
                if (kind!=XR_XIR_PLACE_FIELD && kind!=XR_XIR_PLACE_INDEX) break;
                if (!xir_compile_work(&context->remaining,1)) return XR_XIR_BUDGET;
                id=node->function->instructions[id-node->function->parameter_count].args[0];
            }
            if (id!=place) continue;
            if (!(node->state&1)) {
                context->location.reason=XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ;
                return XR_XIR_BAD_VALUE;
            }
            if (readonly && xr_xir_operand_role(op->op,a)==XR_XIR_OPERAND_WRITE) {
                context->location.reason=XR_XIR_DIAGNOSTIC_READONLY_WRITE;
                return XR_XIR_BAD_VALUE;
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus initialization_check(const XrXirFunction *function,
    const XrXirInitializationRegion *regions, VerifyContext *context) {
    InitializationGraph graph={0}; XrXirStatus status=XR_XIR_OK;
    for (uint32_t i=0;i<function->instruction_count && status==XR_XIR_OK;++i) {
        if (!xir_compile_work(&context->remaining,1)) {status=XR_XIR_BUDGET;break;}
        if (function->instructions[i].op!=XR_XIR_LOCAL_UNINIT) continue;
        if (!graph.nodes) {
            status=initialization_allocate(&graph,function,regions,&context->remaining);
            if (status==XR_XIR_OK) status=initialization_connect(&graph,&context->remaining);
        }
        uint32_t place=function->parameter_count+i;
        if (status==XR_XIR_OK) status=initialization_solve(&graph,i,place,&context->remaining);
        if (status==XR_XIR_OK) status=initialization_reads(&graph,place,function->instructions[i].immediate!=0,context);
    }
    initialization_free(&graph); return status;
}
XrXirStatus xr_xir_compile_initialization_check(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, const XrXirInitializationRegion *regions, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (diagnostic) *diagnostic=(XrXirDiagnostic){XR_XIR_BAD_STRUCTURE,function,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    if (!module || !module->functions || function>=module->function_count || !remaining) return XR_XIR_BAD_STRUCTURE;
    VerifyContext context={*remaining,{XR_XIR_OK,function,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE},module};
    XrXirStatus status=initialization_check(&module->functions[function],regions,&context);
    *remaining=context.remaining;
    context.location.status=status;
    if (diagnostic) *diagnostic=context.location;
    return status;
}
