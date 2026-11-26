/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_cell_cfg.inc.c - Unique scalar constructors over the existing bytecode CFG
 */
typedef struct RbcCellBlock {
    uint32_t edges[2], edge_count, next, parent, index, low;
    bool stacked, constructor;
} RbcCellBlock;
static bool rbc_cell_successors(RbcStringGraph *g, RbcCellBlock *nodes) {
    const XrRbcFunction *f=g->work.function;
    for (uint32_t b=0;b<g->count;++b) {
        RbcCellBlock *node=&nodes[b];
        const RbcStringIncoming *block=&g->incoming[b];
        bool terminal=false;
        node->parent=UINT32_MAX;
        for (size_t pc=block->begin;pc<block->end;++pc) {
            uint32_t word=f->code[pc],op=word&255;
            if (op==XR_RBC_CELL_NEW) node->constructor=true;
            if (op==XR_RBC_JMP) {
                size_t target;
                if (!rbc_string_jump(f,pc,&target)) return false;
                node->edges[node->edge_count++]=g->indices[target]; terminal=true; break;
            }
            if (xr_rbc_op_is_comparison(op)) {
                node->edges[node->edge_count++]=g->indices[pc+1];
                node->edges[node->edge_count++]=g->indices[pc+2]; terminal=true; break;
            }
            if (xr_rbc_op_is_exit(op) ||
                xr_rbc_value_return_word(g->work.module,f,word) ||
                xr_rbc_aggregate_return_word(f->aggregate_accesses,f->aggregate_access_count,word)) {
                terminal=true; break;
            }
        }
        if (!terminal && block->end<f->code_count)
            node->edges[node->edge_count++]=g->indices[block->end];
        for (uint32_t e=0;e<node->edge_count;++e)
            if (node->edges[e]>=g->count) return false;
    }
    return true;
}
static bool rbc_cell_components(RbcCellBlock *nodes, size_t count, uint32_t *stack) {
    uint32_t serial=0; size_t depth=0;
    for (uint32_t start=0;start<count;++start) {
        if (nodes[start].index) continue;
        uint32_t current=start;
        while (current!=UINT32_MAX) {
            RbcCellBlock *node=&nodes[current];
            if (!node->index) {
                node->index=node->low=++serial; node->stacked=true; stack[depth++]=current;
            }
            if (node->next<node->edge_count) {
                uint32_t child=node->edges[node->next++];
                if (!nodes[child].index) {
                    nodes[child].parent=current; current=child; continue;
                }
                if (nodes[child].stacked && nodes[child].index<node->low) node->low=nodes[child].index;
                continue;
            }
            if (node->low==node->index) {
                size_t members=0; bool constructor=false; uint32_t member;
                do {
                    member=stack[--depth]; nodes[member].stacked=false;
                    constructor|=nodes[member].constructor; ++members;
                } while (member!=current);
                bool cyclic=members>1;
                for (uint32_t e=0;e<node->edge_count;++e) cyclic|=node->edges[e]==current;
                if (cyclic && constructor) return false;
            }
            uint32_t parent=node->parent;
            if (parent!=UINT32_MAX && node->low<nodes[parent].low) nodes[parent].low=node->low;
            current=parent;
        }
    }
    return true;
}
static XrRbcStatus rbc_cell_constructors(RbcStringGraph *g) {
    const XrRbcFunction *f=g->work.function;
    bool constructed=false;
    for (size_t pc=0;pc<f->code_count;++pc) constructed|=(f->code[pc]&255)==XR_RBC_CELL_NEW;
    // Reserved capacity alone grants no live Cell and needs no producer proof.
    // READ/WRITE can legally name a caller-owned ready Cell through raw BITS.
    if (!constructed) return XR_RBC_OK;
    if (!f->cell_count) return XR_RBC_INVALID;
    if (g->count>SIZE_MAX/sizeof(RbcCellBlock) || g->count>SIZE_MAX/sizeof(uint32_t))
        return XR_RBC_OUT_OF_MEMORY;
    uint8_t *seen=xr_calloc(f->cell_count,1);
    RbcCellBlock *nodes=xr_calloc(g->count,sizeof(*nodes));
    uint32_t *stack=xr_malloc(g->count*sizeof(*stack));
    XrRbcStatus status=XR_RBC_OUT_OF_MEMORY;
    if (!seen || !nodes || !stack) goto done;
    status=XR_RBC_INVALID;
    for (size_t pc=0;pc<f->code_count;++pc) {
        uint32_t word=f->code[pc];
        if ((word&255)!=XR_RBC_CELL_NEW) continue;
        uint32_t ordinal=word>>16;
        if (ordinal>=f->cell_count || seen[ordinal] ||
            (f->cell_kinds && f->cell_kinds[ordinal]!=XR_RBC_BITS)) goto done;
        seen[ordinal]=1;
    }
    // Unused ordinals remain unready. Only actual NEW sites need uniqueness
    // and non-reentry; a missing constructor never grants runtime authority.
    if (!rbc_cell_successors(g,nodes) || !rbc_cell_components(nodes,g->count,stack)) goto done;
    status=XR_RBC_OK;
done:
    xr_free(stack); xr_free(nodes); xr_free(seen); return status;
}
