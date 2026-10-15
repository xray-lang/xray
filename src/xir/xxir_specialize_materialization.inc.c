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
static XrXirStatus spec_materialization_construction(SpecContext *c,
    const XrXirModule *module,XrXirConstruction **output) {
    if (!c->source_artifact->construction->materialization_count)
        return xir_construction_empty(&c->remaining,module->types,output);
    uint32_t count=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!spec_work(c,4)) return c->diagnostic.status;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op==XR_XIR_CALL && c->identities[op->immediate].method_kind==XR_XIR_MATERIALIZER) {
                if (count==UINT32_MAX) return XR_XIR_BUDGET;
                ++count;
            }
        }
    }
    if (!count) return xir_construction_empty(&c->remaining,module->types,output);
    XrXirMaterializationBinding *edges=spec_alloc(c,count,sizeof(*edges));
    if (!edges) return c->diagnostic.status;
    uint32_t next=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!spec_work(c,260)) return c->diagnostic.status;
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
    XrXirStatus status=xir_construction_empty(&c->remaining,module->types,&empty);
    if (status!=XR_XIR_OK) return status;
    XrXirConstructionSpec input={empty->rows,empty->count,edges,count};
    status=xr_xir_compile_construction_new_v2(&c->remaining,module->types,&input,output);
    xr_xir_compile_construction_free(empty);return status;
}
