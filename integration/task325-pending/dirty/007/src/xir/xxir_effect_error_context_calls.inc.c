/* Unknown/may bits are invariant under substitution. A symbolic Error atom
 * expands through the current caller's actual type and the complete atom
 * inventory; a constructed generic atom needs its own structural type proof. */
static XrXirStatus error_finite_call_summary(const EffectErrorEquationRead *equation,
    const XrXirInstruction *op,uint32_t target,uint32_t word,uint64_t *output) {
    const EffectErrorFiniteRead *read=equation->owner;const EffectErrorFinite *proof=read->proof;
    if(target>=proof->contexts || word>=proof->words) return XR_XIR_BAD_STRUCTURE;
    uint64_t first=proof->rows[target].end-(uint64_t)proof->words*64;
    if(!op->type_arguments[1]) return error_finite_word(read,first+(uint64_t)word*64,64,output);
    const XrXirGeneric *generic=equation->body.domain.generics?&equation->body.domain.generics[equation->function]:NULL;
    uint32_t begin=op->type_arguments[0],count=op->type_arguments[1];
    if(!generic || begin>generic->argument_count || count>generic->argument_count-begin || !generic->arguments)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t result=0;
    for(uint32_t source=0;source<proof->words;++source) {
        uint64_t row=0;XrXirStatus status=error_finite_word(read,first+(uint64_t)source*64,64,&row);
        if(status!=XR_XIR_OK) return status;
        if(!word && !source) result|=row&UINT64_C(3);
        for(uint32_t bit=0;bit<64;++bit) {
            if(!xir_compile_work(read->work,1)) return XR_XIR_BUDGET;
            uint64_t index=(uint64_t)source*64+bit;
            if(index<2 || !(row&(UINT64_C(1)<<bit))) continue;
            if(index-2>=proof->atom_count) return XR_XIR_BAD_STRUCTURE;
            const EffectErrorAtom *atom=&proof->atoms[index-2];
            if(atom->variant==XR_XIR_ERROR_SYMBOLIC_VARIANT) {
                uint32_t parameter=(uint32_t)atom->type-XR_XIR_TYPE_PARAMETER_BASE;
                if(parameter>=count) return XR_XIR_BAD_TYPE;
                uint64_t expanded=0;
                status=error_equation_type_word(equation,generic->arguments[begin+parameter],word,&expanded);
                if(status!=XR_XIR_OK) return status;
                result|=expanded;
            } else {
                if(xr_xir_type_span(equation->body.domain.types,atom->type)) return XR_XIR_UNRESOLVED;
                if(source==word) result|=UINT64_C(1)<<bit;
            }
        }
    }
    *output=result;return XR_XIR_OK;
}
static XrXirStatus error_finite_call(const EffectErrorEquationRead *equation,
    uint32_t instruction,uint32_t word,uint64_t *output) {
    const EffectErrorFiniteRead *read=equation->owner;const EffectErrorFinite *proof=read->proof;
    const EffectInvocationFinal *final=proof->invocations;XrXirInstruction op;
    XrXirStatus status=error_equation_operation(equation,instruction,&op);
    if(status!=XR_XIR_OK) return status;
    if(op.op==XR_XIR_CALL_REQUIREMENT) { *output=word?0:1;return XR_XIR_OK; }
    bool indirect=op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT;
    uint64_t result=0;bool present=false;
    uint32_t node=proof->rows[equation->context].node,walked=0;
    for(uint32_t e=final->nodes[node].edge_head;e!=UINT32_MAX;e=final->edges[e].next) {
        if(!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
        if(e>=final->edge_count || ++walked>final->edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge *edge=&final->edges[e];
        if(edge->latent || edge->instruction!=instruction) continue;
        bool matched=false;status=effect_final_error_edge_matches(read->work,proof->material,final,e,&matched);
        if(status!=XR_XIR_OK) return status;
        if(!matched) continue;
        uint32_t target=UINT32_MAX;status=error_finite_node_context(read->work,proof,edge->target,&target);
        if(status!=XR_XIR_OK) return status;
        uint64_t contribution=0;
        status=error_finite_call_summary(equation,&op,target,word,&contribution);
        if(status!=XR_XIR_OK) return status;
        result|=contribution;present=true;
    }
    if(indirect) {
        if(op.immediate<0 || (uint64_t)op.immediate>=proof->rows[equation->context].values)
            return XR_XIR_BAD_STRUCTURE;
        bool unknown=false;
        status=effect_final_error_unknown(read->work,proof->material,final,node,
            (uint32_t)op.immediate,present,&unknown);
        if(status!=XR_XIR_OK) return status;
        if(unknown && !word) result|=1;
    } else if(!present) {
        uint32_t target=UINT32_MAX;
        if(op.op==XR_XIR_CALL_DEFAULT || op.op==XR_XIR_INVOKE_DEFAULT) {
            const XrXirDefaultBinding *binding=NULL;const uint32_t *id=xr_xir_default_identity(&op);
            status=xir_metadata_default_lookup(read->work,&equation->body.domain,id[0],id[1],&binding);
            if(status!=XR_XIR_OK) return status;
            if(!binding) return XR_XIR_BAD_STRUCTURE;
            target=binding->function;
        } else if(op.op==XR_XIR_CALL || op.op==XR_XIR_INVOKE || op.op==XR_XIR_GO) {
            if(op.immediate<0 || (uint64_t)op.immediate>=proof->functions) return XR_XIR_BAD_STRUCTURE;
            target=(uint32_t)op.immediate;
        } else return XR_XIR_BAD_STRUCTURE;
        if(target>=proof->functions) return XR_XIR_BAD_TYPE;
        status=error_finite_call_summary(equation,&op,target,word,&result);
        if(status!=XR_XIR_OK) return status;
    }
    *output=result;return XR_XIR_OK;
}
