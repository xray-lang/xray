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

static XrXirStatus error_invocation_storage(ErrorFlow *flow,
    const EffectInvocationCertificate *certificate,ErrorInvocationContexts **output) {
    const EffectInvocationFinal *owner=certificate->final;
    if (!owner->count || !owner->nodes || !owner->roots || !owner->root_spaces ||
        (owner->edge_count && !owner->edges)) return XR_XIR_BAD_STRUCTURE;
    uint64_t calls=0;
    for (uint32_t n=0;n<owner->count;++n) {
        if (!xir_compile_work(flow->remaining,2)) return XR_XIR_BUDGET;
        if (owner->nodes[n].body>=flow->body.domain.function_count ||
            owner->nodes[n].root>=flow->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionDimensions head={0};
        XrXirStatus status=effect_body_request_dimensions(&flow->body,owner->nodes[n].body,&head);
        if (status!=XR_XIR_OK) return status;
        if (owner->nodes[n].parameter_count!=head.parameter_count) return XR_XIR_BAD_STRUCTURE;
        calls+=head.instruction_count;
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
        EffectBodyFunctionDimensions head={0};
        status=effect_body_request_dimensions(&flow->body,owner->nodes[n].body,&head);
        if (status!=XR_XIR_OK) break;
        uint32_t count=head.instruction_count;
        for (uint32_t i=0;i<count;++i) {
            if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;break; }
            contexts->calls[at++].head=UINT32_MAX;
        }
        if (status!=XR_XIR_OK) break;
    }
    for (uint32_t f=0;status==XR_XIR_OK && f<flow->body.domain.function_count;++f) {
        if (!xir_compile_work(flow->remaining,3)) { status=XR_XIR_BUDGET;break; }
        uint32_t n=owner->roots[f];
        if (n>=owner->count || owner->root_spaces[f]>=flow->body.domain.function_count ||
            owner->nodes[n].body!=f || owner->nodes[n].root!=owner->root_spaces[f] ||
            contexts->nodes[n].summary!=UINT32_MAX) { status=XR_XIR_BAD_STRUCTURE;break; }
        contexts->nodes[n].summary=f;
    }
    contexts->summaries=flow->body.domain.function_count;
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
    uint32_t instruction,const XrXirInstruction *op,bool *unknown) {
    if (!op || instruction>=replay->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (op->immediate<0 || (uint64_t)op->immediate>=replay->values) return XR_XIR_BAD_STRUCTURE;
    const uint64_t *row=effect_invocation_row(replay,(uint32_t)op->immediate);
    if (!row) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=replay->read;
    uint32_t opaque=owner->final->site_count+replay->basis.parameters;
    bool unresolved=false,present=false;
    for (uint32_t bit=owner->final->site_count;bit<=opaque+2;++bit) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        /* The original ROOT advertisement stays intact. Only independently
         * certified exhaustive targets discharge its error-only residual. */
        if (bit==opaque && coverage->ranks && coverage->ranks[(uint32_t)op->immediate]) continue;
        if (row[bit/64]&(UINT64_C(1)<<(bit%64))) unresolved=true;
    }
    for (uint32_t s=0;s<owner->final->site_count;++s) {
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
    XrXirStatus status=effect_invocation_certificate_edge(replay->read->work,certificate,replay->node,e);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationEdge edge=certificate->final->edges[e];
    const EffectInvocationNode *target=&certificate->final->nodes[edge.target];
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_invocation_flow_instruction(replay,edge.instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if (target->parameter_count!=certificate->body->functions[target->body].parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CLEANUP_REGISTER) {
        const XrXirDeclarations *declarations=certificate->body->declarations;
        if (!declarations || !declarations->functions ||
            declarations->functions[target->body].cleanup_owner!=
                certificate->final->nodes[replay->node].body+1)
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
    status=effect_invocation_arguments(replay,op,target->body,edge.producer,closure,&input);
    uint64_t words=(uint64_t)target->parameter_count*replay->basis.row_words;
    if (status==XR_XIR_OK && words>SIZE_MAX/sizeof(*input)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK && words && (!input || !target->input)) status=XR_XIR_BAD_STRUCTURE;
    bool same=true;
    for (size_t w=0;status==XR_XIR_OK && w<(size_t)words;++w) {
        if (!xir_compile_work(replay->read->work,2)) { status=XR_XIR_BUDGET;break; }
        if (input[w]!=target->input[w]) same=false;
    }
    xr_compile_resources_free(input);if (status==XR_XIR_OK) *matched=same;return status;
}

static XrXirStatus error_invocation_link(ErrorFlow *flow,ErrorInvocationContexts *contexts,
    const EffectInvocationCertificate *certificate,EffectInvocationFlow *replay,uint32_t e) {
    const EffectInvocationFinal *owner=certificate->final;
    EffectInvocationEdge edge=owner->edges[e];
    if (edge.caller>=contexts->count || edge.target>=contexts->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t body=contexts->nodes[edge.caller].body;
    EffectBodyFunctionDimensions function={0};
    XrXirStatus read=effect_body_request_dimensions(&flow->body,body,&function);
    if (read!=XR_XIR_OK) return read;
    if (edge.instruction>=function.instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    read=error_operation(flow,body,edge.instruction,&storage,&op);
    if (read!=XR_XIR_OK) return read;
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
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus read=error_operation(flow,contexts->nodes[n].body,instruction,&storage,&op);
    if (read!=XR_XIR_OK) return read;
    uint32_t target=UINT32_MAX;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_GO)
        target=op->immediate<0 || (uint64_t)op->immediate>=flow->effects->count?UINT32_MAX:(uint32_t)op->immediate;
    else if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xir_metadata_default_lookup(flow->remaining,&flow->body.domain,identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding || binding->function>=flow->effects->count) return XR_XIR_BAD_STRUCTURE;
        target=binding->function;
    } else return XR_XIR_OK;
    if (target>=flow->effects->count || contexts->link_count>=contexts->link_capacity)
        return XR_XIR_BAD_STRUCTURE;
    target=certificate->final->roots[target];
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
    if (!xir_effects_context_matches(flow->remaining,flow->effects,flow->body.domain.function_count) ||
        certificate->resources!=flow->remaining->resources || !certificate->final ||
        !effect_body_material_shape(certificate->body) || flow->body.owner!=certificate->body ||
        flow->body.domain.types!=certificate->body->domain.types ||
        flow->body.domain.declarations!=certificate->body->domain.declarations ||
        flow->body.domain.generics!=certificate->body->domain.generics ||
        flow->body.domain.defaults!=certificate->body->domain.defaults)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *sealed=certificate->final;
    if (!sealed->site_count) return XR_XIR_OK;
    bool indirect=false;
    for (uint32_t f=0;f<flow->body.domain.function_count && !indirect;++f) {
        EffectBodyFunctionDimensions head={0};
        XrXirStatus read=effect_body_request_dimensions(&flow->body,f,&head);
        if (read!=XR_XIR_OK) return read;
        for (uint32_t i=0;i<head.instruction_count;++i) {
            if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
            EffectBodyInstructionShape shape={0};read=error_shape(flow,f,i,&shape);
            if (read!=XR_XIR_OK) return read;
            if (shape.op==XR_XIR_CALL_INDIRECT || shape.op==XR_XIR_INVOKE_INDIRECT) { indirect=true;break; }
        }
    }
    if (!indirect) return XR_XIR_OK;
    XrXirStatus status=effect_body_physical_match(flow->remaining,sealed->physical,certificate->body);
    ErrorInvocationContexts *contexts=NULL;
    if (status==XR_XIR_OK) status=error_invocation_storage(flow,certificate,&contexts);
    EffectInvocationRead owner={0};
    if (status==XR_XIR_OK) status=effect_invocation_read_open(flow->remaining,
        certificate,sealed,&owner);
    for (uint32_t n=0;status==XR_XIR_OK && n<owner.final->count;++n) {
        EffectInvocationFlow replay={.read=&owner,.node=n,.instruction=UINT32_MAX};
        status=effect_invocation_origins(&replay);
        EffectInvocationCoverage coverage={0};
        if (status==XR_XIR_OK && owner.producer_enabled)
            status=effect_invocation_coverage_values(&replay,&coverage);
        uint32_t walked=0;
        for (uint32_t e=owner.final->nodes[n].edge_head;status==XR_XIR_OK && e!=UINT32_MAX;e=owner.final->edges[e].next) {
            if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;break; }
            if (e>=owner.final->edge_count || ++walked>owner.final->edge_count) { status=XR_XIR_BAD_STRUCTURE;break; }
            status=error_invocation_link(flow,contexts,certificate,&replay,e);
        }
        for (uint32_t i=0;status==XR_XIR_OK && i<replay.function->instruction_count;++i) {
            if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;break; }
            XrXirInstruction op_storage;const XrXirInstruction *instruction=NULL;
            status=effect_invocation_flow_instruction(&replay,i,&op_storage,&instruction);
            if (status!=XR_XIR_OK) break;
            XrXirOp op=instruction->op;
            ErrorInvocationCall *call=&contexts->calls[contexts->nodes[n].calls+i];
            if (call->frontier && (!call->handled || (op!=XR_XIR_CALL_INDIRECT &&
                op!=XR_XIR_INVOKE_INDIRECT && contexts->links[call->head].next!=UINT32_MAX))) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            if (op!=XR_XIR_CALL_INDIRECT && op!=XR_XIR_INVOKE_INDIRECT) continue;
            status=error_invocation_indirect_check(&replay,contexts,&coverage,i,instruction,&call->unknown);
            call->handled=true;
        }
        xr_compile_resources_free(coverage.memory);
        xr_compile_resources_free(replay.memory);
    }
    for (uint32_t n=0;status==XR_XIR_OK && n<owner.final->count;++n) {
        EffectBodyFunctionDimensions head={0};
        status=effect_body_request_dimensions(&flow->body,contexts->nodes[n].body,&head);
        for (uint32_t i=0;status==XR_XIR_OK && i<head.instruction_count;++i)
            status=error_invocation_fallback(flow,contexts,certificate,n,i);
    }
    effect_invocation_read_close(&owner);
    if (status!=XR_XIR_OK) { error_invocation_contexts_free(contexts);return status; }
    flow->summary_count=contexts->summaries;*output=contexts;return XR_XIR_OK;
}

static XrXirStatus error_invocation_summaries(ErrorFlow *flow) {
    if (flow->summary_count<flow->effects->count || !flow->invocations) return XR_XIR_BAD_STRUCTURE;
    if (flow->summary_count==flow->effects->count) return XR_XIR_OK;
    uint64_t words=(uint64_t)flow->summary_count*flow->effects->outcomes_work.words;
    uint64_t bytes=(uint64_t)flow->effects->count*flow->effects->outcomes_work.words*sizeof(uint64_t);
    if (words>SIZE_MAX/sizeof(uint64_t) || bytes>SIZE_MAX ||
        !xir_compile_work(flow->remaining,bytes)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint64_t *rows=xir_compile_calloc(flow->remaining,(size_t)words,sizeof(*rows),&status);
    if (!rows) return status;
    memcpy(rows,flow->effects->outcomes_work.errors,(size_t)bytes);
    xr_compile_resources_free(flow->effects->outcomes_work.errors);flow->effects->outcomes_work.errors=rows;
    flow->effects->outcomes_work.error_rows=flow->summary_count;return XR_XIR_OK;
}

static XrXirStatus error_invocation_call(ErrorFlow *flow,const XrXirInstruction *op,
    uint64_t *set,bool *handled) {
    *handled=false;
    ErrorInvocationContexts *contexts=flow->invocations;
    if (!contexts) return XR_XIR_OK;
    if (contexts->resources!=flow->remaining->resources || flow->invocation>=contexts->count ||
        contexts->nodes[flow->invocation].body!=flow->function_index)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t i=flow->instruction;
    if (i>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const ErrorInvocationCall *call=&contexts->calls[contexts->nodes[flow->invocation].calls+(size_t)i];
    *handled=call->handled;
    if (!*handled) return XR_XIR_OK;
    if (call->unknown) error_add(set,0);
    for (uint32_t e=call->head;e!=UINT32_MAX;e=contexts->links[e].next) {
        if (!xir_compile_work(flow->remaining,2)) return XR_XIR_BUDGET;
        if (e>=contexts->link_count || contexts->links[e].caller!=flow->invocation ||
            contexts->links[e].target>=contexts->count) return XR_XIR_BAD_STRUCTURE;
        const uint64_t *from=flow->effects->outcomes_work.errors+(size_t)contexts->nodes[contexts->links[e].target].summary*
            flow->effects->outcomes_work.words;
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
            status=error_empty_escaping_read(&flow->body,f,flow->remaining,&empty);
            if (status==XR_XIR_OK && empty) {
                if (!xir_compile_work(flow->remaining,flow->effects->outcomes_work.words)) return XR_XIR_BUDGET;
                empty=!error_any(flow->effects->outcomes_work.errors+(size_t)contexts->nodes[n].summary*
                    flow->effects->outcomes_work.words,flow->effects->outcomes_work.words);
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

static XrXirStatus effect_errors_analyze(XrXirEffects *effects,
    EffectGraph *graph, XrXirCompileContext *remaining) {
    EffectTerms terms = {0}; terms.remaining = remaining;
    if (!effects || !graph || !xir_compile_context_valid(remaining) ||
        !effects->invocations || effects->invocations->resources!=remaining->resources ||
        effects->resources!=remaining->resources || !effects->outcomes_work.functions || !effects->outcomes_work.task_errors ||
        graph->physical_owned || graph->material!=effects->invocations->body ||
        !effect_body_material_shape(graph->material) || graph->material->resources!=remaining->resources ||
        graph->physical!=graph->material->physical || effects->count!=effect_body_count(graph->material))
        return XR_XIR_BAD_STRUCTURE;
    terms.types=*graph->material->domain.types;
    ErrorFlow flow={0};flow.body=effect_body_owned_request(remaining,effects->invocations->body);
    flow.effects=effects;flow.remaining=remaining;flow.terms=&terms;
    flow.summary_count=effects->count;flow.invocation=UINT32_MAX;
    XrXirStatus status=error_invocation_contexts_new(&flow,&flow.invocations);
    if (status==XR_XIR_OK) status=effect_errors_seed(&flow.body.domain,effects,remaining);
    if (status==XR_XIR_OK && flow.invocations) status=error_invocation_summaries(&flow);
    if (status==XR_XIR_OK && flow.invocations) status=error_invocation_functions(&flow);
    else if (status==XR_XIR_OK) {
        status = error_graph_prepare(graph, remaining);
        if (status == XR_XIR_OK) status = error_functions(effects, graph, &flow);
    }
    /* Temporary substituted identities die with terms. Prove their complete
     * outcomes while the sole error summary and authentic binders are alive. */
    XirMetadataDomain permission=flow.body.domain;permission.types=&terms.types;
    for (uint32_t f = 0; status == XR_XIR_OK && f < effects->count; ++f) {
        const uint64_t *set = effects->outcomes_work.errors + (size_t)f * effects->outcomes_work.words;
        XrXirStatus qualified = error_bit(set, 0) || error_bit(set, 1) ? XR_XIR_BAD_TYPE : XR_XIR_OK;
        XirMetadataProofContext proof={&permission,{XR_XIR_CONTEXT_FUNCTION,f,0}};
        for (uint32_t a = 0; qualified == XR_XIR_OK && a < effects->outcomes_work.atom_count; ++a) {
            if (!xir_compile_work(remaining, 1)) { status = XR_XIR_BUDGET; break; }
            if (!error_bit(set, a + 2)) continue;
            qualified = xir_metadata_type_markers_prove(remaining,&proof, effects->outcomes_work.atoms[a].type,
                XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_SENDABLE);
            if (qualified == XR_XIR_BUDGET || qualified == XR_XIR_OUT_OF_MEMORY) { status = qualified; break; }
        }
        effects->outcomes_work.task_errors[f] = qualified;
    }
    error_storage_free(&flow);
    error_invocation_contexts_free(flow.invocations);
    effect_terms_free(&terms);
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < effects->count; ++f) {
        const uint64_t *set = effects->outcomes_work.errors + (size_t)f * effects->outcomes_work.words;
        XrXirEffect fact = error_bit(set, 0) ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE;
        if (error_bit(set,1)) fact=XR_XIR_EFFECT_MAY;
        if (!xir_compile_work(remaining, effects->outcomes_work.atom_count + 1)) return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < effects->outcomes_work.atom_count; ++a) if (error_bit(set, a + 2)) fact = XR_XIR_EFFECT_MAY;
        effects->outcomes_work.functions[f].throws = fact;
    }
    return XR_XIR_OK;
}
