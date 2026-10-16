/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_flow.inc.c - Finite real callable and common Cell origin projection
 */
typedef struct EffectInvocationFlow {
    EffectInvocationWorkspace *owner;
    const EffectInvocationRead *read;
    const EffectBodyFunctionHead *function;
    EffectBodyFunctionHead head;
    EffectBodyRequest body;
    EffectInvocationBasis basis;
    uint32_t node,values,instruction,row_count;
    uint32_t *row_indices;
    const EffectPhysicalFunction *physical;
    uint64_t *rows;
    uint64_t *fixed,*certified;
    void *memory;
    uint64_t *cell_row;
    EffectInvocationScratch *scratch;
    bool changed,precision;
} EffectInvocationFlow;

/* Requests borrow only for this one replay. Every failed field read is
 * returned before it can be interpreted as a type, origin or permission. */
static XrXirStatus effect_invocation_flow_instruction(EffectInvocationFlow *flow,
    uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!flow || !flow->read || flow->node>=flow->read->final->count) return XR_XIR_BAD_STRUCTURE;
    return effect_body_request_instruction(&flow->body,flow->read->final->nodes[flow->node].body,i,storage,output);
}
static XrXirStatus effect_invocation_flow_type(EffectInvocationFlow *flow,
    uint32_t value,XrXirType *output) {
    if (!flow || !flow->read || flow->node>=flow->read->final->count) return XR_XIR_BAD_STRUCTURE;
    return effect_body_request_type(&flow->body,flow->read->final->nodes[flow->node].body,value,output);
}

static XrXirStatus effect_invocation_cell(EffectInvocationFlow *flow,
    uint32_t value,uint64_t *output,bool *changed);

/* The map belongs to this one immutable body replay, never to a Source
 * refiner generation. MAX denotes absent callable storage, not ROOT None. */
static uint64_t *effect_invocation_row(const EffectInvocationFlow *flow,uint32_t value) {
    if (!flow || value>=flow->values || !flow->row_count || !flow->row_indices)
        return NULL;
    uint32_t row=flow->row_indices[value];
    return row<flow->row_count ? flow->rows+(size_t)row*flow->basis.fn_words : NULL;
}

static void effect_invocation_flow_finish(EffectInvocationFlow *flow) {
    if (!flow) return;
    effect_invocation_scratch_release(flow->read,flow->scratch);
    flow->scratch=NULL;flow->memory=NULL;flow->cell_row=NULL;
    flow->rows=NULL;flow->fixed=NULL;flow->certified=NULL;flow->row_indices=NULL;
}
static XrXirStatus effect_invocation_flow_storage(EffectInvocationFlow *flow) {
    const EffectBodyFunctionHead *function=flow->function;
    const EffectPhysicalDomain *domain=flow->read->final->physical;
    uint32_t body=flow->read->final->nodes[flow->node].body;
    if (!domain || domain->resources!=flow->read->work->resources || body>=domain->count ||
        !domain->functions) return XR_XIR_BAD_STRUCTURE;
    const EffectPhysicalFunction *physical=&domain->functions[body];
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    if (!xir_compile_work(flow->read->work,6)) return XR_XIR_BUDGET;
    if (values>UINT32_MAX || values!=physical->values || physical->parameters!=function->parameter_count ||
        physical->instructions!=function->instruction_count || physical->rows>values ||
        physical->count>physical->capacity || (physical->count && !physical->records))
        return XR_XIR_BAD_STRUCTURE;
    uint64_t words=(uint64_t)physical->rows*flow->basis.fn_words;
    uint64_t indices=physical->rows?values*sizeof(uint32_t):0;
    uint64_t flag_words=(values+63)/64;
    if (words>SIZE_MAX/sizeof(*flow->rows) || indices>SIZE_MAX-words*sizeof(*flow->rows) ||
        words*sizeof(*flow->rows)+indices>SIZE_MAX-7) return XR_XIR_BUDGET;
    if (!xir_compile_work(flow->read->work,6+sizeof(flag_words))) return XR_XIR_BUDGET;
    uint64_t flag_offset=(words*sizeof(*flow->rows)+indices+7)&~UINT64_C(7);
    if (flag_words>(SIZE_MAX-flag_offset)/(2*sizeof(uint64_t))) return XR_XIR_BUDGET;
    size_t bytes=(size_t)(flag_offset+2*flag_words*sizeof(uint64_t));
    uint64_t cell_bytes=(uint64_t)flow->basis.cell_words*sizeof(uint64_t);
    if (cell_bytes>SIZE_MAX-bytes) return XR_XIR_BUDGET;
    if (flow->memory || flow->scratch) return XR_XIR_BAD_STRUCTURE;
    flow->physical=physical;
    XrXirStatus status=effect_invocation_scratch_claim(flow->read,bytes+(size_t)cell_bytes,&flow->scratch);
    if (status!=XR_XIR_OK) return status;
    flow->memory=flow->scratch?(void *)(flow->scratch+1):NULL;
    flow->cell_row=cell_bytes?(uint64_t *)((uint8_t *)flow->memory+bytes):NULL;
    uint64_t initialized=words*sizeof(*flow->rows)+indices+2*flag_words*sizeof(uint64_t);
    if (!xir_compile_work(flow->read->work,initialized)) return XR_XIR_BUDGET;
    flow->rows=flow->memory;flow->row_count=physical->rows;flow->values=(uint32_t)values;
    flow->row_indices=physical->rows?(uint32_t *)(flow->rows+(size_t)words):NULL;
    flow->fixed=flag_words?(uint64_t *)((uint8_t *)flow->memory+(size_t)flag_offset):NULL;
    flow->certified=flag_words?flow->fixed+(size_t)flag_words:NULL;
    if (words) memset(flow->rows,0,(size_t)words*sizeof(*flow->rows));
    if (flag_words) memset(flow->fixed,0,(size_t)flag_words*2*sizeof(uint64_t));
    if (physical->rows) {
        for (uint32_t v=0;v<(uint32_t)values;++v) flow->row_indices[v]=UINT32_MAX;
        uint32_t row=0;
        for (uint32_t r=0;r<physical->count;++r) {
            if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=physical->records[r];
            if (!(record.mask&EFFECT_PHYSICAL_CALLABLE)) continue;
            if (record.value>=values || row>=physical->rows) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(flow->read->work,sizeof(*flow->row_indices))) return XR_XIR_BUDGET;
            flow->row_indices[record.value]=row++;
        }
        if (row!=physical->rows) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}


/* Both finite flags still represent every actual SSA, including non-callable
 * barrier/alias facts. A compressed bit is never interpreted as a ROOT,
 * producer, Cell or Class permission. The full independent gates remain. */
static XrXirStatus effect_invocation_flags_match(EffectInvocationFlow *flow) {
    if (!flow || !flow->read || !flow->function || !flow->physical ||
        flow->values!=flow->physical->values ||
        (uint64_t)flow->function->parameter_count+flow->function->instruction_count!=flow->values)
        return XR_XIR_BAD_STRUCTURE;
    if (!flow->values) return !flow->fixed && !flow->certified?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    uint64_t words=((uint64_t)flow->values+63)/64;
    if (!flow->memory || !flow->fixed || !flow->certified ||
        flow->certified!=flow->fixed+(size_t)words) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->read->work,4)) return XR_XIR_BUDGET;
    uint32_t tail=flow->values%64;
    if (!tail) return XR_XIR_OK;
    if (!xir_compile_work(flow->read->work,5+2*sizeof(uint64_t))) return XR_XIR_BUDGET;
    uint64_t extra=~((UINT64_C(1)<<tail)-1);
    return ((flow->fixed[words-1]|flow->certified[words-1])&extra)?
        XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}
static XrXirStatus effect_invocation_flag_get(EffectInvocationFlow *flow,
    uint32_t value,bool certified,bool *output) {
    if (!flow || !flow->read || !output || value>=flow->values ||
        !flow->physical || flow->values!=flow->physical->values ||
        !flow->fixed || !flow->certified) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->read->work,4+sizeof(*output))) return XR_XIR_BUDGET;
    const uint64_t *flags=certified?flow->certified:flow->fixed;
    *output=(flags[value/64]&(UINT64_C(1)<<(value%64)))!=0;return XR_XIR_OK;
}
static XrXirStatus effect_invocation_flag_put(EffectInvocationFlow *flow,
    uint32_t value,bool certified,bool *changed) {
    if (!flow || !flow->read || value>=flow->values || !flow->physical ||
        flow->values!=flow->physical->values || !flow->fixed || !flow->certified)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->read->work,5+sizeof(uint64_t))) return XR_XIR_BUDGET;
    uint64_t *flags=certified?flow->certified:flow->fixed;
    uint64_t bit=UINT64_C(1)<<(value%64);
    if (changed && !(flags[value/64]&bit)) {
        if (!xir_compile_work(flow->read->work,sizeof(*changed))) return XR_XIR_BUDGET;
        *changed=true;
    }
    flags[value/64]|=bit;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_value_matches(EffectInvocationFlow *flow,
    XrXirType expected,XrXirType actual,XrXirTypeMatchScratch *scratch) {
    const EffectInvocationRead *owner=flow->read;
    if (xr_xir_callable_signature(flow->body.domain.types,expected)) {
        XirEffectCallableBound request={flow->body.domain.types,flow->body.domain.types,
            NULL,0,expected,actual};
        if (xr_xir_type_span(flow->body.domain.types,expected) ||
            xr_xir_type_span(flow->body.domain.types,actual)) {
            if (flow->node>=owner->final->count) return XR_XIR_BAD_STRUCTURE;
            return effect_callable_bound_metadata_scratch(owner->work,&request,&flow->body.domain,
                owner->final->nodes[flow->node].body,scratch);
        }
        return effect_callable_bound_matches_scratch(owner->work,&request,scratch);
    }
    return xr_xir_compile_type_substitution_matches_between_scratch(owner->work,
        flow->body.domain.types,flow->body.domain.types,NULL,0,expected,actual,scratch);
}

static XrXirStatus effect_invocation_flow_head(EffectInvocationFlow *flow,
    uint32_t f,EffectBodyFunctionHead *output) {
    return effect_body_request_head(&flow->body,f,output);
}

static XrXirStatus effect_invocation_origin_bit(EffectInvocationFlow *flow,
    uint32_t value,uint32_t bit) {
    if (value>=flow->values || bit/64>=flow->basis.origin_words) return XR_XIR_BAD_VALUE;
    if (!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
    uint64_t *row=effect_invocation_row(flow,value);
    if (!row) return XR_XIR_BAD_TYPE;
    uint64_t *word=row+bit/64;
    uint64_t joined=*word|(UINT64_C(1)<<(bit%64));
    if (joined!=*word) flow->changed=true;
    *word=joined;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_origin_join(EffectInvocationFlow *flow,
    uint32_t destination,uint32_t source) {
    if (destination>=flow->values || source>=flow->values) return XR_XIR_BAD_VALUE;
    uint64_t *to=effect_invocation_row(flow,destination);
    const uint64_t *from=effect_invocation_row(flow,source);
    if (!to || !from) {
        /* Scalar/aggregate aliases retain their full fixed flags below. A
         * genuine callable with missing storage is never an empty origin. */
        XrXirType destination_type=0,source_type=0;
        XrXirStatus status=effect_invocation_flow_type(flow,destination,&destination_type);
        if (status==XR_XIR_OK) status=effect_invocation_flow_type(flow,source,&source_type);
        if (status!=XR_XIR_OK) return status;
        if (xr_xir_callable_signature(flow->body.domain.types,destination_type) ||
            xr_xir_callable_signature(flow->body.domain.types,source_type)) return XR_XIR_BAD_STRUCTURE;
        return XR_XIR_OK;
    }
    for (uint32_t w=0;w<flow->basis.fn_words;++w) {
        if (!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
        uint64_t joined=to[w]|from[w];
        if (joined!=to[w]) flow->changed=true;
        to[w]=joined;
    }
    return XR_XIR_OK;
}

/* Opaque masks are origin facts, not the target's physical type identity.
 * Carrying them in the fixed basis prevents a temporary None or a narrower
 * receiver descriptor from erasing a genuine producer UNKNOWN contribution. */
static XrXirStatus effect_invocation_opaque(EffectInvocationFlow *flow,
    uint32_t value,bool force_unknown) {
    if (value>=flow->values) return XR_XIR_BAD_VALUE;
    XrXirType actual_type=0;
    XrXirStatus read_status=effect_invocation_flow_type(flow,value,&actual_type);
    if (read_status!=XR_XIR_OK) return read_status;
    const XrXirTypeNode *signature=xr_xir_callable_signature(flow->body.domain.types,actual_type);
    if (!signature) return XR_XIR_OK;
    uint32_t mask=0;
    XrXirStatus status=effect_invocation_bounds_mask(flow->read->work,flow->read->declared,
        flow->read->final->nodes[flow->node].body,value,&mask);
    if (status!=XR_XIR_OK) return status;
    uint32_t opaque=flow->read->final->site_count+flow->basis.parameters;
    status=effect_invocation_origin_bit(flow,value,opaque);
    if (status==XR_XIR_OK && (mask&XR_XIR_CALLABLE_ROOT_REQUIRED))
        status=effect_invocation_origin_bit(flow,value,opaque+1);
    if (status==XR_XIR_OK && (force_unknown || (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)))
        status=effect_invocation_origin_bit(flow,value,opaque+2);
    return status;
}

static XrXirStatus effect_invocation_ref_target(EffectInvocationFlow *flow,
    const XrXirInstruction *op,bool *authentic) {
    if (!op || !authentic) return XR_XIR_BAD_STRUCTURE;
    const XirMetadataDomain *module=&flow->body.domain;
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,op->type);
    *authentic=false;
    if (!signature || op->immediate<0 || (uint64_t)op->immediate>=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead target_head={0};
    XrXirStatus head_status=effect_invocation_flow_head(flow,(uint32_t)op->immediate,&target_head);
    if (head_status!=XR_XIR_OK) return head_status;
    const EffectBodyFunctionHead *target=&target_head;
    if (op->args[1]>target->parameter_count ||
        signature->parameter_count!=target->parameter_count-op->args[1] ||
        op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_TYPE;
    for (uint32_t p=0;p<signature->parameter_count;++p) {
        if (!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
        const XrXirCallableParameter *parameter=&signature->parameters[p];
        if (!xr_xir_callable_parameter_storage_valid(module->types,parameter)) return XR_XIR_BAD_TYPE;
        if (xr_xir_type_span(module->types,parameter->type) ||
            xr_xir_type_span(module->types,target->parameters[p+op->args[1]])) return XR_XIR_OK;
        if (parameter->mode==XR_PARAM_REF) {
            if (op->args[1]) return XR_XIR_BAD_TYPE;
            if (xr_xir_cell_provenance_role(flow->read->final->cells,
                    (uint32_t)op->immediate,p)!=XR_XIR_CELL_PROOF_SCOPED_REF) return XR_XIR_OK;
        }
    }
    if (xr_xir_type_span(module->types,signature->result) ||
        xr_xir_type_span(module->types,target->result)) return XR_XIR_OK;
    XrXirTypeMatchScratch scratch={flow->read->work->resources,NULL};
    XrXirStatus status=xr_xir_compile_type_substitution_matches_between_scratch(flow->read->work,
        module->types,module->types,NULL,0,signature->result,target->result,&scratch);
    for (uint32_t p=0;p<signature->parameter_count && status==XR_XIR_OK;++p)
        status=xr_xir_compile_type_substitution_matches_between_scratch(flow->read->work,
            module->types,module->types,NULL,0,signature->parameters[p].type,target->parameters[p+op->args[1]],&scratch);
    for (uint32_t p=0;p<op->args[1] && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(flow->read->work,2)) { status=XR_XIR_BUDGET;break; }
        uint32_t value=flow->function->operands[op->args[0]+p];
        if (value>=flow->values) { status=XR_XIR_BAD_VALUE;break; }
        if (xr_xir_type_span(module->types,target->parameters[p])) {
            xr_xir_type_match_scratch_free(&scratch);return XR_XIR_OK;
        }
        XrXirType actual_type=0;status=effect_invocation_flow_type(flow,value,&actual_type);
        if (status==XR_XIR_OK)
            status=effect_invocation_value_matches(flow,target->parameters[p],actual_type,&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status==XR_XIR_OK) *authentic=true;
    return status;
}

static XrXirStatus effect_invocation_site(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    uint32_t body=flow->read->final->nodes[flow->node].body,selected=UINT32_MAX;
    for (uint32_t s=0;s<flow->read->final->site_count;++s) {
        if (!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
        const EffectInvocationSite *site=&flow->read->final->sites[s];
        if (site->function!=body || site->instruction!=instruction) continue;
        if (selected!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        selected=s;
    }
    if (selected==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    *output=selected;return XR_XIR_OK;
}

/* A captured descriptor carries a flat, finite relation for every original
 * producer site. It does not recursively copy another descriptor's captures.
 * Equal producer sites encountered through PHI or recursion only OR rows. */
static XrXirStatus effect_invocation_capture(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op) {
    uint32_t destination=flow->function->parameter_count+instruction;
    bool certified=false;
    XrXirStatus status=effect_invocation_flag_get(flow,destination,true,&certified);
    if (status!=XR_XIR_OK || !certified) return status;
    uint32_t selected=UINT32_MAX;
    status=effect_invocation_site(flow,instruction,&selected);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationSite *site=&flow->read->final->sites[selected];
    if (!op) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead target_head={0};
    XrXirStatus head_status=effect_invocation_flow_head(flow,site->target,&target_head);
    if (head_status!=XR_XIR_OK) return head_status;
    const EffectBodyFunctionHead *target=&target_head;
    if (site->captures!=op->args[1] || site->target!=(uint32_t)op->immediate ||
        site->binding>flow->read->final->binding_count ||
        site->captures>flow->read->final->binding_count-site->binding) return XR_XIR_BAD_STRUCTURE;
    status=effect_invocation_origin_bit(flow,destination,selected);
    uint64_t *descriptor=effect_invocation_row(flow,destination);
    if (!descriptor) return XR_XIR_BAD_STRUCTURE;
    uint32_t binding_words=flow->basis.origin_words+flow->basis.cell_words;
    for (uint32_t p=0;p<site->captures && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
        uint32_t value=flow->function->operands[op->args[0]+p];
        if (value>=flow->values) return XR_XIR_BAD_VALUE;
        uint64_t *binding=descriptor+flow->basis.origin_words+
            (size_t)(site->binding+p)*binding_words;
        if (xr_xir_type_is_cell(flow->body.domain.types,target->parameters[p])) {
            uint64_t *cell=binding+flow->basis.origin_words;
            /* Capture Cell facts come only from the shared real producer proof. */
            status=effect_invocation_cell(flow,value,cell,&flow->changed);
        } else if (xr_xir_callable_signature(flow->body.domain.types,target->parameters[p])) {
            const uint64_t *source=effect_invocation_row(flow,value);
            if (!source) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t w=0;w<flow->basis.origin_words;++w) {
                if (!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
                uint64_t joined=binding[w]|source[w];
                if (joined!=binding[w]) flow->changed=true;
                binding[w]=joined;
            }
            for (uint32_t w=flow->basis.origin_words;w<flow->basis.fn_words;++w) {
                if (!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
                uint64_t joined=descriptor[w]|source[w];
                if (joined!=descriptor[w]) flow->changed=true;
                descriptor[w]=joined;
            }
        }
    }
    return status;
}

static XrXirStatus effect_invocation_fixed(EffectInvocationFlow *flow,uint32_t value) {
    if (value>=flow->values) return XR_XIR_BAD_VALUE;
    XrXirStatus status=effect_invocation_flag_put(flow,value,false,&flow->changed);
    return status==XR_XIR_OK?effect_invocation_opaque(flow,value,false):status;
}

static XrXirStatus effect_invocation_barriers(EffectInvocationFlow *flow) {
    const XirMetadataDomain *module=&flow->body.domain;
    for (uint32_t r=0;r<flow->physical->count;++r) {
        if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        if (!(flow->physical->records[r].mask&EFFECT_PHYSICAL_BARRIER)) continue;
        uint32_t physical_value=flow->physical->records[r].value;
        if (physical_value<flow->function->parameter_count || physical_value>=flow->values) return XR_XIR_BAD_STRUCTURE;
        uint32_t i=physical_value-flow->function->parameter_count;
        XrXirInstruction body_op_3;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_3=effect_invocation_flow_instruction(flow,i,&body_op_3,&op);
        if (body_read_3!=XR_XIR_OK) return body_read_3;
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        bool store=op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_WRITE ||
            op->op==XR_XIR_CELL_LOCAL_WRITE || op->op==XR_XIR_PLACE_WRITE ||
            op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_GROUP_INIT ||
            op->op==XR_XIR_SLOT_STORE || op->op==XR_XIR_MAP_NEW || op->op==XR_XIR_SET_NEW || op->op==XR_XIR_MAP_SET || op->op==XR_XIR_SET_ADD || op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_SET ||
            op->op==XR_XIR_ARRAY_PUSH || op->op==XR_XIR_STRUCT_NEW || op->op==XR_XIR_STRUCT_SET ||
            op->op==XR_XIR_CLASS_NEW || op->op==XR_XIR_CLASS_SET || op->op==XR_XIR_ENUM_NEW ||
            op->op==XR_XIR_TUPLE_NEW || op->op==XR_XIR_NULLABLE_SOME;
        if (!store) continue;
        bool table=xr_xir_op_uses_operand_table(op->op);
        uint32_t count=table ? op->args[1] : effect_parameter_operands[op->op];
        if (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE) {
            XrXirType cell=op->type;
            if (op->op!=XR_XIR_CELL_NEW) {
                XrXirStatus status=effect_invocation_flow_type(flow,op->args[0],&cell);
                if (status!=XR_XIR_OK) return status;
            }
            count=xr_xir_cell_payload_operands(module->types,cell,op->op);
        } else if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
            if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
            count=xr_xir_slot_payload_operands(module->declarations->slots,
                module->declarations->slot_count,op);
        }
        if (table && ((count && !flow->function->operands) ||
            op->args[0]>flow->function->operand_count ||
            count>flow->function->operand_count-op->args[0])) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=0;a<count;++a) {
            if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
            uint32_t value=table ? flow->function->operands[op->args[0]+a] : op->args[a];
            XrXirStatus status=effect_invocation_fixed(flow,value);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_transfer(EffectInvocationFlow *flow,
    uint32_t destination,uint32_t source) {
    XrXirStatus status=effect_invocation_origin_join(flow,destination,source);
    if (status!=XR_XIR_OK) return status;
    bool fixed=false;
    if (status==XR_XIR_OK) status=effect_invocation_flag_get(flow,source,false,&fixed);
    if (status==XR_XIR_OK && fixed) return effect_invocation_fixed(flow,destination);
    return status;
}

/* Returned identities supplement the original opaque advertisement. Only
 * ordinary NONE return signatures participate; REF/UNKNOWN/fixed barriers
 * retain their original bounds and cannot acquire a permission here. */
static XrXirStatus effect_invocation_return_origins(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op) {
    const EffectInvocationRead *owner=flow->read;
    if (!owner->producer_enabled) return XR_XIR_OK;
    if (!op) return XR_XIR_BAD_STRUCTURE;
    uint32_t frontier=instruction,destination=flow->function->parameter_count+instruction;
    if (op->op==XR_XIR_INVOKE_RESULT) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        frontier=(uint32_t)op->immediate;
    } else if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_DEFAULT && op->op!=XR_XIR_CALL_INDIRECT)
        return XR_XIR_OK;
    if (!xr_xir_callable_signature(flow->body.domain.types,op->type)) return XR_XIR_OK;
    for (uint32_t e=owner->final->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->final->edges[e].next) {
        if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
        if (e>=owner->final->edge_count || owner->final->edges[e].caller!=flow->node ||
            owner->final->edges[e].target>=owner->final->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge edge=owner->final->edges[e];
        if (edge.latent || edge.instruction!=frontier) continue;
        const EffectInvocationNode *child=&owner->final->nodes[edge.target];
        EffectBodyFunctionHead target_head={0};
        XrXirStatus head_status=effect_invocation_flow_head(flow,child->body,&target_head);
        if (head_status!=XR_XIR_OK) return head_status;
        const EffectBodyFunctionHead *target=&target_head;
        uint64_t returned=(uint64_t)target->parameter_count+target->instruction_count;
        for (uint32_t p=0;p<child->producer_count;++p) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            if (child->producers[p].value!=returned) continue;
            const EffectInvocationProducer *fact=&child->producers[p];
            XrXirStatus status=effect_invocation_origin_bit(flow,destination,fact->site);
            if (status!=XR_XIR_OK) return status;
            if (fact->atom) {
                EffectInvocationColumn column={0};
                status=effect_invocation_producer_column_read(owner,&flow->body,&flow->basis,fact->site,fact->atom,&column);
                if (status!=XR_XIR_OK) return status;
                if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                uint64_t *row=effect_invocation_row(flow,destination);
                if (!row) return XR_XIR_BAD_STRUCTURE;
                uint64_t *word=row+column.word;
                uint64_t joined=*word|(UINT64_C(1)<<column.bit);
                if (joined!=*word) flow->changed=true;
                *word=joined;
            }
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_origins_decoded(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op) {
    uint32_t destination=flow->function->parameter_count+instruction;
    switch (op->op) {
    case XR_XIR_CALL: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_RESULT:
        return effect_invocation_return_origins(flow,instruction,op);
    case XR_XIR_FUNCTION_REF:
        return effect_invocation_capture(flow,instruction,op);
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
        return effect_invocation_transfer(flow,destination,op->args[0]);
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return effect_invocation_transfer(flow,op->args[0],op->args[1]);
    case XR_XIR_PHI:
        if (!op->args[1] || op->args[1]%2 || !flow->function->operands ||
            op->args[0]>flow->function->operand_count ||
            op->args[1]>flow->function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=1;a<op->args[1];a+=2) {
            XrXirStatus status=effect_invocation_transfer(flow,destination,
                flow->function->operands[op->args[0]+a]);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    default: return XR_XIR_OK;
    }
}

static XrXirStatus effect_invocation_origins_step(EffectInvocationFlow *flow,
    uint32_t instruction) {
    if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus status=effect_invocation_flow_instruction(flow,instruction,&storage,&op);
    return status==XR_XIR_OK?effect_invocation_origins_decoded(flow,instruction,op):status;
}

/* This is only the original origin transfer's early-return condition. It
 * neither clears a carrier row nor authorizes execution or ROOT permission.
 * INVOKE_RESULT keeps its independent physical frontier check when enabled. */
static bool effect_invocation_origins_unused(const EffectInvocationFlow *flow,
    XrXirOp op,XrXirType type) {
    switch (op) {
    case XR_XIR_CALL: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL_INDIRECT:
        return !flow->read->producer_enabled ||
            !xr_xir_callable_signature(flow->body.domain.types,type);
    case XR_XIR_INVOKE_RESULT:
        return !flow->read->producer_enabled;
    case XR_XIR_FUNCTION_REF:
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
    case XR_XIR_PHI:
        return false;
    default: return true;
    }
}

/* Each fixed-point pass independently reaches current canonical EOF. Only
 * actual constant opcode/type fields share an early-return classification;
 * every active physical transfer enters the unchanged complete handler. */
static XrXirStatus effect_invocation_origins_pass(EffectInvocationFlow *flow) {
    const EffectInvocationRead *owner=flow->read;
    if (flow->body.instruction!=effect_body_owned_instruction ||
        flow->body.head!=effect_body_owned_head || flow->body.type!=effect_body_owned_type ||
        flow->body.shape!=effect_body_owned_shape ||
        flow->body.read.consume!=effect_body_compile_consume) {
        for (uint32_t r=0;r<flow->physical->count;++r) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=flow->physical->records[r];
            if (!(record.mask&EFFECT_PHYSICAL_TRANSFER)) continue;
            if (record.value<flow->function->parameter_count || record.value>=flow->values)
                return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=effect_invocation_origins_step(flow,record.value-flow->function->parameter_count);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    const EffectBodyMaterial *material=flow->body.owner;
    uint32_t f=owner->final->nodes[flow->node].body;
    if (material!=owner->final->material || !xir_compile_context_valid(owner->work) ||
        !effect_body_material_shape(material) ||
        material->resources!=owner->work->resources || f>=effect_body_count(material) ||
        flow->body.read.owner!=owner->work || flow->body.read.consume!=effect_body_compile_consume ||
        flow->body.domain.types!=material->domain.types ||
        flow->body.domain.declarations!=material->domain.declarations ||
        flow->body.domain.generics!=material->domain.generics ||
        flow->body.domain.defaults!=material->domain.defaults ||
        flow->body.domain.function_count!=effect_body_count(material) || !material->physical ||
        material->physical->count!=effect_body_count(material) || !material->physical->functions ||
        owner->final->physical!=material->physical ||
        flow->physical!=&material->physical->functions[f]) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *body=&material->functions[f];
    if (body->parameter_count!=flow->function->parameter_count ||
        body->instruction_count!=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};uint32_t r=0;
    if (!xir_compile_work(owner->work,sizeof(cursor)+sizeof(r))) return XR_XIR_BUDGET;
    while (r<flow->physical->count && flow->physical->records[r].value<flow->function->parameter_count) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (flow->physical->records[r++].mask&EFFECT_PHYSICAL_TRANSFER) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t run=0;run<body->run_count;++run) {
        const EffectBodyRun *ref;uint32_t opcode=XR_XIR_INVALID,type=XR_XIR_UNIT;
        if (!xir_compile_work(owner->work,2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        XrXirStatus status=effect_body_range_ref(&flow->body.read,body,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        bool constant=!(ref->mask&UINT32_C(3)),unused=false;
        if (!xir_compile_work(owner->work,sizeof(constant)+sizeof(unused))) return XR_XIR_BUDGET;
        if (constant) {
            status=effect_body_run_word(&flow->body.read,ref,ref->first,0,&opcode);
            if (status==XR_XIR_OK) status=effect_body_run_word(&flow->body.read,ref,ref->first,1,&type);
            if (status!=XR_XIR_OK) return status;
            if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            unused=effect_invocation_origins_unused(flow,(XrXirOp)opcode,(XrXirType)type);
        }
        while (r<flow->physical->count &&
            flow->physical->records[r].value-flow->function->parameter_count<ref->first+ref->count) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=flow->physical->records[r++];
            if (record.value<flow->function->parameter_count || record.value>=flow->values ||
                record.value-flow->function->parameter_count<ref->first) return XR_XIR_BAD_STRUCTURE;
            if (!(record.mask&EFFECT_PHYSICAL_TRANSFER)) continue;
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            uint32_t i=record.value-flow->function->parameter_count;
            if (!constant) {
                status=effect_body_run_word(&flow->body.read,ref,i,0,&opcode);
                if (status==XR_XIR_OK) status=effect_body_run_word(&flow->body.read,ref,i,1,&type);
                if (status!=XR_XIR_OK) return status;
                if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
                unused=effect_invocation_origins_unused(flow,(XrXirOp)opcode,(XrXirType)type);
            }
            if (unused) continue;
            XrXirInstruction op;
            status=effect_body_instruction_ref_value(&flow->body.read,ref,i,&op);
            if (status==XR_XIR_OK) status=effect_invocation_origins_decoded(flow,i,&op);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return r==flow->physical->count && cursor.first==body->instruction_count &&
        cursor.run==body->run_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* Classification locates a real FnRef, whose complete row is still read.
 * Other initial callable values consume only opcode here; the shape reader
 * retains full canonical run checks without constructing an unused row. */
static XrXirStatus effect_invocation_initial_origins(EffectInvocationFlow *flow) {
    const EffectInvocationRead *owner=flow->read;XrXirStatus status=XR_XIR_OK;
    for (uint32_t r=0;r<flow->physical->count && status==XR_XIR_OK;++r) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_CALLABLE) || record.value<flow->function->parameter_count) continue;
        if (record.value>=flow->values) return XR_XIR_BAD_STRUCTURE;
        uint32_t destination=record.value,i=destination-flow->function->parameter_count;
        if (record.mask&EFFECT_PHYSICAL_REFERENCE) {
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_invocation_flow_instruction(flow,i,&storage,&op);
            if (status!=XR_XIR_OK) return status;
            if (op->op!=XR_XIR_FUNCTION_REF) return XR_XIR_BAD_STRUCTURE;
            bool authentic=false;
            status=effect_invocation_ref_target(flow,op,&authentic);
            if (status==XR_XIR_OK && authentic) status=effect_invocation_flag_put(flow,destination,true,NULL);
            else if (status==XR_XIR_OK) status=effect_invocation_opaque(flow,destination,true);
        } else {
            EffectBodyInstructionShape shape={0};
            status=effect_body_request_shape(&flow->body,owner->final->nodes[flow->node].body,i,&shape);
            if (status!=XR_XIR_OK) return status;
            if (shape.op==XR_XIR_FUNCTION_REF) return XR_XIR_BAD_STRUCTURE;
            if (shape.op!=XR_XIR_COPY && shape.op!=XR_XIR_LOCAL_NEW &&
                shape.op!=XR_XIR_LOCAL_READ && shape.op!=XR_XIR_PHI &&
                shape.op!=XR_XIR_SCALAR_COPY && shape.op!=XR_XIR_OWNED_RETAIN &&
                shape.op!=XR_XIR_SCALAR_LOCAL_NEW && shape.op!=XR_XIR_SCALAR_LOCAL_READ &&
                shape.op!=XR_XIR_OWNED_LOCAL_NEW && shape.op!=XR_XIR_OWNED_LOCAL_READ)
                status=effect_invocation_opaque(flow,destination,false);
        }
    }
    return status;
}

static XrXirStatus effect_invocation_origins_read(EffectInvocationFlow *flow,
    const EffectBodyRequest *request) {
    const EffectInvocationRead *owner=flow->read;
    if (!request || flow->node>=owner->final->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,sizeof(flow->body))) return XR_XIR_BUDGET;
    flow->body=*request;
    if (!xir_compile_work(owner->work,2*sizeof(uint32_t)+sizeof(const uint64_t *))) return XR_XIR_BUDGET;
    uint32_t key_body=owner->final->nodes[flow->node].body,key_root=owner->final->nodes[flow->node].root;
    const uint64_t *key_input=owner->final->nodes[flow->node].input;
    XrXirStatus status=effect_invocation_flow_head(flow,key_body,&flow->head);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
    uint32_t root_parameters=flow->head.parameter_count;
    if (key_root!=key_body) {
        EffectBodyFunctionDimensions root={0};
        status=effect_body_request_dimensions(&flow->body,key_root,&root);
        if (status!=XR_XIR_OK) return status;
        root_parameters=root.parameter_count;
    }
    flow->function=&flow->head;
    status=effect_invocation_basis_parameters(owner,root_parameters,&flow->basis);
    if (status!=XR_XIR_OK) return status;
    status=effect_invocation_flow_storage(flow);
    if (status!=XR_XIR_OK) return status;
    for (uint32_t p=0;p<flow->function->parameter_count;++p) {
        uint64_t *row=effect_invocation_row(flow,p);
        const uint64_t *input=key_input+(size_t)p*flow->basis.row_words;
        if (!row) {
            /* The physical key remains complete. Non-callable formals must
             * not smuggle a callable producer through omitted storage. */
            for (uint32_t w=0;w<flow->basis.fn_words;++w) {
                if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
                if (input[w]) return XR_XIR_BAD_STRUCTURE;
            }
            continue;
        }
        if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row)))
            return XR_XIR_BUDGET;
        memcpy(row,input,(size_t)flow->basis.fn_words*sizeof(*row));
    }
    status=effect_invocation_barriers(flow);
    if (status==XR_XIR_OK) status=effect_invocation_initial_origins(flow);
    flow->changed=true;
    while (status==XR_XIR_OK && flow->changed) {
        flow->changed=false;
        status=effect_invocation_origins_pass(flow);
    }
    /* Empty cyclic carrier rows have no producer certificate. Add UNKNOWN
     * before any execution result can consume the internal origin bottom. */
    for (uint32_t r=0;r<flow->physical->count && status==XR_XIR_OK;++r) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_CALLABLE)) continue;
        uint32_t v=record.value;
        const uint64_t *row=effect_invocation_row(flow,v);
        if (!row) return XR_XIR_BAD_STRUCTURE;
        bool present=false;
        for (uint32_t w=0;w<flow->basis.origin_words;++w) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (row[w]) present=true;
        }
        if (!present) status=effect_invocation_opaque(flow,v,true);
    }
    while (status==XR_XIR_OK && flow->changed) {
        flow->changed=false;
        status=effect_invocation_origins_pass(flow);
    }
    if (status==XR_XIR_OK) status=effect_invocation_flags_match(flow);
    return status;
}

/* One replay consumes the sole immutable owning representation. Complete
 * rows may be shared only within the current request; no queue visit retains
 * its stack instruction or derives authority from a previous read. */
static XrXirStatus effect_invocation_origins(EffectInvocationFlow *flow) {
    if (!flow || !flow->read || !effect_body_material_shape(flow->read->final->material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(flow->read->work,flow->read->final->material);
    return effect_invocation_origins_read(flow,&request);
}

#include "xxir_effect_invocation_class_cells.inc.c"

/* This operation consumes the common Cell proof. It does not infer a second
 * set of Cell roles or origins: each compact provider dependency is projected
 * through the already-owned physical input row of this exact equation. */
static XrXirStatus effect_invocation_cell(EffectInvocationFlow *flow,
    uint32_t value,uint64_t *output,bool *changed) {
    const EffectInvocationRead *owner=flow->read;
    if (flow->node>=owner->final->count || !output || value>=flow->values)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,sizeof(uint32_t)+sizeof(const uint64_t *))) return XR_XIR_BUDGET;
    uint32_t key_body=owner->final->nodes[flow->node].body;
    const uint64_t *key_input=owner->final->nodes[flow->node].input;
    XrXirCellOriginView view={0};
    XrXirStatus status=owner->class_enabled ? effect_invocation_class_cell_view(flow,value,&view) :
        xr_xir_compile_cell_origin_view(owner->work,owner->final->cells,key_body,value,&view);
    if (status!=XR_XIR_OK) return status;
    if (view.word_count!=((uint64_t)view.parameter_count+63)/64 ||
        (!!view.dependencies!=!!view.word_count) ||
        (!!view.parameters!=!!view.parameter_count) ||
        (view.intrinsic_mask&~(XR_XIR_CELL_ACCESS_ROOT|XR_XIR_CELL_ACCESS_UNKNOWN)))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    uint32_t known=flow->basis.parameters+2;
    uint64_t known_bit=UINT64_C(1)<<(known%64);
    if (changed && !(output[known/64]&known_bit)) *changed=true;
    output[known/64]|=known_bit;
    if (view.intrinsic_mask&XR_XIR_CELL_ACCESS_ROOT) {
        uint64_t root_bit=UINT64_C(1)<<(flow->basis.parameters%64);
        if (changed && !(output[flow->basis.parameters/64]&root_bit)) *changed=true;
        output[flow->basis.parameters/64]|=root_bit;
    }
    if (view.intrinsic_mask&XR_XIR_CELL_ACCESS_UNKNOWN) {
        uint32_t bit=flow->basis.parameters+1;
        uint64_t unknown_bit=UINT64_C(1)<<(bit%64);
        if (changed && !(output[bit/64]&unknown_bit)) *changed=true;
        output[bit/64]|=unknown_bit;
    }
    for (uint32_t bit=0;bit<view.parameter_count;++bit) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!(view.dependencies[bit/64]&(UINT64_C(1)<<(bit%64)))) continue;
        uint32_t parameter=view.parameters[bit];
        if (parameter>=flow->function->parameter_count ||
            !xr_xir_type_is_cell(flow->body.domain.types,flow->function->parameters[parameter]))
            return XR_XIR_BAD_STRUCTURE;
        const uint64_t *row=key_input+(size_t)parameter*flow->basis.row_words+flow->basis.fn_words;
        bool present=(row[known/64]&(UINT64_C(1)<<(known%64)))!=0;
        for (uint32_t w=0;w<flow->basis.cell_words;++w) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            uint64_t joined=output[w]|row[w];
            if (changed && joined!=output[w]) *changed=true;
            output[w]=joined;
        }
        /* An unbound physical formal cannot be interpreted as an owned root. */
        if (!present) {
            uint32_t unknown=flow->basis.parameters+1;
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            uint64_t unknown_bit=UINT64_C(1)<<(unknown%64);
            if (changed && !(output[unknown/64]&unknown_bit)) *changed=true;
            output[unknown/64]|=unknown_bit;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_cell_consume(EffectInvocationFlow *flow,
    const uint64_t *row,bool *changed) {
    const EffectInvocationRead *owner=flow->read;
    if (!row || !changed || flow->node>=owner->final->count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&owner->final->nodes[flow->node];
    uint32_t mask=0;
    XrXirStatus mask_status=effect_invocation_cell_mask(flow,row,&mask);
    if (mask_status!=XR_XIR_OK) return mask_status;
    if ((node->intrinsic_mask|mask)!=node->intrinsic_mask) *changed=true;
    node->intrinsic_mask|=mask;
    if (flow->instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction access_storage;const XrXirInstruction *access=NULL;
    XrXirStatus read_status=effect_invocation_flow_instruction(flow,flow->instruction,&access_storage,&access);
    if (read_status!=XR_XIR_OK) return read_status;
    for (uint32_t unknown_bit=0;unknown_bit<2;++unknown_bit) {
        uint32_t flag=unknown_bit?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (!(mask&flag)) continue;
        XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CELL_ACCESS,flow->instruction,UINT32_MAX,
            access->args[0],0};
        XrXirStatus status=effect_invocation_cell_local(flow,unknown_bit,witness,changed);
        if (status!=XR_XIR_OK) return status;
    }
    EffectBodyFunctionHead root_head={0};
    read_status=effect_invocation_flow_head(flow,node->root,&root_head);
    if (read_status!=XR_XIR_OK) return read_status;
    for (uint32_t p=0;p<flow->basis.parameters;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        uint64_t bit=UINT64_C(1)<<(p%64);
        if (!(row[p/64]&bit)) continue;
        if (!xr_xir_type_is_cell(flow->body.domain.types,root_head.parameters[p]))
            return XR_XIR_BAD_STRUCTURE;
        if (!(node->cells[p/64]&bit)) { node->cells[p/64]|=bit;*changed=true; }
        XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CELL_PARAMETER,flow->instruction,UINT32_MAX,p,0};
        XrXirStatus status=effect_invocation_cell_local(flow,flow->basis.parameters+p+2,witness,changed);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
