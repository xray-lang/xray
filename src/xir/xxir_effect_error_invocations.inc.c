/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_invocations.inc.c - Escaping errors on sealed actual invocation edges
 *
 * KEY CONCEPT:
 *   The existing error CFG interprets exact sealed inputs. Unknown callable
 *   alternatives remain unknown errors independently of their ROOT bound.
 */
typedef struct ErrorInvocationNode { uint32_t body,summary,calls; } ErrorInvocationNode;
typedef struct ErrorInvocationCall { uint32_t head; bool unknown,handled,frontier; } ErrorInvocationCall;
typedef struct ErrorInvocationLink { uint32_t caller,target,producer,next,consumer_next; } ErrorInvocationLink;
struct ErrorInvocationContexts {
    XrCompileResources *resources;
    uint32_t count,summaries,link_count,link_capacity,front,back,pending;
    ErrorInvocationNode *nodes;
    ErrorInvocationCall *calls;
    ErrorInvocationLink *links;
    uint32_t *consumers,*queue;
    uint8_t *queued;
};
_Static_assert(sizeof(ErrorInvocationContexts)%_Alignof(ErrorInvocationNode)==0 &&
    sizeof(ErrorInvocationNode)%_Alignof(ErrorInvocationCall)==0 &&
    sizeof(ErrorInvocationCall)%_Alignof(ErrorInvocationLink)==0 &&
    sizeof(ErrorInvocationLink)%_Alignof(uint32_t)==0,"Error context rows preserve alignment");

static void error_invocation_contexts_free(ErrorInvocationContexts *contexts) {
    xr_compile_resources_free(contexts);
}
static uint32_t error_invocation_summary(const ErrorInvocationContexts *contexts,uint32_t n) {
    return contexts->nodes[n].summary;
}

/* This internal operation consumes the certificate produced earlier in the
 * same full inference. Check complete copied IR/type correspondence, not a
 * source address, digest or same-number function identity. */
static XrXirStatus error_invocation_source_match(ErrorFlow *flow,
    const EffectInvocationCertificate *certificate) {
    const XrXirModule *source=flow->module,*copy=&certificate->bodies;
    if (certificate->resources!=flow->remaining->resources || source->stage!=copy->stage ||
        source->linkage_kind!=copy->linkage_kind || source->function_count!=copy->function_count ||
        !certificate->equations || !source->functions || !copy->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *types=source->types?source->types:&effect_empty_types;
    EffectTerms terms=certificate->terms;terms.remaining=flow->remaining;
    XrXirStatus status=source->stage==XR_XIR_LOWERED?
        effect_lowered_snapshot_match(flow->remaining,&certificate->terms.types,types):
        effect_terms_snapshot_match(&terms,types,types->count);
    for (uint32_t f=0;f<source->function_count && status==XR_XIR_OK;++f) {
        const XrXirFunction *a=&source->functions[f],*b=&copy->functions[f];
        if (!xir_compile_work(flow->remaining,7)) return XR_XIR_BUDGET;
        if (a->name_length!=b->name_length || a->parameter_count!=b->parameter_count ||
            a->result!=b->result || a->block_count!=b->block_count ||
            a->instruction_count!=b->instruction_count || a->operand_count!=b->operand_count)
            return XR_XIR_BAD_STRUCTURE;
        const void *left[5]={a->name,a->parameters,a->blocks,a->instructions,a->operands};
        const void *right[5]={b->name,b->parameters,b->blocks,b->instructions,b->operands};
        uint64_t bytes[5]={a->name_length,(uint64_t)a->parameter_count*sizeof(*a->parameters),
            (uint64_t)a->block_count*sizeof(*a->blocks),(uint64_t)a->instruction_count*sizeof(*a->instructions),
            (uint64_t)a->operand_count*sizeof(*a->operands)};
        for (uint32_t p=0;p<5;++p) {
            if (bytes[p]>SIZE_MAX) return XR_XIR_BUDGET;
            if (!xir_compile_work(flow->remaining,bytes[p]+1)) return XR_XIR_BUDGET;
            if (bytes[p] && (!left[p] || !right[p] || memcmp(left[p],right[p],(size_t)bytes[p])))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return status;
}

static XrXirStatus error_invocation_storage(ErrorFlow *flow,
    const EffectInvocationCertificate *certificate,ErrorInvocationContexts **output) {
    const EffectInvocationOwner *owner=certificate->equations;
    if (!owner->count || !owner->nodes || !owner->roots || !owner->root_spaces ||
        (owner->edge_count && !owner->edges)) return XR_XIR_BAD_STRUCTURE;
    uint64_t calls=0;
    for (uint32_t n=0;n<owner->count;++n) {
        if (!xir_compile_work(flow->remaining,2)) return XR_XIR_BUDGET;
        if (owner->nodes[n].body>=flow->module->function_count ||
            owner->nodes[n].root>=flow->module->function_count ||
            owner->nodes[n].parameter_count!=flow->module->functions[owner->nodes[n].body].parameter_count)
            return XR_XIR_BAD_STRUCTURE;
        calls+=flow->module->functions[owner->nodes[n].body].instruction_count;
        if (calls>UINT32_MAX) return XR_XIR_BUDGET;
    }
    uint64_t capacity=(uint64_t)owner->edge_count+calls;
    uint64_t bytes=sizeof(ErrorInvocationContexts)+(uint64_t)owner->count*
        (sizeof(ErrorInvocationNode)+2*sizeof(uint32_t)+sizeof(uint8_t))+
        calls*sizeof(ErrorInvocationCall)+capacity*sizeof(ErrorInvocationLink);
    if (bytes>SIZE_MAX || capacity>UINT32_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    ErrorInvocationContexts *contexts=xir_compile_calloc(flow->remaining,1,(size_t)bytes,&status);
    if (!contexts) return status;
    contexts->resources=flow->remaining->resources;contexts->count=owner->count;
    contexts->link_capacity=(uint32_t)capacity;
    contexts->nodes=(ErrorInvocationNode *)(contexts+1);
    contexts->calls=(ErrorInvocationCall *)(contexts->nodes+owner->count);
    contexts->links=(ErrorInvocationLink *)(contexts->calls+(size_t)calls);
    contexts->consumers=(uint32_t *)(contexts->links+(size_t)capacity);
    contexts->queue=contexts->consumers+owner->count;
    contexts->queued=(uint8_t *)(contexts->queue+owner->count);
    uint32_t at=0;
    for (uint32_t n=0;n<owner->count;++n) {
        if (!xir_compile_work(flow->remaining,4)) { status=XR_XIR_BUDGET;break; }
        contexts->nodes[n]=(ErrorInvocationNode){owner->nodes[n].body,UINT32_MAX,at};
        contexts->consumers[n]=UINT32_MAX;
        uint32_t count=flow->module->functions[owner->nodes[n].body].instruction_count;
        for (uint32_t i=0;i<count;++i) {
            if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;break; }
            contexts->calls[at++].head=UINT32_MAX;
        }
        if (status!=XR_XIR_OK) break;
    }
    for (uint32_t f=0;status==XR_XIR_OK && f<flow->module->function_count;++f) {
        if (!xir_compile_work(flow->remaining,3)) { status=XR_XIR_BUDGET;break; }
        uint32_t n=owner->roots[f];
        if (n>=owner->count || owner->root_spaces[f]>=flow->module->function_count ||
            owner->nodes[n].body!=f || owner->nodes[n].root!=owner->root_spaces[f] ||
            contexts->nodes[n].summary!=UINT32_MAX) { status=XR_XIR_BAD_STRUCTURE;break; }
        contexts->nodes[n].summary=f;
    }
    contexts->summaries=flow->module->function_count;
    for (uint32_t n=0;status==XR_XIR_OK && n<owner->count;++n) {
        if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;break; }
        if (contexts->nodes[n].summary!=UINT32_MAX) continue;
        if (contexts->summaries==UINT32_MAX) { status=XR_XIR_BUDGET;break; }
        contexts->nodes[n].summary=contexts->summaries++;
    }
    if (status!=XR_XIR_OK) { error_invocation_contexts_free(contexts);return status; }
    *output=contexts;return XR_XIR_OK;
}

/* Replay the original complete finite descriptor. Any opaque, symbolic or
 * unrooted alternative is still an unknown error even with a NONE ROOT mask.
 * Every concrete site must have exactly one real edge for this full input. */
static XrXirStatus error_invocation_indirect_check(EffectInvocationFlow *replay,
    const ErrorInvocationContexts *contexts,const EffectInvocationCoverage *coverage,
    uint32_t instruction,bool *unknown) {
    const XrXirInstruction *op=&replay->function->instructions[instruction];
    if (op->immediate<0 || (uint64_t)op->immediate>=replay->values) return XR_XIR_BAD_STRUCTURE;
    const uint64_t *row=effect_invocation_row(replay,(uint32_t)op->immediate);
    if (!row) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationOwner *owner=replay->owner;
    uint32_t opaque=owner->site_count+replay->basis.parameters;
    bool unresolved=false,present=false;
    for (uint32_t bit=owner->site_count;bit<=opaque+2;++bit) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        /* The original ROOT advertisement stays intact. Only independently
         * certified exhaustive targets discharge its error-only residual. */
        if (bit==opaque && coverage->ranks && coverage->ranks[(uint32_t)op->immediate]) continue;
        if (row[bit/64]&(UINT64_C(1)<<(bit%64))) unresolved=true;
    }
    for (uint32_t s=0;s<owner->site_count;++s) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!(row[s/64]&(UINT64_C(1)<<(s%64)))) continue;
        present=true;uint32_t matched=0;
        const ErrorInvocationCall *call=&contexts->calls[contexts->nodes[replay->node].calls+instruction];
        for (uint32_t e=call->head;e!=UINT32_MAX;e=contexts->links[e].next) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            if (e>=contexts->link_count) return XR_XIR_BAD_STRUCTURE;
            if (contexts->links[e].producer==s) ++matched;
        }
        if (matched!=1) return XR_XIR_BAD_STRUCTURE;
    }
    *unknown=unresolved || !present;return XR_XIR_OK;
}

/* Validate the complete physical argument relation through the original
 * read-only argument builder. Replaying inputs never interns a new node or
 * changes the sealed call graph, including latent GO transport edges. */
static XrXirStatus error_invocation_input_match(EffectInvocationFlow *replay,
    const EffectInvocationCertificate *certificate,uint32_t e,bool *matched) {
    *matched=false;
    XrXirStatus status=effect_invocation_certificate_edge(replay->owner->work,certificate,replay->node,e);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationEdge edge=certificate->equations->edges[e];
    const EffectInvocationNode *target=&certificate->equations->nodes[edge.target];
    const XrXirInstruction *op=&replay->function->instructions[edge.instruction];
    if (target->parameter_count!=certificate->bodies.functions[target->body].parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CLEANUP_REGISTER) {
        const XrXirDeclarations *declarations=certificate->bodies.declarations;
        if (!declarations || !declarations->functions ||
            declarations->functions[target->body].cleanup_owner!=
                certificate->equations->nodes[replay->node].body+1)
            return XR_XIR_BAD_STRUCTURE;
    }
    if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        if (target->parameter_count || target->input) return XR_XIR_BAD_STRUCTURE;
        *matched=true;return XR_XIR_OK;
    }
    const uint64_t *closure=NULL;
    if (edge.producer!=UINT32_MAX) {
        if (op->immediate<0 || (uint64_t)op->immediate>=replay->values) return XR_XIR_BAD_STRUCTURE;
        closure=effect_invocation_row(replay,(uint32_t)op->immediate);
        if (!closure) return XR_XIR_BAD_STRUCTURE;
    }
    uint64_t *input=NULL;replay->instruction=edge.instruction;
    status=effect_invocation_arguments(replay,edge.instruction,target->body,edge.producer,closure,&input);
    uint64_t words=(uint64_t)target->parameter_count*replay->basis.row_words;
    if (status==XR_XIR_OK && words>SIZE_MAX/sizeof(*input)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK && words && (!input || !target->input)) status=XR_XIR_BAD_STRUCTURE;
    bool same=true;
    for (size_t w=0;status==XR_XIR_OK && w<(size_t)words;++w) {
        if (!xir_compile_work(replay->owner->work,2)) { status=XR_XIR_BUDGET;break; }
        if (input[w]!=target->input[w]) same=false;
    }
    xr_compile_resources_free(input);if (status==XR_XIR_OK) *matched=same;return status;
}

static XrXirStatus error_invocation_link(ErrorFlow *flow,ErrorInvocationContexts *contexts,
    const EffectInvocationCertificate *certificate,EffectInvocationFlow *replay,uint32_t e) {
    const EffectInvocationOwner *owner=certificate->equations;
    EffectInvocationEdge edge=owner->edges[e];
    if (edge.caller>=contexts->count || edge.target>=contexts->count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&flow->module->functions[contexts->nodes[edge.caller].body];
    if (edge.instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&function->instructions[edge.instruction];
    if (op->op==XR_XIR_FUNCTION_REF) return XR_XIR_OK; /* Latent construction does not execute. */
    if (edge.caller!=replay->node) return XR_XIR_BAD_STRUCTURE;
    ErrorInvocationCall *call=&contexts->calls[contexts->nodes[edge.caller].calls+edge.instruction];
    call->frontier=true;bool matched=false;
    XrXirStatus status=error_invocation_input_match(replay,certificate,e,&matched);
    if (status!=XR_XIR_OK) return status;
    /* Producer closure grows monotonically. Earlier edges keep smaller owned
     * input keys; only the current complete relation executes in this row. */
    if (!matched) return XR_XIR_OK;
    if (contexts->link_count>=contexts->link_capacity) return XR_XIR_BAD_STRUCTURE;
    uint32_t link=contexts->link_count++;
    if (!xir_compile_work(flow->remaining,6)) return XR_XIR_BUDGET;
    contexts->links[link]=(ErrorInvocationLink){edge.caller,edge.target,edge.producer,
        call->head,contexts->consumers[edge.target]};
    call->head=link;call->handled=true;contexts->consumers[edge.target]=link;
    return XR_XIR_OK;
}

/* An open generic direct call keeps its original conservative public summary
 * and typed substitution. Its error dependency still wakes the actual caller;
 * this link is not an invocation/ROOT edge or a callable authority proof. */
static XrXirStatus error_invocation_fallback(ErrorFlow *flow,ErrorInvocationContexts *contexts,
    const EffectInvocationCertificate *certificate,uint32_t n,uint32_t instruction) {
    ErrorInvocationCall *call=&contexts->calls[contexts->nodes[n].calls+instruction];
    if (call->handled) return XR_XIR_OK;
    const XrXirInstruction *op=&flow->module->functions[contexts->nodes[n].body].instructions[instruction];
    uint32_t target=UINT32_MAX;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_GO)
        target=op->immediate<0 || (uint64_t)op->immediate>=flow->effects->count?UINT32_MAX:(uint32_t)op->immediate;
    else if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xr_xir_compile_default_lookup(flow->remaining,flow->module,identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding || binding->function>=flow->effects->count) return XR_XIR_BAD_STRUCTURE;
        target=binding->function;
    } else return XR_XIR_OK;
    if (target>=flow->effects->count || contexts->link_count>=contexts->link_capacity)
        return XR_XIR_BAD_STRUCTURE;
    target=certificate->equations->roots[target];
    if (target>=contexts->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t link=contexts->link_count++;
    if (!xir_compile_work(flow->remaining,5)) return XR_XIR_BUDGET;
    contexts->links[link]=(ErrorInvocationLink){n,target,UINT32_MAX,call->head,contexts->consumers[target]};
    call->head=link;contexts->consumers[target]=link;return XR_XIR_OK;
}

static XrXirStatus error_invocation_contexts_new(ErrorFlow *flow,ErrorInvocationContexts **output) {
    if (!flow || !flow->effects || !output || *output) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=flow->effects->invocations;
    if (!certificate) return XR_XIR_OK;
    if (!xir_effects_context_matches(flow->remaining,flow->effects,flow->module->function_count) ||
        certificate->resources!=flow->remaining->resources || !certificate->equations)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationOwner *sealed=certificate->equations;
    if (!sealed->site_count) return XR_XIR_OK;
    bool indirect=false;
    for (uint32_t f=0;f<flow->module->function_count && !indirect;++f)
        for (uint32_t i=0;i<flow->module->functions[f].instruction_count;++i) {
            if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
            XrXirOp op=flow->module->functions[f].instructions[i].op;
            if (op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT) { indirect=true;break; }
        }
    if (!indirect) return XR_XIR_OK;
    XrXirStatus status=error_invocation_source_match(flow,certificate);
    ErrorInvocationContexts *contexts=NULL;
    if (status==XR_XIR_OK) status=error_invocation_storage(flow,certificate,&contexts);
    EffectInvocationOwner owner=*sealed;
    owner.work=flow->remaining;owner.module=&certificate->bodies;owner.effects=flow->effects;
    owner.declared=certificate->declared;
    for (uint32_t n=0;status==XR_XIR_OK && n<owner.count;++n) {
        EffectInvocationFlow replay={.owner=&owner,.node=n,.instruction=UINT32_MAX};
        status=effect_invocation_origins(&replay);
        EffectInvocationCoverage coverage={0};
        if (status==XR_XIR_OK && owner.producer_enabled)
            status=effect_invocation_coverage_values(&replay,&coverage);
        uint32_t walked=0;
        for (uint32_t e=owner.nodes[n].edge_head;status==XR_XIR_OK && e!=UINT32_MAX;e=owner.edges[e].next) {
            if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;break; }
            if (e>=owner.edge_count || ++walked>owner.edge_count) { status=XR_XIR_BAD_STRUCTURE;break; }
            status=error_invocation_link(flow,contexts,certificate,&replay,e);
        }
        for (uint32_t i=0;status==XR_XIR_OK && i<replay.function->instruction_count;++i) {
            if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;break; }
            XrXirOp op=replay.function->instructions[i].op;
            ErrorInvocationCall *call=&contexts->calls[contexts->nodes[n].calls+i];
            if (call->frontier && (!call->handled || (op!=XR_XIR_CALL_INDIRECT &&
                op!=XR_XIR_INVOKE_INDIRECT && contexts->links[call->head].next!=UINT32_MAX))) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            if (op!=XR_XIR_CALL_INDIRECT && op!=XR_XIR_INVOKE_INDIRECT) continue;
            status=error_invocation_indirect_check(&replay,contexts,&coverage,i,&call->unknown);
            call->handled=true;
        }
        xr_compile_resources_free(coverage.memory);
        xr_compile_resources_free(replay.memory);
    }
    for (uint32_t n=0;status==XR_XIR_OK && n<owner.count;++n)
        for (uint32_t i=0;status==XR_XIR_OK && i<flow->module->functions[contexts->nodes[n].body].instruction_count;++i)
            status=error_invocation_fallback(flow,contexts,certificate,n,i);
    if (status!=XR_XIR_OK) { error_invocation_contexts_free(contexts);return status; }
    flow->summary_count=contexts->summaries;*output=contexts;return XR_XIR_OK;
}

static XrXirStatus error_invocation_summaries(ErrorFlow *flow) {
    if (flow->summary_count<flow->effects->count || !flow->invocations) return XR_XIR_BAD_STRUCTURE;
    if (flow->summary_count==flow->effects->count) return XR_XIR_OK;
    uint64_t words=(uint64_t)flow->summary_count*flow->effects->words;
    uint64_t bytes=(uint64_t)flow->effects->count*flow->effects->words*sizeof(uint64_t);
    if (words>SIZE_MAX/sizeof(uint64_t) || bytes>SIZE_MAX ||
        !xir_compile_work(flow->remaining,bytes)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint64_t *rows=xir_compile_calloc(flow->remaining,(size_t)words,sizeof(*rows),&status);
    if (!rows) return status;
    memcpy(rows,flow->effects->errors,(size_t)bytes);
    xr_compile_resources_free(flow->effects->errors);flow->effects->errors=rows;return XR_XIR_OK;
}

static XrXirStatus error_invocation_call(ErrorFlow *flow,const XrXirInstruction *op,
    uint64_t *set,bool *handled) {
    *handled=false;
    ErrorInvocationContexts *contexts=flow->invocations;
    if (!contexts) return XR_XIR_OK;
    if (contexts->resources!=flow->remaining->resources || flow->invocation>=contexts->count ||
        contexts->nodes[flow->invocation].body!=(uint32_t)(flow->function-flow->module->functions))
        return XR_XIR_BAD_STRUCTURE;
    ptrdiff_t i=op-flow->function->instructions;
    if (i<0 || (uint64_t)i>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const ErrorInvocationCall *call=&contexts->calls[contexts->nodes[flow->invocation].calls+(size_t)i];
    *handled=call->handled;
    if (!*handled) return XR_XIR_OK;
    if (call->unknown) error_add(set,0);
    for (uint32_t e=call->head;e!=UINT32_MAX;e=contexts->links[e].next) {
        if (!xir_compile_work(flow->remaining,2)) return XR_XIR_BUDGET;
        if (e>=contexts->link_count || contexts->links[e].caller!=flow->invocation ||
            contexts->links[e].target>=contexts->count) return XR_XIR_BAD_STRUCTURE;
        const uint64_t *from=flow->effects->errors+(size_t)contexts->nodes[contexts->links[e].target].summary*
            flow->effects->words;
        XrXirStatus status=error_call_summary(flow,op,from,set);
        if (status!=XR_XIR_OK || flow->restart) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus error_invocation_enqueue(ErrorFlow *flow,uint32_t n) {
    ErrorInvocationContexts *contexts=flow->invocations;
    if (n>=contexts->count || contexts->pending>contexts->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
    if (contexts->queued[n]) return XR_XIR_OK;
    if (contexts->pending==contexts->count) return XR_XIR_BAD_STRUCTURE;
    contexts->queue[contexts->back]=n;contexts->queued[n]=1;
    contexts->back=contexts->back+1==contexts->count?0:contexts->back+1;++contexts->pending;
    return XR_XIR_OK;
}

/* Reuse the sole escaping-error CFG/atom fixed point for existing actual
 * nodes. This queue solves error sets, never ROOT, targets or Cell authority. */
static XrXirStatus error_invocation_functions(ErrorFlow *flow) {
    ErrorInvocationContexts *contexts=flow->invocations;
    XrXirStatus status=XR_XIR_OK;
    do {
        if (!xir_compile_work(flow->remaining,contexts->count)) return XR_XIR_BUDGET;
        memset(contexts->queued,0,contexts->count);
        contexts->front=contexts->back=contexts->pending=0;flow->restart=false;
        for (uint32_t n=0;status==XR_XIR_OK && n<contexts->count;++n)
            status=error_invocation_enqueue(flow,n);
        while (status==XR_XIR_OK && contexts->pending && !flow->restart) {
            if (!xir_compile_work(flow->remaining,3)) return XR_XIR_BUDGET;
            uint32_t n=contexts->queue[contexts->front];
            if (n>=contexts->count) return XR_XIR_BAD_STRUCTURE;
            contexts->front=contexts->front+1==contexts->count?0:contexts->front+1;
            --contexts->pending;contexts->queued[n]=0;flow->invocation=n;flow->summary_changed=false;
            bool empty=false;uint32_t f=contexts->nodes[n].body;
            status=error_empty_escaping(&flow->module->functions[f],flow->remaining,&empty);
            if (status==XR_XIR_OK && empty) {
                if (!xir_compile_work(flow->remaining,flow->effects->words)) return XR_XIR_BUDGET;
                empty=!error_any(flow->effects->errors+(size_t)contexts->nodes[n].summary*
                    flow->effects->words,flow->effects->words);
            }
            if (status==XR_XIR_OK && !empty) status=error_function(flow,f);
            if (status!=XR_XIR_OK || flow->restart || !flow->summary_changed) continue;
            for (uint32_t e=contexts->consumers[n];e!=UINT32_MAX;e=contexts->links[e].consumer_next) {
                if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
                if (e>=contexts->link_count) return XR_XIR_BAD_STRUCTURE;
                status=error_invocation_enqueue(flow,contexts->links[e].caller);
                if (status!=XR_XIR_OK) break;
            }
        }
        if (status==XR_XIR_OK && flow->restart) status=error_summary_resize(flow);
    } while (status==XR_XIR_OK && flow->restart);
    return status;
}
