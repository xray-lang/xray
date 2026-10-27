/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_equation_proof.inc.c - Grounded current CFG Error facts
 *
 * KEY CONCEPT:
 *   Exact equations establish closure. Every positive fact also needs a
 *   strictly earlier derivation, excluding invented facts in an unseeded SCC.
 *   Call operands still require their independently checked invocation domain.
 */
typedef struct EffectErrorEquationFact {
    uint32_t block,value,bit,rank;
} EffectErrorEquationFact;
typedef struct EffectErrorEquationProof {
    EffectErrorEquationRead base;
    const EffectErrorEquationFact *facts;
    uint32_t count,rank_bound;
} EffectErrorEquationProof;

static inline int error_equation_fact_order(const EffectErrorEquationFact *a,
    const EffectErrorEquationFact *b) {
    if(a->block!=b->block) return a->block<b->block?-1:1;
    if(a->value!=b->value) return a->value<b->value?-1:1;
    return a->bit==b->bit?0:a->bit<b->bit?-1:1;
}
static inline XrXirStatus error_equation_fact_word(const EffectErrorEquationRead *read,
    uint32_t block,uint32_t value,uint32_t word,uint64_t *output) {
    const EffectErrorEquationProof *proof=read->owner;
    uint64_t result=0;uint32_t first=0,last=proof->count;
    EffectErrorEquationFact key={block,value,word*64,0};
    while(first<last) {

        uint32_t middle=first+(last-first)/2;
        if(error_equation_fact_order(&proof->facts[middle],&key)<0) first=middle+1;
        else last=middle;
    }
    for(uint32_t n=first;n<proof->count;++n) {

        const EffectErrorEquationFact *fact=&proof->facts[n];
        if(fact->block!=block || fact->value!=value || fact->bit/64!=word) break;
        if(fact->rank<read->rank_limit) result|=UINT64_C(1)<<(fact->bit%64);
    }
    *output=result;return XR_XIR_OK;
}
static inline XrXirStatus error_equation_proof_call(const EffectErrorEquationRead *read,
    uint32_t instruction,uint32_t word,uint64_t *output) {
    const EffectErrorEquationProof *proof=read->owner;

    EffectErrorEquationRead call=proof->base;call.rank_limit=read->rank_limit;
    return call.call(&call,instruction,word,output);
}
static inline EffectErrorEquationRead error_equation_proof_read(
    const EffectErrorEquationProof *proof,uint32_t limit) {
    EffectErrorEquationRead read=proof->base;
    read.owner=proof;read.rank_limit=limit;
    read.entry=error_equation_fact_word;read.call=error_equation_proof_call;
    return read;
}
static inline XrXirStatus error_equation_reachable(const EffectErrorEquationRead *read,
    uint32_t block,bool *output) {
    uint64_t row=0;
    XrXirStatus status=error_equation_fact_word(read,block,UINT32_MAX,0,&row);
    *output=row!=0;return status;
}

static inline uint32_t error_equation_terminal_edges(XrXirOp op) {
    if(op==XR_XIR_BRANCH || op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_DEFAULT ||
        op==XR_XIR_INVOKE_INDIRECT || op==XR_XIR_TASK_AWAIT) return 2;
    return op==XR_XIR_JUMP || op==XR_XIR_CLEANUP_REGISTER ||
        op==XR_XIR_CLEANUP_LEAVE || op==XR_XIR_CLEANUP_ERROR?1:0;
}

static inline XrXirStatus error_equation_incoming_edge(const EffectErrorEquationRead *read,
    const EffectErrorEdgeRead *edge,uint32_t value,uint32_t word,uint64_t *row) {
    bool possible=false;uint64_t incoming=0;
    XrXirStatus status;
    if(value==UINT32_MAX) {
        EffectErrorFilterRead filter;
        status=error_equation_edge_possible(read,edge,&filter,&possible);
        incoming=possible?1:0;
    } else status=error_equation_edge_word(read,edge,value,word,&incoming,&possible);
    if(status==XR_XIR_OK) *row|=incoming;
    return status;
}

/* Both closure and positive derivations enumerate real incoming Input edges.
 * No predecessor list, retained transfer, saved reachability or queue is read. */
static inline XrXirStatus error_equation_incoming_word(const EffectErrorEquationRead *read,
    uint32_t target,uint32_t value,uint32_t word,uint64_t *output) {
    uint64_t result=0;
    for(uint32_t b=0;b<read->head.block_count;++b) {

        bool reachable=false;XrXirStatus status=error_equation_reachable(read,b,&reachable);
        if(status!=XR_XIR_OK) return status;
        if(!reachable) continue;
        const XrXirBlock *block=&read->head.blocks[b];
        if(!block->count || block->first>UINT32_MAX-block->count) return XR_XIR_BAD_STRUCTURE;
        uint32_t end=block->first+block->count;
        if(block->panic && block->panic==target) for(uint32_t i=block->first;i<end;++i) {
            EffectErrorEdgeRead edge={b,target,i,EFFECT_ERROR_EDGE_PANIC};
            status=error_equation_incoming_edge(read,&edge,value,word,&result);
            if(status!=XR_XIR_OK) return status;
        }
        XrXirInstruction terminal;
        status=error_equation_operation(read,end-1,&terminal);
        if(status!=XR_XIR_OK) return status;
        uint32_t sides=error_equation_terminal_edges(terminal.op);
        for(uint32_t side=0;side<sides;++side) if(terminal.targets[side]==target) {
            EffectErrorEdgeRead edge={b,target,end,
                side?EFFECT_ERROR_EDGE_ALTERNATE:EFFECT_ERROR_EDGE_NORMAL};
            status=error_equation_incoming_edge(read,&edge,value,word,&result);
            if(status!=XR_XIR_OK) return status;
        }
    }
    *output=result;return XR_XIR_OK;
}

static inline XrXirStatus error_equation_summary_word(const EffectErrorEquationRead *read,
    uint32_t word,uint64_t *output) {
    uint64_t result=0;
    for(uint32_t b=0;b<read->head.block_count;++b) {
        bool reachable=false;XrXirStatus status=error_equation_reachable(read,b,&reachable);
        if(status!=XR_XIR_OK) return status;
        if(!reachable) continue;
        const XrXirBlock *block=&read->head.blocks[b];
        for(uint32_t i=block->first;i-block->first<block->count;++i) {

            XrXirInstruction op;uint64_t row=0;
            status=error_equation_operation(read,i,&op);
            if(status!=XR_XIR_OK) return status;
            if(op.op==XR_XIR_THROW)
                status=error_equation_value(read,b,i,op.args[0],word,&row);
            else if(op.op==XR_XIR_CALL || op.op==XR_XIR_CALL_DEFAULT ||
                op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_CALL_REQUIREMENT)
                status=read->call(read,i,word,&row);
            if(status!=XR_XIR_OK) return status;
            result|=row;
        }
    }
    *output=result;return XR_XIR_OK;
}

static inline XrXirStatus error_equation_fact_rhs(const EffectErrorEquationRead *read,
    uint32_t block,uint32_t value,uint32_t word,uint64_t *output) {
    if(block==UINT32_MAX) return error_equation_summary_word(read,word,output);
    XrXirStatus status=error_equation_incoming_word(read,block,value,word,output);
    if(status!=XR_XIR_OK || block) return status;
    if(value==UINT32_MAX) *output|=1;
    else if(value<read->head.parameter_count) {
        uint64_t seed=0;
        status=error_equation_type_word(read,read->head.parameters[value],word,&seed);
        *output|=seed;
    }
    return status;
}

static inline XrXirStatus error_equation_proof_shape(const EffectErrorEquationProof *proof) {
    if(!proof) return XR_XIR_BAD_STRUCTURE;
    const EffectErrorEquationRead *read=&proof->base;
    if(!read->owner || !read->call || (proof->count && !proof->facts) ||
        !xir_compile_context_valid(read->work) || !read->head.block_count || !read->head.blocks ||
        (read->head.parameter_count && !read->head.parameters) ||
        read->words!=((uint64_t)read->atom_count+65)/64 ||
        read->head.parameter_count>UINT32_MAX-read->head.instruction_count ||
        proof->count==UINT32_MAX || proof->rank_bound==UINT32_MAX ||
        (proof->rank_bound && proof->rank_bound<proof->count)) return XR_XIR_BAD_STRUCTURE;
    if(proof->count) {
        uint64_t bytes=(uint64_t)proof->count*sizeof(*proof->facts);
        if(bytes>SIZE_MAX || (uintptr_t)proof->facts%_Alignof(EffectErrorEquationFact))
            return XR_XIR_BAD_STRUCTURE;
        XrCompileResourceStatus span=xr_compile_resources_extent(read->work->resources,
            proof->facts,(size_t)bytes);
        if(span!=XR_COMPILE_RESOURCE_OK)
            return XR_XIR_BAD_STRUCTURE;
    }
    uint32_t values=read->head.parameter_count+read->head.instruction_count;
    for(uint32_t n=0;n<proof->count;++n) {
        const EffectErrorEquationFact *fact=&proof->facts[n];

        if(n && error_equation_fact_order(fact-1,fact)>=0) return XR_XIR_BAD_STRUCTURE;
        if(fact->block==UINT32_MAX) {
            if(fact->value!=UINT32_MAX || fact->bit>=(uint64_t)read->atom_count+2)
                return XR_XIR_BAD_STRUCTURE;
        } else {
            if(fact->block>=read->head.block_count) return XR_XIR_BAD_STRUCTURE;
            if(fact->value==UINT32_MAX) { if(fact->bit) return XR_XIR_BAD_STRUCTURE; }
            else {
                if(fact->value>=values || fact->bit>=(uint64_t)read->atom_count+2)
                    return XR_XIR_BAD_STRUCTURE;
                bool active=false;XrXirType type=XR_XIR_UNIT;
                XrXirStatus status=error_equation_membership(read,fact->value,&active,&type);
                if(status!=XR_XIR_OK) return status;
                if(!active) return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}

static inline XrXirStatus error_equation_proof_closure(const EffectErrorEquationProof *proof) {
    XrXirStatus status=error_equation_proof_shape(proof);
    if(status!=XR_XIR_OK) return status;

    EffectErrorEquationRead read=error_equation_proof_read(proof,UINT32_MAX);
    uint32_t values=read.head.parameter_count+read.head.instruction_count;
    for(uint32_t b=0;b<read.head.block_count;++b) {
        uint64_t proposed=0,actual=0;
        status=error_equation_fact_word(&read,b,UINT32_MAX,0,&proposed);
        if(status==XR_XIR_OK) status=error_equation_fact_rhs(&read,b,UINT32_MAX,0,&actual);
        if(status!=XR_XIR_OK) return status;
        if(proposed!=actual) return XR_XIR_BAD_STRUCTURE;
    }
    /* Membership depends on the current Input value, not on a CFG block. Read
     * every value once, then check every block equation for each active value. */
    for(uint32_t v=0;v<values;++v) {
        bool active=false;XrXirType type=XR_XIR_UNIT;
        status=error_equation_membership(&read,v,&active,&type);
        if(status!=XR_XIR_OK) return status;
        if(!active) continue;
        for(uint32_t b=0;b<read.head.block_count;++b)
            for(uint32_t w=0;w<read.words;++w) {
                uint64_t proposed=0,actual=0;
                status=error_equation_fact_word(&read,b,v,w,&proposed);
                if(status==XR_XIR_OK) status=error_equation_fact_rhs(&read,b,v,w,&actual);
                if(status!=XR_XIR_OK) return status;
                if(proposed!=actual) return XR_XIR_BAD_STRUCTURE;
            }
    }
    for(uint32_t w=0;w<read.words;++w) {
        uint64_t proposed=0,actual=0;
        status=error_equation_fact_word(&read,UINT32_MAX,UINT32_MAX,w,&proposed);
        if(status==XR_XIR_OK) status=error_equation_summary_word(&read,w,&actual);
        if(status!=XR_XIR_OK) return status;
        if(proposed!=actual) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* No allocation, release or mutable solver buffer is needed here. Every
 * operand used for positive support must descend in the same rank domain. */
static inline XrXirStatus error_equation_proof_current(const EffectErrorEquationProof *proof) {
    XrXirStatus status=error_equation_proof_closure(proof);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t n=0;n<proof->count;++n) {
        const EffectErrorEquationFact *fact=&proof->facts[n];
        uint32_t bound=proof->rank_bound?proof->rank_bound:proof->count;
        if(!fact->rank || fact->rank>bound) return XR_XIR_BAD_STRUCTURE;

        EffectErrorEquationRead read=error_equation_proof_read(proof,fact->rank);
        uint64_t actual=0;
        status=error_equation_fact_rhs(&read,fact->block,fact->value,fact->bit/64,&actual);
        if(status!=XR_XIR_OK) return status;
        if(!(actual&(UINT64_C(1)<<(fact->bit%64)))) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* Construction only proposes ranks for already proposed semantic facts. The
 * independent current verifier must still check complete closure and support. */
static inline XrXirStatus error_equation_proof_ground(EffectErrorEquationProof *proof,
    EffectErrorEquationFact *facts) {
    if(!proof || proof->facts!=facts) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_equation_proof_shape(proof);
    if(status!=XR_XIR_OK) return status;

    for(uint32_t n=0;n<proof->count;++n) facts[n].rank=UINT32_MAX;
    uint32_t derived=0;
    while(derived<proof->count) {
        bool changed=false;
        /* Each new derivation immediately becomes an operand. The two stable
         * directions avoid repeated whole-chain scans for reverse block IDs;
         * rank still follows actual support, never storage or schedule order. */
        for(uint32_t direction=0;direction<2;++direction) for(uint32_t at=0;at<proof->count;++at) {

            uint32_t n=direction?proof->count-at-1:at;
            if(facts[n].rank==UINT32_MAX) {

                EffectErrorEquationRead read=error_equation_proof_read(proof,derived+1);
                uint64_t actual=0;
                status=error_equation_fact_rhs(&read,facts[n].block,facts[n].value,facts[n].bit/64,&actual);
                if(status!=XR_XIR_OK) return status;
                if(actual&(UINT64_C(1)<<(facts[n].bit%64))) {
                    facts[n].rank=++derived;changed=true;
                }
            }
        }
        if(!changed) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
