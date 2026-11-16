/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLIR_CLEANUP_PROOF_INTERNAL_H
#define XLIR_CLEANUP_PROOF_INTERNAL_H
#include "xlir_internal.h"

/* A block argument is published by its incoming edge, not by an ordinary
 * instruction. Neither it nor a formal participates in this bounded proof. */
static inline const XrLirInstructionView *lir_cleanup_candidate(const XrLirFunctionData *f,uint32_t value) {
    if (!f->values || f->value_count>f->value_capacity || !value || value>f->value_count ||
        f->values[value-1].parameter || !f->blocks || f->block_count>f->block_capacity) return NULL;
    const XrLirValueData *row=&f->values[value-1];
    if (!row->block || row->block>f->block_count) return NULL;
    const XrLirBlockData *block=&f->blocks[row->block-1];
    if (!block->instructions || block->instruction_count>block->instruction_capacity ||
        row->instruction>=block->instruction_count) return NULL;
    const XrLirInstructionView *op=&block->instructions[row->instruction].view;
    if (op->result!=value) return NULL;
    return op->op==XR_LIR_CALL || op->op==XR_LIR_CALL_INTRINSIC ? op : NULL;
}
static inline const XrLirInstructionView *lir_cleanup_definition(const XrLirModule *m,
    const XrLirFunctionData *f,uint32_t value) {
    const XrLirInstructionView *op=lir_cleanup_candidate(f,value);
    if (!op || !xr_lir_internal_slice_type(m,f->values[value-1].type)) return NULL;
    if (op->op==XR_LIR_CALL) return op;
    XrLirSliceIntrinsic shape;
    if (op->op==XR_LIR_CALL_INTRINSIC &&
        xr_lir_slice_intrinsic_view(m,op->intrinsic,&shape)==XR_LIR_OK &&
        (shape.operation==XR_LIR_SLICE_OP_NEW || shape.operation==XR_LIR_SLICE_OP_REBORROW ||
         shape.operation==XR_LIR_SLICE_OP_BIND)) return op;
    return NULL;
}
/* Allocation preflight precedes public verification. Inspect only bounded
 * instruction storage here; exact Slice roles are checked after admission. */
static inline bool lir_cleanup_rank_needed(const XrLirFunctionData *f) {
    if (!f->blocks || f->block_count>f->block_capacity) return false;
    for (uint32_t b=0;b<f->block_count;++b)
        for (uint32_t i=0;i<f->blocks[b].instruction_count;++i) {
            if (!f->blocks[b].instructions || f->blocks[b].instruction_count>f->blocks[b].instruction_capacity) return false;
            const XrLirInstructionView *op=&f->blocks[b].instructions[i].view;
            if (op->op==XR_LIR_SLICE_CLEANUP && op->bits<=UINT32_MAX &&
                lir_cleanup_candidate(f,(uint32_t)op->bits)) return true;
        }
    return false;
}
static inline bool lir_cleanup_module_rank_needed(const XrLirModule *m) {
    if (!m->functions || m->function_count>m->function_capacity) return false;
    for (uint32_t f=0;f<m->function_count;++f)
        if (lir_cleanup_rank_needed(&m->functions[f])) return true;
    return false;
}
static inline bool lir_cleanup_unconstructed(const XrLirModule *m,const XrLirFunctionData *f,
    XrLirBlockConstructionFacts facts,uint32_t block,uint32_t instruction,uint32_t value) {
    if (!facts.may_repeat || !facts.sink_first || facts.count!=f->block_count ||
        !block || block>f->block_count || !instruction ||
        instruction>f->blocks[block-1].instruction_count || !lir_cleanup_definition(m,f,value)) return false;
    const XrLirValueData *definition=&f->values[value-1];
    if (!definition->block || definition->block>f->block_count) return false;
    if (definition->block==block)
        return !facts.may_repeat[block-1] && instruction-1<definition->instruction;
    uint32_t from=facts.sink_first[definition->block-1],to=facts.sink_first[block-1];
    /* Tarjan emits sinks first. A path from definition to cleanup would imply
     * rank(definition)>=rank(cleanup), so only the strict reverse is proof. */
    return from<f->block_count && to<f->block_count && from<to;
}
#endif
