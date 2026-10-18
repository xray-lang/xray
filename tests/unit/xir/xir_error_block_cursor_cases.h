/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_block_cursor_cases.h - Fresh CFG storage positions and literal error oracle
 *
 * The original prefix walk remains an independent storage control. Genuine
 * owned bodies survive their producer and retain all Error and Cell facts.
 */
#ifndef XIR_ERROR_BLOCK_CURSOR_CASES_H
#define XIR_ERROR_BLOCK_CURSOR_CASES_H
static XrXirStatus pc_cursor_original_transfer(ErrorFlow *flow,uint32_t b,
    const uint8_t *zero_copy,ErrorBlockRead *row) {
    const XrXirBlock *block=&flow->function->blocks[b];
    if (flow->body.instruction!=effect_body_owned_instruction) {
        for (uint32_t i=block->first;i<block->first+block->count;++i) {
            XrXirStatus status=error_block_row(flow,b,i,zero_copy,NULL,row);
            if (status!=XR_XIR_OK || flow->restart) return status;
        }
        return XR_XIR_OK;
    }
    const EffectBodyFunction *function=NULL;
    XrXirStatus status=error_owned_function(flow,&function);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->remaining,sizeof(EffectBodyRecordCursor))) return XR_XIR_BUDGET;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    if (!xir_compile_work(flow->remaining,sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t end=block->first+block->count;
    if (!block->count || block->first>=function->instruction_count ||
        block->count>function->instruction_count-block->first) return XR_XIR_BAD_STRUCTURE;
    while (cursor.first<end) {
        if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
        const EffectBodyRun *ref;
        status=effect_body_range_ref(&flow->body.read,function,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(flow->remaining,2+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        if (!xir_compile_work(flow->remaining,1+sizeof(row->constant)+sizeof(row->decoded)))
            return XR_XIR_BUDGET;
        row->constant=ref->mask==0;row->decoded=false;
        uint32_t first=ref->first<block->first?block->first:ref->first;
        uint32_t limit=ref->first+ref->count<end?ref->first+ref->count:end;
        for (uint32_t i=first;i<limit;++i) {
            status=error_block_row(flow,b,i,zero_copy,ref,row);
            if (status!=XR_XIR_OK || flow->restart) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus pc_cursor_original_block(ErrorFlow *flow, uint32_t b,const uint8_t *zero_copy) {
    const XrXirBlock *block = &flow->function->blocks[b];
    if (!xir_compile_work(flow->remaining, flow->stride)) return XR_XIR_BUDGET;
    { if (!xir_compile_work(flow->remaining, flow->stride * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(flow->work, flow->states + (size_t)b * flow->stride, flow->stride * sizeof(uint64_t)); }
    ErrorBlockRead row;
    if (!xir_compile_work(flow->remaining,sizeof(row.end))) return XR_XIR_BUDGET;
    row.end=NULL;
    XrXirStatus transferred=pc_cursor_original_transfer(flow,b,zero_copy,&row);
    if (transferred!=XR_XIR_OK || flow->restart) return transferred;
    const XrXirInstruction *end=row.end;
    if (!end) return XR_XIR_BAD_STRUCTURE;
    uint32_t last=block->first+block->count-1;
    flow->instruction=last;
    if (end->op == XR_XIR_JUMP || end->op == XR_XIR_CLEANUP_REGISTER ||
        end->op == XR_XIR_CLEANUP_LEAVE || end->op == XR_XIR_CLEANUP_ERROR) return error_edge(flow, b, end->targets[0], NULL, true);
    if (end->op == XR_XIR_BRANCH) {
        XrXirStatus status = error_edge(flow, b, end->targets[0], end, true);
        return status == XR_XIR_OK ? error_edge(flow, b, end->targets[1], end, false) : status;
    }
    if (end->op == XR_XIR_INVOKE_DEFAULT || end->op == XR_XIR_INVOKE || end->op == XR_XIR_INVOKE_INDIRECT ||
        end->op == XR_XIR_TASK_AWAIT) {
        XrXirStatus status = error_edge(flow, b, end->targets[0], NULL, true);
        if (status != XR_XIR_OK) return status;
        { if (!xir_compile_work(flow->remaining, (size_t)flow->effects->words * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memset(flow->snapshot, 0, (size_t)flow->effects->words * sizeof(uint64_t)); }
        if (end->op == XR_XIR_TASK_AWAIT) {
            if (!xir_compile_work(flow->remaining, (uint64_t)flow->effects->words * sizeof(uint64_t))) return XR_XIR_BUDGET;
            memcpy(flow->snapshot, error_read(flow, flow->work, end->args[0]),
                (size_t)flow->effects->words * sizeof(uint64_t));
        } else status=error_call(flow, end, flow->snapshot);
        if (status!=XR_XIR_OK || flow->restart) return status;
        if (error_any(flow->snapshot, flow->effects->words)) return error_edge(flow, b, end->targets[1], NULL, true);
    }
    return XR_XIR_OK;
}

static XrXirStatus pc_cursor_original_function(ErrorFlow *flow, uint32_t f) {
    flow->function_index=f;
    XrXirStatus head_status=effect_body_request_head(&flow->body,f,&flow->head);
    if (head_status!=XR_XIR_OK) return head_status;
    flow->function=&flow->head;
    if (!xir_compile_work(flow->remaining,sizeof(flow->cell_types)+sizeof(flow->cell_material)+
        sizeof(flow->cell_function)+sizeof(flow->cell_type_count))) return XR_XIR_BUDGET;
    flow->cell_types=NULL;flow->cell_material=NULL;flow->cell_function=flow->cell_type_count=0;
    flow->cells = NULL; flow->cell_count = flow->active_count = 0;
    if (flow->function->parameter_count > UINT32_MAX - flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    flow->values = flow->function->parameter_count + flow->function->instruction_count;
    uint64_t words = flow->effects->words, rows = (uint64_t)flow->function->block_count + 3;
    uint64_t minimum_extra = (uint64_t)flow->values * 2 * sizeof(uint32_t) + flow->function->block_count;
    /* At least one writable words row and a separate zero vector are required
     * even when no SSA value can carry error facts. Reject resource-impossible
     * metadata before reading it, without charging a fictitious allocation. */
    if (!words || minimum_extra > SIZE_MAX ||
        words > (SIZE_MAX - minimum_extra) / sizeof(uint64_t) / (rows + 1)) return XR_XIR_BUDGET;
    uint64_t minimum = words * (rows + 1) * sizeof(uint64_t) + minimum_extra;
    if (minimum > flow->storage_capacity) {
        if (!xir_compile_context_valid(flow->remaining)) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus admitted = xir_compile_resource_status(xr_compile_resources_admit(
            flow->remaining->resources, (size_t)minimum, minimum));
        if (admitted != XR_XIR_OK) return admitted;
    }
    /* Only byte capacity is local. Every request rebuilds every actual SSA
     * classification; no fact survives a function or atom-width restart. */
    uint8_t inline_membership[1024], *heap_membership = NULL;
    uint32_t inline_sources[1024], *sources = inline_sources;
    uint8_t *membership = inline_membership;
    XrXirStatus status = XR_XIR_OK;
    /* Representation dispatch is not authority. Every real SSA rebuilds
     * membership through the current immutable body's canonical ranges. */
    if (!xir_compile_work(flow->remaining,sizeof(XrXirType *)+4)) return XR_XIR_BUDGET;
    XrXirType inline_cell_types[1024],*cell_types=error_owned_reader(flow)?inline_cell_types:NULL;
    if (flow->values > sizeof(inline_membership)) {
        uint64_t temporary_bytes=(uint64_t)flow->values*(sizeof(*sources)+sizeof(*membership)+
            (cell_types?sizeof(*cell_types):0));
        if (temporary_bytes>SIZE_MAX) return XR_XIR_BUDGET;
        heap_membership = xir_compile_alloc(flow->remaining,(size_t)temporary_bytes,&status);
        if (!heap_membership) return status;
        sources=(uint32_t *)heap_membership;
        if (cell_types) {
            if (!xir_compile_work(flow->remaining,sizeof(cell_types))) { status=XR_XIR_BUDGET;goto membership_done; }
            cell_types=(XrXirType *)(heap_membership+(size_t)flow->values*sizeof(*sources));
            membership=(uint8_t *)(cell_types+flow->values);
        } else membership=heap_membership+(size_t)flow->values*sizeof(*sources);
    }
    if (!xir_compile_work(flow->remaining,sizeof(ErrorMembership))) { status=XR_XIR_BUDGET;goto membership_done; }
    ErrorMembership inventory={membership,sources,cell_types,0};
    status=error_membership(flow,&inventory);
    if (status!=XR_XIR_OK) goto membership_done;
    uint64_t stride = (uint64_t)(flow->active_count ? flow->active_count : 1) * words;
    uint64_t copy_bytes=inventory.zero_copy_count ? ((uint64_t)flow->function->instruction_count+7)/8 : 0;
    uint64_t cell_bytes=error_owned_reader(flow)?(uint64_t)flow->cell_count*sizeof(ErrorCellType):0;
    uint64_t extra = ((uint64_t)flow->values * 2 + flow->active_count + flow->cell_count) *
        sizeof(uint32_t) + cell_bytes + flow->function->block_count + copy_bytes;
    uint64_t zero_bytes = words * sizeof(uint64_t);
    if (extra > SIZE_MAX - zero_bytes ||
        stride > (SIZE_MAX - zero_bytes - extra) / sizeof(uint64_t) / rows) { status = XR_XIR_BUDGET;goto membership_done; }
    uint64_t bytes = stride * rows * sizeof(uint64_t) + zero_bytes + extra;
    if (!xir_compile_work(flow->remaining, stride * rows + words + flow->values + flow->function->block_count))
        { status = XR_XIR_BUDGET;goto membership_done; }
    flow->stride = (size_t)stride;
    status = error_storage_prepare(flow, (size_t)bytes);
    if (status == XR_XIR_OK) {
        size_t state_bytes = (size_t)(stride * rows) * sizeof(uint64_t);
        flow->states = (uint64_t *)flow->storage;
        flow->zero = (const uint64_t *)(flow->storage + state_bytes);
        flow->roots = (uint32_t *)(flow->storage + state_bytes + (size_t)zero_bytes);
        flow->slots = flow->roots + flow->values;
        flow->active = flow->slots + flow->values;
        flow->cells = flow->active + flow->active_count;
        if (cell_bytes) {
            if (!xir_compile_work(flow->remaining,sizeof(flow->cell_types))) { status=XR_XIR_BUDGET;goto membership_done; }
            flow->cell_types=(ErrorCellType *)(flow->cells+flow->cell_count);
        }
        flow->reachable = (uint8_t *)(flow->cells + flow->cell_count)+(size_t)cell_bytes;
        /* Charge the temporary byte scan and all map, inverse and Cell writes. */
        if (!xir_compile_work(flow->remaining, (uint64_t)flow->values * 2 + flow->active_count + flow->cell_count))
            { status = XR_XIR_BUDGET;goto membership_done; }
        uint8_t *zero_copy=copy_bytes ? flow->reachable+flow->function->block_count : NULL;
        uint32_t cell = 0, active = 0;
        for (uint32_t v = 0; v < flow->values; ++v) {
            uint8_t classified = membership[v];
            if (classified & ERROR_ROW_ACTIVE) {
                if (active >= flow->active_count) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
                flow->slots[v] = active; flow->active[active++] = v;
            } else flow->slots[v] = UINT32_MAX;
            if (zero_copy && (classified&ERROR_ROW_COPY) && !(classified&ERROR_ROW_ACTIVE)) {
                if (v<flow->function->parameter_count) { status=XR_XIR_BAD_STRUCTURE;goto membership_done; }
                if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;goto membership_done; }
                uint32_t i=v-flow->function->parameter_count;
                zero_copy[i/8]|=(uint8_t)(UINT32_C(1)<<(i%8));
            }
            if (classified & ERROR_ROW_CELL) {
                if (cell >= flow->cell_count) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
                flow->cells[cell++] = v;
            }
        }
        if (cell != flow->cell_count || active != flow->active_count) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
        status=error_cell_types_prepare(flow,cell_types);
        if (status!=XR_XIR_OK) goto membership_done;
        flow->work = flow->states + (size_t)flow->function->block_count * flow->stride;
        flow->edge = flow->work + flow->stride; flow->snapshot = flow->edge + flow->stride;
        uint32_t summary=flow->invocations?error_invocation_summary(flow->invocations,flow->invocation):f;
        if (summary>=(flow->invocations?flow->summary_count:flow->effects->count)) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
        flow->escaping = flow->effects->errors + (size_t)summary * flow->effects->words;
        status = error_roots(flow,membership,sources);
        xr_compile_resources_free(heap_membership);heap_membership = NULL;
        flow->reachable[0] = ERROR_BLOCK_REACHABLE | ERROR_BLOCK_PENDING;
        for (uint32_t p = 0; p < flow->function->parameter_count && status == XR_XIR_OK; ++p) {
            if (!xir_compile_work(flow->remaining, flow->effects->atom_count + 1)) status = XR_XIR_BUDGET;
            else {
                uint64_t *set = error_value(flow, flow->states, p);
                if (set) error_type(flow, flow->function->parameters[p], set);
            }
        }
        do {
            flow->changed = false;
            for (uint32_t b = 0; b < flow->function->block_count && status == XR_XIR_OK && !flow->restart; ++b) {
                if (!xir_compile_work(flow->remaining, 1)) status = XR_XIR_BUDGET;
                else if (flow->reachable[b] & ERROR_BLOCK_PENDING) {
                    /* Clear before transfer so self/back edges can schedule it again. */
                    flow->reachable[b] &= (uint8_t)~ERROR_BLOCK_PENDING;
                    status = pc_cursor_original_block(flow, b,zero_copy);
                }
            }
        } while (status == XR_XIR_OK && flow->changed && !flow->restart);
    }
membership_done:
    xr_compile_resources_free(heap_membership);
    return status;
}
static void pc_cursor_flow(const XrXirCompileContext *context,XrXirEffects *effects,
    EffectTerms *terms,ErrorFlow *flow) {
    *terms=(EffectTerms){0};terms->remaining=context;
    terms->types=*effects->invocations->body->domain.types;
    *flow=(ErrorFlow){0};flow->body=effect_body_owned_request(context,effects->invocations->body);
    flow->effects=effects;flow->terms=terms;flow->remaining=(XrXirCompileContext *)context;
}
static void pc_cursor_same_rows(const ErrorFlow *a,const ErrorFlow *b) {
    CHECK(a->values==b->values && a->stride==b->stride && a->active_count==b->active_count &&
        a->cell_count==b->cell_count && a->function->block_count==b->function->block_count);
    CHECK(!memcmp(a->states,b->states,(size_t)a->function->block_count*a->stride*sizeof(uint64_t)));
    CHECK(!memcmp(a->roots,b->roots,(size_t)a->values*sizeof(uint32_t)));
    CHECK(!memcmp(a->slots,b->slots,(size_t)a->values*sizeof(uint32_t)));
    CHECK(!memcmp(a->reachable,b->reachable,a->function->block_count));
}
static void pc_error_block_cursor_controls(void) {
    uint64_t original_work=0,current_work=0;
    for(unsigned mode=PC_FORWARD;mode<=PC_TEMPORARY;++mode)for(unsigned snapshot=0;snapshot<2;++snapshot) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        EdgeFixture fixture;pc_fixture(&fixture,(PendingCase)mode,snapshot!=0);fixture.module.stage=XR_XIR_BUILT;
        XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
        CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);pc_golden(effects,(PendingCase)mode,snapshot!=0);
        xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
        EffectTerms old_terms={0},new_terms={0};ErrorFlow old_flow={0},new_flow={0};
        pc_cursor_flow(&context,effects,&old_terms,&old_flow);pc_cursor_flow(&context,effects,&new_terms,&new_flow);
        effects->errors[1]=0;uint64_t entry=pc_stats(&context).work;
        CHECK(pc_cursor_original_function(&old_flow,1)==XR_XIR_OK);
        original_work+=pc_stats(&context).work-entry;
        CHECK(effects->errors[1]==pc_expected((PendingCase)mode,snapshot!=0));
        effects->errors[1]=0;new_flow.block_cursor=(EffectBodyRecordCursor){UINT32_MAX,UINT32_MAX,UINT32_MAX};
        entry=pc_stats(&context).work;CHECK(error_function(&new_flow,1)==XR_XIR_OK);
        current_work+=pc_stats(&context).work-entry;pc_golden(effects,(PendingCase)mode,snapshot!=0);
        pc_cursor_same_rows(&old_flow,&new_flow);
        const EffectBodyFunction *body=&effects->invocations->body->functions[1];
        CHECK(new_flow.block_cursor.run<body->run_count && new_flow.block_cursor.last<body->instruction_count);
        new_flow.block_cursor=(EffectBodyRecordCursor){UINT32_MAX,UINT32_MAX,UINT32_MAX};
        CHECK(error_function(&new_flow,0)==XR_XIR_OK && new_flow.block_cursor.last==0);
        effects->errors[1]=0;CHECK(error_function(&new_flow,1)==XR_XIR_OK);
        pc_golden(effects,(PendingCase)mode,snapshot!=0);pc_cursor_same_rows(&old_flow,&new_flow);
        error_storage_free(&old_flow);error_storage_free(&new_flow);
        effect_terms_free(&old_terms);effect_terms_free(&new_terms);xr_xir_compile_effects_free(effects);
        pc_release(&context,baseline);
    }
    printf("Owned Error cursor:20 literal CFG cases after producer death, poisoned position/function switch, original work%llu current work%llu physical0\n",
        (unsigned long long)original_work,(unsigned long long)current_work);
}
#endif // XIR_ERROR_BLOCK_CURSOR_CASES_H
