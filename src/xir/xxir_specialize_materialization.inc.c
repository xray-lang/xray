/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_specialize_materialization.inc.c - Exact literal-helper call projection
 *
 * KEY CONCEPT:
 *   Each receiving edge comes from the current verified Source catalogue and
 *   actual caller/callee origins; a private role alone creates no new edge.
 */
static XrXirStatus spec_materialization_construction_base(SpecContext *c,
    const XrXirModule *module,XrXirConstruction **output) {
    const XrXirSlotFacts *slots=xr_xir_compile_construction_slots(c->source_artifact->construction);
    if (!c->source_artifact->construction->materialization_count)
        return xir_construction_project(&c->remaining,module->types,slots,output, c->control);
    uint32_t count=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!spec_progress(c)) return c->diagnostic.status;
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!spec_progress(c)) return c->diagnostic.status;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op==XR_XIR_CALL && c->identities[op->immediate].method_kind==XR_XIR_MATERIALIZER) {
                if (count==UINT32_MAX) return XR_XIR_BUDGET;
                ++count;
            }
        }
    }
    if (!count) return xir_construction_project(&c->remaining,module->types,slots,output, c->control);
    XrXirMaterializationBinding *edges=spec_alloc(c,count,sizeof(*edges));
    if (!edges) return c->diagnostic.status;
    uint32_t next=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!spec_progress(c)) return c->diagnostic.status;
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!spec_progress(c)) return c->diagnostic.status;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_CALL || c->identities[op->immediate].method_kind!=XR_XIR_MATERIALIZER) continue;
            uint32_t caller=c->instances[f].declaration,callee=c->instances[op->immediate].declaration;
            const XrXirMaterializationBinding *original=xir_materialization_binding(c->source_artifact->construction,caller,i);
            if (!original || original->function!=callee || original->nominal+1!=c->identities[op->immediate].nominal_owner || next==count)
                return XR_XIR_BAD_STRUCTURE;
            edges[next++]=(XrXirMaterializationBinding){f,i,(uint32_t)op->immediate,original->nominal};
        }
    }
    if (next!=count) return XR_XIR_BAD_STRUCTURE;
    XrXirConstruction *empty=NULL;
    XrXirStatus status=xir_construction_project(&c->remaining,module->types,slots,&empty, c->control);
    if (status!=XR_XIR_OK) return status;
    XrXirConstructionSpec input={empty->rows,empty->count,edges,count,NULL,NULL};
    status=xr_xir_compile_construction_new_v5(&c->remaining,module->types,&input,slots,output, c->control);
    xr_xir_compile_construction_free(empty);return status;
}
static XrXirStatus spec_materialization_construction(SpecContext *c,
    const XrXirModule *module,XrXirConstruction **output) {
    XrXirConstruction *base=NULL;
    XrXirStatus status=spec_materialization_construction_base(c,module,&base);
    if (status!=XR_XIR_OK) return status;
    if (!c->source_artifact->construction->borrow_returns.count) {*output=base;return XR_XIR_OK;}
    XrXirBorrowReturn *rows=spec_alloc(c,module->function_count,sizeof(*rows));uint32_t count=0;
    if (!rows) {xr_xir_compile_construction_free(base);return c->diagnostic.status;}
    for (uint32_t f=0;f<module->function_count && status==XR_XIR_OK;++f) {
        if (!spec_progress(c)) {status=c->diagnostic.status;break;}
        XrXirBorrowReturn row={0};bool present=false;
        status=xir_borrow_return_describe(&c->remaining,module,f,&row,&present,c->control);
        if (status!=XR_XIR_OK || !present) continue;
        uint32_t declaration=c->instances[f].declaration;
        const XrXirBorrowReturn *source=xir_borrow_return_find(c->source_artifact->construction,declaration);
        if (!source || source->receiver!=row.receiver || source->field!=row.field ||
            source->native_callable!=row.native_callable || declaration>=c->source_artifact->module.function_count ||
            source->nominal+1u!=c->source_artifact->module.declarations->functions[declaration].nominal_owner ||
            row.nominal+1u!=module->declarations->functions[f].nominal_owner) {status=XR_XIR_BAD_STRUCTURE;break;}
        rows[count++]=row;
    }
    if (status==XR_XIR_OK) {
        XrXirBorrowReturns returns={count?rows:NULL,count};
        XrXirConstructionSpec input={base->rows,base->count,base->materializations,base->materialization_count,
            &base->comptime,&returns};
        status=xr_xir_compile_construction_new_v5(&c->remaining,module->types,&input,&base->slots,output,c->control);
    }
    xr_xir_compile_construction_free(base);return status;
}
