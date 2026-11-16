/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_cycles.inc.c - Final construction multiplicity on the CFG
 */
typedef struct LirCallableCycleNode {
    uint32_t index, low, parent, edge;
    bool active, cyclic;
} LirCallableCycleNode;
_Static_assert(sizeof(LirCallableCycleNode) % _Alignof(uint32_t) == 0,
    "the callable component member stack preserves its natural alignment");
#include "xlir_cleanup_proof_internal.h"

/* Iterative Tarjan visits each block and edge once. The parent cursor is the
 * DFS stack; the separate member stack contains only unfinished components. */
static XrLirStatus lir_block_multiplicity_create(const XrLirFunctionData *f,bool need_rank,
    XrLirBlockConstructionFacts *empty) {
    if (!empty || empty->may_repeat || empty->sink_first || empty->count) return XR_LIR_INVALID_ARGUMENT;
    size_t bytes = 0;
    if (!f->block_count || !lir_callable_extent(&bytes, f->block_count, sizeof(LirCallableCycleNode)) ||
        !lir_callable_extent(&bytes, f->block_count, sizeof(uint32_t))) return XR_LIR_OUT_OF_MEMORY;
    LirCallableCycleNode *nodes = xr_calloc(1, bytes);
    if (!nodes) return XR_LIR_OUT_OF_MEMORY;
    size_t rank_offset=0,map_bytes=f->block_count;
    if (need_rank) {
        size_t alignment=_Alignof(uint32_t);
        if (map_bytes>SIZE_MAX-(alignment-1)) {xr_free(nodes);return XR_LIR_OUT_OF_MEMORY;}
        rank_offset=(map_bytes+alignment-1)&~(alignment-1);map_bytes=rank_offset;
        if (!lir_callable_extent(&map_bytes,f->block_count,sizeof(uint32_t))) {xr_free(nodes);return XR_LIR_OUT_OF_MEMORY;}
    }
    uint8_t *multiplicity=xr_malloc(map_bytes);
    if (!multiplicity) {xr_free(nodes);return XR_LIR_OUT_OF_MEMORY;}
    uint32_t *rank=need_rank ? (uint32_t *)(multiplicity+rank_offset) : NULL;
    uint32_t *members = (uint32_t *)(nodes + f->block_count);
    uint32_t clock = 0,component=0; size_t used = 0;
    for (uint32_t start = 0; start < f->block_count; ++start) {
        if (nodes[start].index) continue;
        uint32_t current = start;
        nodes[current].parent = UINT32_MAX;
        for (;;) {
            LirCallableCycleNode *node = &nodes[current];
            if (!node->index) {
                node->index = node->low = ++clock;
                members[used++] = current; node->active = true;
            }
            const XrLirBlockData *block = &f->blocks[current];
            const XrLirInstructionView *end = &block->instructions[block->instruction_count - 1].view;
            if (node->edge < end->edge_count) {
                uint32_t target = end->edges[node->edge++].target - 1;
                if (!nodes[target].index) {
                    nodes[target].parent = current; current = target; continue;
                }
                if (nodes[target].active && node->low > nodes[target].index) node->low = nodes[target].index;
                if (target == current) node->cyclic = true;
                continue;
            }
            if (node->low == node->index) {
                size_t begin = used;
                do { XR_CHECK(begin, "a callable component contains its root"); --begin; } while (members[begin] != current);
                bool cyclic = used - begin > 1 || node->cyclic;
                for (size_t n = begin; n < used; ++n) {
                    nodes[members[n]].active = false; nodes[members[n]].cyclic = cyclic;
                    if (rank) rank[members[n]]=component;
                }
                ++component;
                used = begin;
            }
            uint32_t parent = node->parent;
            if (parent == UINT32_MAX) break;
            if (nodes[parent].low > node->low) nodes[parent].low = node->low;
            current = parent;
        }
    }
    for (uint32_t b=0;b<f->block_count;++b) multiplicity[b]=(uint8_t)nodes[b].cyclic;
    xr_free(nodes);*empty=(XrLirBlockConstructionFacts){multiplicity,rank,f->block_count};return XR_LIR_OK;
}
static XrLirStatus lir_callable_construction_reentry(const XrLirFunctionData *f,
    XrLirCallableValueFact *rows,const uint8_t *multiplicity) {
    for (uint32_t v = 0; v < f->value_count; ++v) {
        XrLirCallableValueFact *row = &rows[v + 1];
        if (row->operation == XR_LIR_FUNCTION_REF && row->constructor_storage == XR_LIR_CLOSURE_FRAME && row->capture_count) {
            XR_CHECK(row->block && row->block <= f->block_count, "a final constructor names its verified block");
            if (!multiplicity) return XR_LIR_VERIFY_ERROR;
            row->constructor_may_repeat = multiplicity[row->block - 1]!=0;
        }
    }
    return XR_LIR_OK;
}
