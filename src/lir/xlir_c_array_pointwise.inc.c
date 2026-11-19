/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array_pointwise.inc.c - Private Array capacity segments over scalar CFG
 */
#include "xlir_array_scalar_region.h"

typedef struct LirCPointwiseNode {
    size_t predecessors, incoming;
    unsigned reach;
    bool without_push;
} LirCPointwiseNode;
typedef struct LirCPointwisePredecessor { size_t next; uint32_t block; } LirCPointwisePredecessor;

static const XrLirHomeEvent *c_pointwise_push_event(const XrLirValueHomes *homes) {
    const XrLirHomeEvent *push=NULL;
    for (size_t i=0; i<homes->event_count; ++i) {
        const XrLirHomeEvent *event=&homes->events[i];
        if (event->kind!=XR_LIR_OWNERSHIP_ARRAY_OPERATION || event->operation!=XR_LIR_ARRAY_OP_PUSH) continue;
        if (push) return NULL;
        push=event;
    }
    return push;
}

static const XrLirInstructionView *c_pointwise_end(const XrLirFunctionData *f, uint32_t block) {
    const XrLirBlockData *row=&f->blocks[block-1];
    return &row->instructions[row->instruction_count-1].view;
}

static void c_pointwise_reach(const XrLirFunctionData *f, LirCPointwiseNode *nodes,
    const LirCPointwisePredecessor *predecessors, uint32_t *work, uint32_t origin, bool reverse) {
    unsigned bit=reverse ? 2u : 1u;
    size_t count=1; work[0]=origin; nodes[origin-1].reach|=bit;
    while (count) {
        uint32_t block=work[--count];
        const XrLirInstructionView *term=c_pointwise_end(f,block);
        size_t edge=reverse ? nodes[block-1].predecessors : 0;
        while (reverse ? edge!=0 : edge<term->edge_count) {
            uint32_t target=reverse ? predecessors[edge].block : term->edges[edge].target;
            edge=reverse ? predecessors[edge].next : edge+1;
            if (!(nodes[target-1].reach&bit)) {
                nodes[target-1].reach|=bit;
                work[count++]=target;
            }
        }
    }
}

static bool c_pointwise_edge(const XrLirFunctionData *f, const XrLirValueHomes *homes,
    const XrLirEdgeView *edge, uint32_t owner) {
    const XrLirBlockData *target=&f->blocks[edge->target-1];
    bool found=false;
    for (uint32_t p=0; p<edge->argument_count; ++p) {
        uint32_t source=edge->arguments[p], destination=target->parameters[p];
        if (lir_array_region_scalar(f->values[source-1].type)) continue;
        if (found || homes->values[source].stationary_owner!=owner ||
            !xr_lir_home_stationary_edge(homes,source,destination)) return false;
        found=true;
    }
    return found;
}

/* The SCC contains exactly the blocks both reachable from and returning to
 * the append. One external entry and exit at the original header, followed
 * by a topological pass with the unique latch removed, close the segment. */
static bool c_pointwise_cfg(LirC *c, const XrLirFunctionData *f, LirCPointwiseNode *nodes,
    uint32_t *work, uint32_t owner, LirCPointwisePush *plan) {
    uint32_t entry=0;
    for (uint32_t b=1; b<=f->block_count && c_request_poll(c); ++b) {
        bool internal=nodes[b-1].reach==3;
        const XrLirInstructionView *term=c_pointwise_end(f,b);
        for (uint32_t e=0; e<term->edge_count; ++e) {
            uint32_t target=term->edges[e].target;
            bool destination=nodes[target-1].reach==3;
            if (internal && !destination) {
                if (plan->header || term->op!=XR_LIR_CONDBR) return false;
                plan->header=b; plan->body_on_true=e==1;
            } else if (!internal && destination) {
                if (entry) return false;
                entry=target;
            }
        }
    }
    if (!entry || plan->header!=entry || plan->header==plan->push_block) return false;
    size_t body=0; uint32_t latch=0;
    for (uint32_t b=1; b<=f->block_count && c_request_poll(c); ++b) {
        if (nodes[b-1].reach!=3) continue;
        ++body;
        const XrLirBlockData *block=&f->blocks[b-1];
        const XrLirInstructionView *term=c_pointwise_end(f,b);
        if (term->op!=XR_LIR_BR && term->op!=XR_LIR_CONDBR) return false;
        for (uint32_t i=1; i<block->instruction_count; ++i) {
            if (b==plan->push_block && (i==plan->push || i==plan->create || i==plan->create+1 ||
                i==plan->read || i==plan->read+1)) continue;
            if (!lir_array_region_pure(f,&block->instructions[i-1].view)) return false;
        }
        for (uint32_t e=0; e<term->edge_count; ++e) {
            const XrLirEdgeView *edge=&term->edges[e];
            if (nodes[edge->target-1].reach!=3) continue;
            if (!c_pointwise_edge(f,c->homes,edge,owner)) return false;
            if (edge->target==entry) {
                if (latch) return false;
                latch=b;
            } else ++nodes[edge->target-1].incoming;
        }
    }
    if (!latch || latch==entry) return false;
    size_t at=0, count=1; work[0]=entry; nodes[entry-1].without_push=true;
    while (at<count && c_request_poll(c)) {
        uint32_t b=work[at++];
        bool bypass=nodes[b-1].without_push && b!=plan->push_block;
        if (b==latch && bypass) return false;
        const XrLirInstructionView *term=c_pointwise_end(f,b);
        for (uint32_t e=0; e<term->edge_count; ++e) {
            uint32_t target=term->edges[e].target;
            LirCPointwiseNode *next=&nodes[target-1];
            if (target==entry || next->reach!=3) continue;
            next->without_push|=bypass;
            if (--next->incoming==0) work[count++]=target;
        }
    }
    return c_request_poll(c) && count==body;
}

static void c_pointwise_push_prepare(LirC *c, uint32_t function, const XrLirValueHomes *homes,
    LirCPointwisePush *output) {
    const XrLirHomeEvent *push=c_pointwise_push_event(homes), *create=NULL, *read=NULL;
    if (!push || !homes->bindings) return;
    if (!c_array_local_candidate(c,homes,push)) return;
    for (size_t i=0; i<homes->event_count; ++i) {
        const XrLirHomeEvent *event=&homes->events[i];
        if (event->block!=push->block || event->source!=push->source ||
            event->forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE) continue;
        if (event->kind==XR_LIR_OWNERSHIP_BINDING_NEW) create=event;
        if (event->kind==XR_LIR_OWNERSHIP_BINDING_READ) read=event;
    }
    if (!create || !read || create->forwarding_drop>=push->instruction || push->instruction>=read->instruction) return;
    const XrLirFunctionData *f=&c->request->module->functions[function-1];
    const XrLirBlockData *append=&f->blocks[push->block-1];
    const XrLirInstructionView *operation=&append->instructions[push->instruction-1].view;
    if (operation->operand_count!=2 || operation->operands[0]!=push->source ||
        operation->operands[1]!=push->array_item) return;
    LirCBindingForward before={0}, after={0};
    if (!c_binding_forward_geometry(c,create,&append->instructions[create->instruction-1].view,&before) ||
        !c_binding_forward_geometry(c,read,&append->instructions[read->instruction-1].view,&after) ||
        before.source!=after.destination || before.destination!=after.source ||
        !xr_lir_home_stationary_edge(homes,create->target,read->target)) return;
    uint32_t owner=homes->values[create->target].stationary_owner;
    size_t workspace=0;
    for (; workspace<homes->workspace_count; ++workspace)
        if (homes->workspaces[workspace].block==push->block &&
            homes->workspaces[workspace].instruction==push->instruction) break;
    if (workspace==homes->workspace_count || !c_array_local_pair(c,homes,push,&homes->workspaces[workspace])) return;
    const XrLirHomeWorkspace *pending=&homes->workspaces[workspace];
    if (pending->first_owner==before.source) return;
    size_t count=f->block_count;
    if (count>(SIZE_MAX-1)/2) { c_error(c,XR_LIR_C_OVERFLOW,"Array append edge inventory overflows"); return; }
    void *node_memory=NULL, *edge_memory=NULL, *work_memory=NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,count,sizeof(LirCPointwiseNode),&node_memory)) ||
        !c_resource(c,xr_compile_resources_calloc(c->request->resources,2*count+1,sizeof(LirCPointwisePredecessor),&edge_memory)) ||
        !c_resource(c,xr_compile_resources_calloc(c->request->resources,count,sizeof(uint32_t),&work_memory))) goto done;
    LirCPointwiseNode *nodes=node_memory;
    LirCPointwisePredecessor *edges=edge_memory;
    uint32_t *work=work_memory;
    size_t index=1;
    for (uint32_t b=1; b<=f->block_count && c_request_poll(c); ++b) {
        const XrLirInstructionView *term=c_pointwise_end(f,b);
        for (uint32_t e=0; e<term->edge_count; ++e) {
            LirCPointwiseNode *target=&nodes[term->edges[e].target-1];
            edges[index]=(LirCPointwisePredecessor){target->predecessors,b};
            target->predecessors=index++;
        }
    }
    if (!c_request_poll(c)) goto done;
    c_pointwise_reach(f,nodes,edges,work,push->block,false);
    c_pointwise_reach(f,nodes,edges,work,push->block,true);
    LirCPointwisePush plan={.push_block=push->block,.push=push->instruction,.create=create->instruction,
        .read=read->instruction,.slots={{before.source,before.source_offset},{before.destination,before.destination_offset},
            {pending->first_owner,pending->offset}}};
    if (c_pointwise_cfg(c,f,nodes,work,owner,&plan)) *output=plan;
done:
    xr_compile_resources_free(work_memory);
    xr_compile_resources_free(edge_memory);
    xr_compile_resources_free(node_memory);
}

static void c_pointwise_push_spec(LirC *c) {
    const LirCPointwisePush *plan=&c->array_region->pointwise;
    c_format(c,"static const XrtArrayI64RegionSlot xrl_pc_slots_%" PRIu32 "[] = {\n",c->function.id);
    for (size_t n=0;n<3;++n) c_format(c,"    {%zu, %zu},\n",plan->slots[n].slot,plan->slots[n].offset);
    c_format(c,"};\nstatic const XrtArrayI64RegionSpec xrl_pc_spec_%" PRIu32 " = {xrl_pc_slots_%" PRIu32
        ", 3, 0};\n",c->function.id,c->function.id);
}

static void c_pointwise_push_header(LirC *c, const XrLirInstructionView *term) {
    if (!c->array_region || c->array_helper_kind || c->array_region->pointwise.header!=c->block) return;
    const LirCPointwisePush *plan=&c->array_region->pointwise;
    c_text(c,"    const bool xrl_pc_body = ");
    if (!plan->body_on_true) c_text(c,"!");
    c_value(c,term->operands[0]); c_text(c,";\n");
    /* Finish the private segment before the next real append can grow it. */
    c_text(c,"    if (xrl_pc_owner && (!xrl_pc_body || xrl_pc_length == xrl_pc_capacity)) {\n"
        "        xrl_pc_owner->length = xrl_pc_length; xrl_pc_owner = NULL;\n"
        "        xrl_pc_growth = xrl_pc_body;\n"
        "    } else if (xrl_pc_body && !xrl_pc_owner && xrl_pc_ready) {\n"
        "        XrtArrayI64RegionCache xrl_pc_start = {0};\n");
    c_format(c,"        if (xrt_array_i64_region_try_begin(&(XrtArrayI64RegionContext){xrl_frame, xrl_payload, "
        "&xrl_frame_locations, &xrl_frame_%" PRIu32 "}, &xrl_pc_spec_%" PRIu32 ", true, &xrl_pc_start)) {\n",
        c->function.id,c->function.id);
    c_text(c,"            XrtArrayBacking *xrl_pc_candidate = (XrtArrayBacking *)xrl_pc_start.owner;\n"
        "            size_t xrl_pc_limit = xrl_pc_candidate->capacity;\n"
        "            if ((uint64_t)xrl_pc_limit > (uint64_t)INT64_MAX) xrl_pc_limit = (size_t)INT64_MAX;\n"
        "            if (xrl_pc_start.length < xrl_pc_limit) {\n"
        "                xrl_pc_owner = xrl_pc_candidate; xrl_pc_length = xrl_pc_start.length;\n"
        "                xrl_pc_capacity = xrl_pc_limit;\n"
        "            }\n        }\n    }\n"
        "    if (!xrl_pc_body) xrl_pc_ready = false;\n");
}

static void c_pointwise_push_instruction(LirC *c, const XrLirInstructionView *op, uint32_t source) {
    if (!c->array_region || c->array_helper_kind || c->array_region->pointwise.push_block!=c->block) {
        c_array_instruction(c,op,source); return;
    }
    const LirCPointwisePush *plan=&c->array_region->pointwise;
    uint32_t i=c->instruction;
    bool push=i==plan->push;
    if (push) {
        c_text(c,"    if (xrl_pc_owner) {\n        const int64_t xrl_pc_item = "); c_value(c,op->operands[1]);
        c_text(c,";\n        memcpy(xrl_pc_owner->elements + xrl_pc_length * sizeof(xrl_pc_item), "
            "&xrl_pc_item, sizeof(xrl_pc_item));\n        ++xrl_pc_length;\n    } else {\n");
    } else if (i==plan->create || i==plan->read) c_text(c,"    if (!xrl_pc_owner) {\n");
    c_array_instruction(c,op,source);
    if (push) c_text(c,"        xrl_pc_ready = true; xrl_pc_growth = false;\n    }\n");
    else if (i==plan->create+1 || i==plan->read+1) c_text(c,"    }\n");
}
