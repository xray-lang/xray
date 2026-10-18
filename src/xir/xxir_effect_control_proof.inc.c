/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_control_proof.inc.c - Canonical finite control traversal evidence
 *
 * KEY CONCEPT:
 *   Final owns only function identities and their canonical traversal positions.
 *   Receiving checks actual seeds and incoming edges without a queue, Graph,
 *   result clone or callback. A proposed traversal cannot start an unseeded SCC.
 */
static void effect_control_proof_free(EffectControlProof *proof) {
    xr_compile_resources_free(proof);
}
static const EffectControlPositions *effect_control_positions(const EffectControlProof *proof) {
    return (const EffectControlPositions *)(proof+1);
}
static const uint32_t *effect_control_order(const EffectControlProof *proof,uint32_t mode) {
    return (const uint32_t *)(effect_control_positions(proof)+proof->functions)+(size_t)mode*proof->functions;
}
static uint32_t *effect_control_offsets(EffectControlProof *proof) {
    return (uint32_t *)(effect_control_order(proof,2)+proof->functions);
}
static const uint32_t *effect_control_offsets_read(const EffectControlProof *proof) {
    return effect_control_offsets((EffectControlProof *)proof);
}
static EffectControlEdgeId *effect_control_edges(EffectControlProof *proof) {
    return (EffectControlEdgeId *)(effect_control_offsets(proof)+proof->functions+1);
}
static const EffectControlEdgeId *effect_control_edges_read(const EffectControlProof *proof) {
    return effect_control_edges((EffectControlProof *)proof);
}
static uint64_t effect_control_bytes(uint32_t functions,uint32_t edges) {
    return sizeof(EffectControlProof)+(uint64_t)functions*(sizeof(EffectControlPositions)+
        3*sizeof(uint32_t))+((uint64_t)functions+1)*sizeof(uint32_t)+(uint64_t)edges*sizeof(EffectControlEdgeId);
}
static XrXirStatus effect_control_constructor_frame(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectFinalOutcomes *claims,uint32_t functions,EffectBodyCurrentRead *current) {
    XrXirStatus status=effect_body_current_open(work,material,current);
    if(status!=XR_XIR_OK) return status;
    if(functions!=effect_body_count(material)) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,claims,1,sizeof(*claims));if(status!=XR_XIR_OK) return status;
    if((claims->resources && (claims->count!=functions || claims->resources!=work->resources)) ||
        (!claims->resources && claims->count)) return XR_XIR_BAD_STRUCTURE;
#define EFFECT_CONTROL_CONSTRUCTOR_RANGE(member) if(status==XR_XIR_OK) status=effect_role_range(work,claims->member,functions,sizeof(*claims->member))
    EFFECT_CONTROL_CONSTRUCTOR_RANGE(functions);EFFECT_CONTROL_CONSTRUCTOR_RANGE(witnesses);
    EFFECT_CONTROL_CONSTRUCTOR_RANGE(task_creation);EFFECT_CONTROL_CONSTRUCTOR_RANGE(root);
    EFFECT_CONTROL_CONSTRUCTOR_RANGE(go_safe);EFFECT_CONTROL_CONSTRUCTOR_RANGE(contracts);EFFECT_CONTROL_CONSTRUCTOR_RANGE(entry_roots);
#undef EFFECT_CONTROL_CONSTRUCTOR_RANGE
    if(status!=XR_XIR_OK) return status;
    const XrXirDefaultTable *defaults=material->domain.defaults;
    if(defaults) {
        status=effect_role_range(work,defaults,1,sizeof(*defaults));
        if(status==XR_XIR_OK) status=effect_role_range(work,defaults->records,defaults->count,sizeof(*defaults->records));
        if(status!=XR_XIR_OK) return status;
    }
    for(uint32_t f=0;f<functions;++f) {
        const XrXirFunctionEffectContract *contract=&claims->contracts[f];
        const XirEffectEntryRootView *entry=&claims->entry_roots[f];
        const EffectBodyFunction *head=&material->functions[f];
        if(contract->parameter_count!=head->parameter_count ||
            contract->formula.term_count>(uint64_t)head->parameter_count*2+head->instruction_count ||
            entry->parameter_count>head->parameter_count) return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,contract->parameters,contract->parameter_count,sizeof(*contract->parameters));
        if(status==XR_XIR_OK) status=effect_role_range(work,contract->formula.terms,contract->formula.term_count,sizeof(*contract->formula.terms));
        if(status==XR_XIR_OK) status=effect_role_range(work,entry->parameters,entry->parameter_count,sizeof(*entry->parameters));
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

typedef struct EffectControlRead {
    const XrXirCompileContext *work;
    const EffectFinalOutcomes *claims;
    const EffectControlProof *proof;
    EffectBodyRequest input;
    EffectBodyCurrentRead current;
    uint32_t functions;
} EffectControlRead;
typedef struct EffectControlLocal {
    uint32_t instruction;
    XrXirEffectCause cause;
    bool suspend,task,shape;
} EffectControlLocal;

static XrXirStatus effect_control_local_read(const EffectControlRead *read,uint32_t function,
    EffectControlLocal *output) {
    const EffectBodyMaterial *body=read->input.owner;const EffectBodyFunction *head=&body->functions[function];
    XrXirEffect actual=read->claims->functions[function].suspend;EffectControlLocal local={0};
    if(actual<XR_XIR_EFFECT_NONE || actual>XR_XIR_EFFECT_MAY ||
        (read->claims->task_creation[function]!=XR_XIR_EFFECT_NONE &&
         read->claims->task_creation[function]!=XR_XIR_EFFECT_MAY)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for(uint32_t r=0;r<head->run_count;++r) {
        const EffectBodyRun *run=NULL;XrXirStatus status=effect_body_range_ref(&read->input.read,head,&cursor,&run);
        if(status!=XR_XIR_OK) return status;
        bool constant=!(run->mask&UINT32_C(1));uint32_t points=run->count;
        for(uint32_t i=0;i<points;++i) {
            XrXirInstruction op;XrXirFunctionEffects seed;
            status=effect_control_primitive(&read->input,function,run,run->first+i,&op,&seed);
            if(status!=XR_XIR_OK) return status;
            bool direct=effect_control_direct(op.op);
            XrXirFunctionEffects projection_seed={0};
            if(constant && !direct && effect_seed_opcode(op.op,&projection_seed)==EFFECT_SEED_LOCAL) points=1;
            if(direct) {
                status=effect_body_instruction_ref_value(&read->input.read,run,run->first+i,&op);
                if(status!=XR_XIR_OK) return status;
                bool found=false,cleanup=false;uint32_t target=UINT32_MAX;
                status=effect_control_target(&read->input,read->functions,&op,&found,&target,&cleanup);
                if(status!=XR_XIR_OK || !found) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
                if(!cleanup && (actual<read->claims->functions[target].suspend ||
                    read->claims->task_creation[function]<read->claims->task_creation[target])) return XR_XIR_BAD_STRUCTURE;
            }
            if(!xir_compile_work(read->work,12)) return XR_XIR_BUDGET;
            if(seed.suspend>actual) return XR_XIR_BAD_STRUCTURE;
            if(actual && seed.suspend==actual && !local.suspend) {
                local.suspend=true;local.instruction=run->first+i;
                local.cause=actual==XR_XIR_EFFECT_MAY?XR_XIR_EFFECT_CAUSE_SUSPEND:XR_XIR_EFFECT_CAUSE_INDIRECT;
            }
            local.task|=op.op==XR_XIR_GO;local.shape|=op.op==XR_XIR_SLOT_PLACE;
        }
    }
    if(cursor.first!=head->instruction_count || cursor.run!=head->run_count) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(read->work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=local;return XR_XIR_OK;
}
static XrXirStatus effect_control_read_frame(EffectControlRead *read,
    const EffectInvocationCertificate *certificate) {
    XrXirStatus status=effect_role_range(read->work,certificate,1,sizeof(*certificate));
    if(status==XR_XIR_OK) status=effect_role_range(read->work,read->claims,1,sizeof(*read->claims));
    if(status!=XR_XIR_OK || certificate->resources!=read->work->resources)
        return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
    status=effect_role_range(read->work,certificate->body,1,sizeof(*certificate->body));if(status!=XR_XIR_OK) return status;
    read->functions=effect_body_count(certificate->body);
    status=effect_control_constructor_frame(read->work,certificate->body,read->claims,read->functions,&read->current);
    if(status!=XR_XIR_OK) return status;
    read->input=effect_body_owned_request(read->work,certificate->body);read->input.current=&read->current;
    read->proof=read->claims->control_proof;
    status=effect_role_range(read->work,read->proof,1,sizeof(*read->proof));if(status!=XR_XIR_OK) return status;
    if(read->proof->resources!=read->work->resources || read->proof->material!=certificate->body ||
        read->proof->functions!=read->functions) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes=effect_control_bytes(read->functions,read->proof->edges);
    status=effect_role_range(read->work,read->proof,bytes,1);if(status!=XR_XIR_OK) return status;
    return XR_XIR_OK;
}
static XrXirStatus effect_control_positions_check(const EffectControlRead *read,uint32_t mode) {
    uint32_t count=read->proof->counts[mode];if(count>read->functions) return XR_XIR_BAD_STRUCTURE;
    const EffectControlPositions *positions=effect_control_positions(read->proof);const uint32_t *order=effect_control_order(read->proof,mode);
    for(uint32_t f=0;f<read->functions;++f) {
        if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
        uint32_t position=positions[f].position[mode];bool active=position!=UINT32_MAX;
        if(active && (position>=count || order[position]!=f)) return XR_XIR_BAD_STRUCTURE;
        if((!mode && !!read->claims->functions[f].suspend!=active) ||
            (mode==1 && (read->claims->task_creation[f]==XR_XIR_EFFECT_MAY)!=active)) return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<read->functions;++i) {
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        if(i<count) { if(order[i]>=read->functions || positions[order[i]].position[mode]!=i) return XR_XIR_BAD_STRUCTURE; }
        else if(order[i]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_control_proof_seeds(const EffectControlRead *read,uint32_t next[3]) {
    const EffectControlPositions *positions=effect_control_positions(read->proof);
    for(uint32_t f=0;f<read->functions;++f) {
        EffectControlLocal local;XrXirStatus status=effect_control_local_read(read,f,&local);if(status!=XR_XIR_OK) return status;
        bool seeds[3]={local.suspend,local.task,local.shape};
        for(uint32_t mode=0;mode<3;++mode) if(seeds[mode]) {
            if(positions[f].position[mode]!=next[mode] || next[mode]>=read->proof->counts[mode]) return XR_XIR_BAD_STRUCTURE;
            ++next[mode];
        }
        const XrXirEffectWitness *witness=&read->claims->witnesses[f];
        if(local.suspend && !effect_control_cause_is(witness,local.cause,local.instruction,UINT32_MAX,0)) return XR_XIR_BAD_STRUCTURE;
        if(!read->claims->functions[f].suspend && !effect_control_cause_is(witness,XR_XIR_EFFECT_CAUSE_NONE,0,0,0))
            return XR_XIR_BAD_STRUCTURE;
        bool shape=positions[f].position[2]!=UINT32_MAX,root=read->claims->root[f].requires_root || read->claims->root[f].unresolved;
        if(read->claims->go_safe[f]!=(shape || root?XR_XIR_BAD_TYPE:XR_XIR_OK)) return XR_XIR_BAD_STRUCTURE;
        status=effect_control_entry(read->work,&read->input,read->claims,f);if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_control_proof_edge(const EffectControlRead *read,uint32_t mode,
    uint32_t caller,uint32_t instruction,uint32_t callee,uint32_t *next) {
    const EffectControlPositions *positions=effect_control_positions(read->proof);
    if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
    if(!mode && read->claims->functions[caller].suspend!=read->claims->functions[callee].suspend) return XR_XIR_OK;
    if(positions[caller].position[mode]<*next) return XR_XIR_OK;
    if(*next>=read->proof->counts[mode] || positions[caller].position[mode]!=*next) return XR_XIR_BAD_STRUCTURE;
    if(!mode) {
        const XrXirEffectWitness *from=&read->claims->witnesses[callee];
        if(from->distance==UINT32_MAX || !effect_control_cause_is(&read->claims->witnesses[caller],
            XR_XIR_EFFECT_CAUSE_CALL,instruction,callee,from->distance+1)) return XR_XIR_BAD_STRUCTURE;
    }
    ++*next;return XR_XIR_OK;
}
/* Incoming rows contain only actual caller/instruction identities in the
 * original reverse Input order. Their target and cleanup role are read now. */
static XrXirStatus effect_control_id_read(const EffectControlRead *read,EffectControlEdgeId id,
    uint32_t *target,bool *cleanup) {
    if(id.caller>=read->functions) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus status=effect_body_request_instruction(&read->input,id.caller,id.instruction,&storage,&op);
    bool direct=false;if(status==XR_XIR_OK)
        status=effect_control_target(&read->input,read->functions,op,&direct,target,cleanup);
    return status!=XR_XIR_OK?status:direct?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_control_proof_incoming(const EffectControlRead *read,uint32_t mode,
    uint32_t callee,uint32_t *next) {
    const uint32_t *offsets=effect_control_offsets_read(read->proof);
    const EffectControlEdgeId *edges=effect_control_edges_read(read->proof);
    for(uint32_t i=offsets[callee];i<offsets[callee+1];++i) {
        uint32_t target=UINT32_MAX;bool cleanup=false;
        XrXirStatus status=effect_control_id_read(read,edges[i],&target,&cleanup);
        if(status!=XR_XIR_OK || target!=callee) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
        if(mode!=2 && cleanup) continue;
        status=effect_control_proof_edge(read,mode,edges[i].caller,edges[i].instruction,callee,next);
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Counting real direct opcodes plus distinct valid edge identities proves
 * completeness without a receiving edge bitmap or a copied Graph. */
static XrXirStatus effect_control_edge_census(const EffectControlRead *read,uint32_t *total) {
    uint64_t count=0;const EffectBodyMaterial *body=read->input.owner;
    for(uint32_t f=0;f<read->functions;++f) {
        const EffectBodyFunction *head=&body->functions[f];
        for(uint32_t r=0;r<head->run_count;++r) {
            const EffectBodyRun *run=&head->runs[r];uint32_t points=(run->mask&1)?run->count:1;
            for(uint32_t i=0;i<points;++i) {
                uint32_t opcode=XR_XIR_INVALID;
                XrXirStatus status=effect_body_run_word(&read->input.read,run,run->first+i,0,&opcode);
                if(status!=XR_XIR_OK) return status;
                if(effect_control_direct((XrXirOp)opcode)) count+=(run->mask&1)?1:run->count;
                if(count>UINT32_MAX) return XR_XIR_BUDGET;
            }
        }
    }
    *total=(uint32_t)count;return XR_XIR_OK;
}
static XrXirStatus effect_control_edges_check(const EffectControlRead *read) {
    const uint32_t *offsets=effect_control_offsets_read(read->proof);
    const EffectControlEdgeId *edges=effect_control_edges_read(read->proof);
    if(offsets[0] || offsets[read->functions]!=read->proof->edges) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<read->functions;++f) {
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        if(offsets[f]>offsets[f+1] || offsets[f+1]>read->proof->edges) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t i=offsets[f];i<offsets[f+1];++i) {
            EffectControlEdgeId id=edges[i];uint32_t target=UINT32_MAX;bool cleanup=false;
            if(i>offsets[f] && (edges[i-1].caller<id.caller ||
                (edges[i-1].caller==id.caller && edges[i-1].instruction<=id.instruction))) return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=effect_control_id_read(read,id,&target,&cleanup);
            if(status!=XR_XIR_OK || target!=f) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
        }
    }
    uint32_t count=0;XrXirStatus status=effect_control_edge_census(read,&count);
    return status!=XR_XIR_OK?status:count==read->proof->edges?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_control_proof_check(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectFinalOutcomes *claims) {
    if(!xir_compile_context_valid(work) || !certificate || !claims) return XR_XIR_BAD_STRUCTURE;
    EffectControlRead read={.work=work,.claims=claims};XrXirStatus status=effect_control_read_frame(&read,certificate);
    if(status!=XR_XIR_OK) return status;
    status=effect_control_edges_check(&read);if(status!=XR_XIR_OK) return status;
    for(uint32_t mode=0;mode<3;++mode) { status=effect_control_positions_check(&read,mode);if(status!=XR_XIR_OK) return status; }
    uint32_t next[3]={0};status=effect_control_proof_seeds(&read,next);if(status!=XR_XIR_OK) return status;
    for(uint32_t mode=0;mode<3;++mode) {
        uint32_t count=read.proof->counts[mode];const uint32_t *order=effect_control_order(read.proof,mode);
        for(uint32_t at=0;at<count;++at) {
            if(at>=next[mode]) return XR_XIR_BAD_STRUCTURE;
            status=effect_control_proof_incoming(&read,mode,order[at],&next[mode]);if(status!=XR_XIR_OK) return status;
        }
        if(next[mode]!=count) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* Construction writes proposed identities and traversal positions directly
 * into one owned allocation. No mutable queue or adjacency scratch survives. */
static XrXirStatus effect_control_edges_build(EffectControlRead *read,EffectControlProof *proof,uint32_t pass) {
    uint32_t *offsets=effect_control_offsets(proof);EffectControlEdgeId *edges=effect_control_edges(proof);
    const EffectBodyMaterial *body=read->input.owner;uint32_t written=0;
    for(uint32_t f=read->functions;f>0;) {
        --f;const EffectBodyFunction *head=&body->functions[f];
        for(uint32_t r=head->run_count;r>0;) {
            const EffectBodyRun *run=&head->runs[--r];uint32_t opcode=XR_XIR_INVALID;
            XrXirStatus status=effect_body_run_word(&read->input.read,run,run->first,0,&opcode);
            if(status!=XR_XIR_OK) return status;
            if(!(run->mask&1) && !effect_control_direct((XrXirOp)opcode)) continue;
            for(uint32_t i=run->count;i>0;) {
                --i;if(run->mask&1) {
                    status=effect_body_run_word(&read->input.read,run,run->first+i,0,&opcode);
                    if(status!=XR_XIR_OK) return status;
                    if(!effect_control_direct((XrXirOp)opcode)) continue;
                }
                EffectControlEdgeId id={f,run->first+i};uint32_t target=UINT32_MAX;bool cleanup=false;
                status=effect_control_id_read(read,id,&target,&cleanup);if(status!=XR_XIR_OK) return status;
                if(written>=proof->edges || !xir_compile_work(read->work,3+sizeof(id)))
                    return written>=proof->edges?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET;
                if(!pass) ++offsets[target+1];
                else { if(offsets[target]>=proof->edges) return XR_XIR_BAD_STRUCTURE;edges[offsets[target]++]=id; }
                ++written;
            }
        }
    }
    if(written!=proof->edges) return XR_XIR_BAD_STRUCTURE;
    if(!pass) for(uint32_t f=0;f<read->functions;++f) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        offsets[f+1]+=offsets[f];
    }
    else {
        for(uint32_t f=read->functions;f>0;--f) {
            if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
            offsets[f]=offsets[f-1];
        }
        offsets[0]=0;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_control_order_build(EffectControlRead *read,EffectControlProof *proof) {
    EffectControlPositions *positions=(EffectControlPositions *)(proof+1);
    for(uint32_t f=0;f<read->functions;++f) {
        EffectControlLocal local;XrXirStatus status=effect_control_local_read(read,f,&local);if(status!=XR_XIR_OK) return status;
        bool seeds[3]={local.suspend,local.task,local.shape};
        if(!xir_compile_work(read->work,3*sizeof(uint32_t))) return XR_XIR_BUDGET;
        for(uint32_t mode=0;mode<3;++mode) {
            positions[f].position[mode]=UINT32_MAX;
            if(seeds[mode]) {
                uint32_t at=proof->counts[mode]++;
                ((uint32_t *)effect_control_order(proof,mode))[at]=f;positions[f].position[mode]=at;
            }
        }
    }
    const uint32_t *offsets=effect_control_offsets_read(proof);const EffectControlEdgeId *edges=effect_control_edges_read(proof);
    for(uint32_t mode=0;mode<3;++mode) {
        uint32_t *order=(uint32_t *)effect_control_order(proof,mode);
        for(uint32_t at=0;at<proof->counts[mode];++at) {
            uint32_t callee=order[at];
            for(uint32_t i=offsets[callee];i<offsets[callee+1];++i) {
                EffectControlEdgeId id=edges[i];uint32_t target=UINT32_MAX;bool cleanup=false;
                XrXirStatus status=effect_control_id_read(read,id,&target,&cleanup);if(status!=XR_XIR_OK) return status;
                if((mode!=2 && cleanup) || positions[id.caller].position[mode]!=UINT32_MAX ||
                    (!mode && read->claims->functions[id.caller].suspend!=read->claims->functions[callee].suspend)) continue;
                uint32_t next=proof->counts[mode];if(next>=read->functions) return XR_XIR_BAD_STRUCTURE;
                if(!xir_compile_work(read->work,3*sizeof(uint32_t))) return XR_XIR_BUDGET;
                order[next]=id.caller;positions[id.caller].position[mode]=next;proof->counts[mode]=next+1;
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_control_proof_build(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectFinalOutcomes *claims,EffectControlProof **output) {
    if(!output || *output || !certificate || !claims || !xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,certificate,1,sizeof(*certificate));
    if(status==XR_XIR_OK) status=effect_role_range(work,certificate->body,1,sizeof(*certificate->body));
    if(status!=XR_XIR_OK || certificate->resources!=work->resources) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
    const EffectBodyMaterial *body=certificate->body;uint32_t functions=effect_body_count(body),edges=0;
    EffectControlRead read={.work=work,.claims=claims,.functions=functions};
    status=effect_control_constructor_frame(work,body,claims,functions,&read.current);if(status!=XR_XIR_OK) return status;
    read.input=effect_body_owned_request(work,body);read.input.current=&read.current;
    status=effect_control_edge_census(&read,&edges);if(status!=XR_XIR_OK) return status;
    uint64_t bytes=effect_control_bytes(functions,edges);if(!functions || bytes>SIZE_MAX) return XR_XIR_BUDGET;
    EffectControlProof *proof=xir_compile_calloc(work,1,(size_t)bytes,&status);if(!proof) return status;
    read.current=(EffectBodyCurrentRead){0};
    status=effect_role_range(work,certificate,1,sizeof(*certificate));
    if(status==XR_XIR_OK && (certificate->resources!=work->resources || certificate->body!=body || *output)) status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK) status=effect_control_constructor_frame(work,body,claims,functions,&read.current);
    if(status==XR_XIR_OK && !xir_compile_work(work,sizeof(*proof))) status=XR_XIR_BUDGET;
    if(status==XR_XIR_OK) {
        *proof=(EffectControlProof){.resources=work->resources,.material=body,.functions=functions,.edges=edges};
        read.proof=proof;read.input=effect_body_owned_request(work,body);read.input.current=&read.current;
        status=effect_control_edges_build(&read,proof,0);
    }
    if(status==XR_XIR_OK) status=effect_control_edges_build(&read,proof,1);
    if(status==XR_XIR_OK) status=effect_control_order_build(&read,proof);
    if(status!=XR_XIR_OK) { effect_control_proof_free(proof);return status; }
    *output=proof;return XR_XIR_OK;
}
