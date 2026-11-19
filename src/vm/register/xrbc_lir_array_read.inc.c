/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Project a wrapping reduction over an already-proved read window.
 */
static uint32_t rbc_lir_array_read_predecessor(const RbcLirCompiler *c,
    const XrLirArrayRegionMove *move, uint32_t value) {
    XrLirInstructionView term={0};
    if (xr_lir_instruction_view(c->input,(XrLirBlock){c->input,c->function.id,move->step.block},
        move->step.instruction-1,&term)!=XR_LIR_OK || !move->step.edge || move->step.edge>term.edge_count) return 0;
    const XrLirEdgeView *edge=&term.edges[move->step.edge-1];
    XrLirBlockView target={0};
    if (xr_lir_block_view(c->input,(XrLirBlock){c->input,c->function.id,edge->target},&target)!=XR_LIR_OK ||
        target.parameter_count!=edge->argument_count) return 0;
    for (uint32_t a=0;a<target.parameter_count;++a)
        if (target.parameters[a]==value) return edge->arguments[a];
    return value;
}

static bool rbc_lir_array_read_reduction(const RbcLirCompiler *c, const XrLirArrayRegionBinding *row,
    XrRbcArrayReadFold *fold) {
    const XrLirArrayRegionBindings *bindings=c->value_code->array_regions;
    const XrLirArraySetRegion *region=&row->region;
    const XrLirArraySetRegionRange *range=&region->range;
    const XrLirArrayRegionMove *moves=bindings->moves+row->first_move;
    if (row->move_count<4 || !row->slot_count || row->owner_home.kind!=XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW ||
        (range->kind!=XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN &&
            range->kind!=XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT)) return false;
    XrLirInstructionView get={0},add={0}; size_t primary=0;
    XrLirBlock header={c->input,c->function.id,region->header};
    XrLirBlockView parameters={0};
    if (xr_lir_block_view(c->input,header,&parameters)!=XR_LIR_OK ||
        xr_lir_instruction_view(c->input,(XrLirBlock){c->input,c->function.id,region->primary_block},
            region->primary_instruction-1,&get)!=XR_LIR_OK || get.op!=XR_LIR_CALL_INTRINSIC ||
        !get.result || get.type!=XR_LIR_I64) return false;
    /* The common moves enumerate the sole body route. Extra computations
     * retain their ordinary bytecode unless this projection can reproduce them. */
    size_t latch=row->move_count-2;
    for (size_t n=2;n<=latch;++n) {
        uint32_t block=moves[n].step.block;
        if (block==region->primary_block) primary=n;
        for (uint32_t i=1;i<moves[n].step.instruction;++i) {
            if ((block==region->primary_block && i==region->primary_instruction) ||
                (block==range->increment_block && i==range->increment_instruction)) continue;
            XrLirInstructionView op={0};
            if (xr_lir_instruction_view(c->input,(XrLirBlock){c->input,c->function.id,block},i-1,&op)!=XR_LIR_OK)
                return false;
            if (op.op==XR_LIR_CONST) continue;
            if (add.result || block!=region->primary_block || i<=region->primary_instruction ||
                op.op!=XR_LIR_ADD || op.type!=XR_LIR_I64 || op.operand_count!=2 ||
                (op.operands[0]==get.result)==(op.operands[1]==get.result)) return false;
            add=op;
        }
    }
    if (!primary || !add.result || moves[latch].step.block!=region->latch) return false;
    uint32_t accumulator=add.operands[add.operands[0]==get.result];
    for (size_t n=primary;n>1;--n)
        accumulator=rbc_lir_array_read_predecessor(c,&moves[n-1],accumulator);
    if (!accumulator || accumulator==range->iv || accumulator==range->length) return false;
    size_t scalar_parameters=0;
    for (uint32_t n=0;n<parameters.parameter_count;++n) {
        uint32_t value=parameters.parameters[n];
        if (value==accumulator) ++scalar_parameters;
        else if (value!=range->length && value!=range->iv && value!=region->canonical_value) return false;
    }
    if (scalar_parameters!=1) return false;
    uint32_t next_accumulator=rbc_lir_array_read_predecessor(c,&moves[latch],accumulator);
    uint32_t result=next_accumulator;
    for (size_t n=latch;n>1;--n)
        result=rbc_lir_array_read_predecessor(c,&moves[n-1],result);
    if (result!=add.result) return false;
    *fold=(XrRbcArrayReadFold){.accumulator=accumulator,.next_accumulator=next_accumulator};
    if (range->kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT) {
        const XrLirFunctionData *function=&c->input->functions[c->function.id-1];
        const XrLirInstructionView *bound=xr_lir_internal_value_definition(function,range->length);
        /* An immediate can lack storage, and a header constant need not
         * remain live after its comparison. Use the exact SSA bits. */
        if (bound && bound->op==XR_LIR_CONST && bound->type==XR_LIR_I64) {
            fold->constant=true;
            fold->limit_bits=bound->bits;
        } else {
            fold->limit=range->length;
        }
    }
    const XrRbcValueLocation *locations=c->value_code->locations;
    return locations[range->iv].kind==XR_RBC_VALUE_REGISTER && locations[range->latch_iv].kind==XR_RBC_VALUE_REGISTER &&
        locations[fold->accumulator].kind==XR_RBC_VALUE_REGISTER && locations[fold->next_accumulator].kind==XR_RBC_VALUE_REGISTER &&
        (!fold->limit || locations[fold->limit].kind==XR_RBC_VALUE_REGISTER);
}

static bool rbc_lir_array_read_prepare(RbcLirCompiler *c) {
    XrRbcValueCode *code=c->value_code;
    const XrLirArrayRegionBindings *bindings=code->array_regions;
    if (!bindings) return true;
    XrRbcArrayReadFold *folds=NULL;
    for (size_t n=0;n<bindings->edge_count;++n) {
        XrLirArrayRegionEdge edge=bindings->edges[n];
        if (edge.kind!=XR_LIR_ARRAY_REGION_EDGE_LATCH || !c->homes->edges[n].direct_header_move) continue;
        const XrLirArrayRegionBinding *row=&bindings->regions[edge.region-1];
        XrRbcArrayReadFold fold={0};
        if (row->region.primary_kind!=XR_LIR_ARRAY_REGION_PRIMARY_GET || !rbc_lir_array_read_reduction(c,row,&fold)) continue;
        if (!folds) {
            folds=xr_calloc(bindings->region_count,sizeof(*folds));
            if (!folds) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own Array reduction descriptors");
            code->array_read_folds=folds;
        }
        folds[edge.region-1]=fold;
    }
    return true;
}

static uint32_t rbc_lir_array_read_edge(const XrRbcValueCode *code, uint32_t index) {
    if (!code->array_read_folds || index>=code->array_regions->edge_count) return 0;
    XrLirArrayRegionEdge edge=code->array_regions->edges[index];
    return (edge.kind==XR_LIR_ARRAY_REGION_EDGE_ENTRY || edge.kind==XR_LIR_ARRAY_REGION_EDGE_LATCH) &&
        code->array_read_folds[edge.region-1].accumulator ? edge.region : 0;
}
