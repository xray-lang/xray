/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_finite.inc.c - Finite support for current Error equations
 *
 * KEY CONCEPT:
 *   Closure excludes missing contributions. Every positive claim must also be
 *   derivable from strictly earlier claims, excluding unsupported cycles.
 */
static XrXirStatus effect_role_range(const XrXirCompileContext *work,
    const void *pointer,uint64_t count,size_t size);
static XrXirStatus effect_role_material_frame(const XrXirCompileContext *work,
    const EffectBodyMaterial *material);
typedef struct EffectErrorFiniteFact { uint64_t coordinate;uint32_t rank; } EffectErrorFiniteFact;
typedef struct EffectErrorFiniteRow { uint64_t first,end;uint32_t blocks,values,node,returned; } EffectErrorFiniteRow;
typedef struct EffectErrorMarkers EffectErrorMarkers;
static void error_markers_free(EffectErrorMarkers *markers);
typedef struct EffectErrorBindings EffectErrorBindings;
static void error_bindings_free(EffectErrorBindings *bindings);
typedef struct EffectErrorFinite {
    XrCompileResources *resources;
    const EffectBodyMaterial *material;
    const EffectInvocationFinal *invocations;
    const EffectErrorAtom *atoms;
    EffectErrorFiniteRow *rows;
    EffectErrorFiniteFact *facts;
    EffectErrorMarkers *markers;
    EffectErrorBindings *bindings;
    EffectTerms terms;
    uint32_t functions,contexts,atom_count,words,count,capacity;
    bool contextual;
} EffectErrorFinite;
static XrXirStatus error_finite_substitution(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,uint32_t function,uint32_t instruction,XrXirType source,XrXirType *output);
static const XrXirTypes *error_finite_types(const EffectErrorFinite *proof) {
    const XrXirTypes *input=proof->material->domain.types,*types=&proof->terms.types;
    if(!types->nodes && !types->count) return input;
    return types->nodes==input->nodes && types->count==input->count &&
        types->nominals==input->nominals && types->interfaces==input->interfaces?input:types;
}
#include "xxir_effect_error_types.inc.c"
/* This identity and its structural block positions belong only to the current
 * callback-free walk. No Error facts or admission result are retained. A full
 * construction buffer expires this view before allocating and reopens it. */
typedef struct EffectErrorFiniteView {
    const XrXirCompileContext *work;
    const EffectErrorFinite *proof;
    const EffectBodyMaterial *material;
    EffectBodyCurrentRead input;
    EffectErrorPredecessors predecessors;
} EffectErrorFiniteView;
typedef struct EffectErrorFiniteRead {
    const EffectErrorFinite *proof;
    const XrXirCompileContext *work;
    uint32_t limit;
    EffectErrorFiniteView *current;
} EffectErrorFiniteRead;
#include "xxir_effect_error_context_namespace.inc.c"

static XrXirStatus error_finite_lower(const EffectErrorFiniteRead *read,uint64_t coordinate,uint32_t *output) {
    const EffectErrorFinite *proof=read->proof;
    uint32_t lo=0,hi=proof->count;
    while(lo<hi) {

        uint32_t mid=lo+(hi-lo)/2;
        if(proof->facts[mid].coordinate<coordinate) lo=mid+1;else hi=mid;
    }
    *output=lo;return XR_XIR_OK;
}
static XrXirStatus error_finite_word(const EffectErrorFiniteRead *read,
    uint64_t coordinate,uint32_t bits,uint64_t *output) {
    const EffectErrorFinite *proof=read->proof;
    if(!output || !bits || bits>64 || coordinate>UINT64_MAX-bits) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;uint64_t result=0;
    XrXirStatus status=error_finite_lower(read,coordinate,&at);if(status!=XR_XIR_OK) return status;

    for(;at<proof->count && proof->facts[at].coordinate<coordinate+bits;++at) {

        if(proof->facts[at].rank<read->limit)
            result|=UINT64_C(1)<<(proof->facts[at].coordinate-coordinate);
    }
    *output=result;return XR_XIR_OK;
}
static uint64_t error_finite_entry_coordinate(const EffectErrorFinite *proof,
    uint32_t function,uint32_t block,uint32_t value,uint32_t word) {
    const EffectErrorFiniteRow *row=&proof->rows[function];
    return row->first+row->blocks+(((uint64_t)block*row->values+value)*proof->words+word)*64;
}
static XrXirStatus error_finite_entry(const EffectErrorEquationRead *equation,
    uint32_t block,uint32_t value,uint32_t word,uint64_t *output) {
    const EffectErrorFiniteRead *read=equation->owner;
    const EffectErrorFiniteRow *row=&read->proof->rows[equation->context];
    if(block>=row->blocks || value>=row->values || word>=read->proof->words) return XR_XIR_BAD_STRUCTURE;
    return error_finite_word(read,error_finite_entry_coordinate(read->proof,equation->context,block,value,word),64,output);
}
#include "xxir_effect_error_context_calls.inc.c"
static XrXirStatus error_finite_equation(const EffectErrorFiniteRead *read,
    uint32_t function,EffectErrorEquationRead *output) {
    uint32_t body=UINT32_MAX;
    XrXirStatus status=error_finite_context_read(read,function,&body);
    if(status!=XR_XIR_OK) return status;

    *output=(EffectErrorEquationRead){.body=effect_body_owned_request(read->work,read->proof->material),
        .work=read->work,.atoms=read->proof->atoms,.context=function,.function=body,
        .atom_count=read->proof->atom_count,.words=read->proof->words,.owner=read,
        .predecessors=read->current?&read->current->predecessors:NULL,
        .entry=error_finite_entry,.call=error_finite_call,.result=error_finite_result};
    if(read->current) output->body.current=&read->current->input;
    return effect_body_request_head(&output->body,output->function,&output->head);
}
static XrXirStatus error_finite_reachable(const EffectErrorFiniteRead *read,
    uint32_t function,uint32_t block,bool *output) {
    uint64_t bit=0;XrXirStatus status=error_finite_word(read,read->proof->rows[function].first+block,1,&bit);
    if(status==XR_XIR_OK) *output=bit!=0;
    return status;
}
typedef struct EffectErrorFiniteEdgeQuery {
    uint32_t to,value,word;
    uint64_t result;
    bool reach;
    const EffectErrorEquationRead *eligible;
} EffectErrorFiniteEdgeQuery;
static XrXirStatus error_finite_edge(const EffectErrorEquationRead *equation,
    const EffectErrorEdgeRead *edge,EffectErrorFiniteEdgeQuery *query) {
    if(edge->to!=query->to) return XR_XIR_OK;
    bool possible=false;uint64_t result=0;XrXirStatus status;
    if(query->reach) {
        EffectErrorFilterRead filter;status=error_equation_edge_possible(equation,edge,&filter,&possible);
        if(possible) result=1;
    } else {

        EffectErrorEdgeValueRead value={query->value,query->word,query->eligible};
        status=error_equation_edge_word_read(equation,edge,&value,&result,&possible);
    }
    if(status==XR_XIR_OK && possible) query->result|=result;
    return status;
}
/* A predecessor census needs only the current opcode and successor ordinals.
 * The live view has framed every canonical column; matching edges still replay
 * their complete transfer. No decoded row or successor inventory survives. */
static XrXirStatus error_finite_successors(const EffectErrorEquationRead *equation,
    uint32_t instruction,uint32_t targets[2],uint32_t *count) {
    const EffectBodyRequest *body=&equation->body;
    const EffectBodyRun *ref=NULL;XrXirInstruction full;uint32_t opcode=XR_XIR_INVALID;
    bool projected=body->current && effect_body_request_sequence_owned(body);
    XrXirStatus status;
    if(projected) {
        status=effect_body_current_record(body,equation->function,instruction,&ref);
        if(status==XR_XIR_OK) status=effect_body_run_word(&body->read,ref,instruction,0,&opcode);
    } else {
        status=error_equation_operation(equation,instruction,&full);
        if(status==XR_XIR_OK) opcode=(uint32_t)full.op;
    }
    if(status!=XR_XIR_OK) return status;
    if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;

    *count=opcode==XR_XIR_BRANCH || opcode==XR_XIR_INVOKE || opcode==XR_XIR_INVOKE_DEFAULT ||
        opcode==XR_XIR_INVOKE_INDIRECT || opcode==XR_XIR_TASK_AWAIT?2:
        opcode==XR_XIR_JUMP || opcode==XR_XIR_CLEANUP_REGISTER || opcode==XR_XIR_CLEANUP_LEAVE ||
        opcode==XR_XIR_CLEANUP_ERROR?1:0;
    for(uint32_t side=0;side<*count;++side) {
        if(projected) status=effect_body_run_word(&body->read,ref,instruction,4+side,&targets[side]);
        else {

            targets[side]=full.targets[side];
        }
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_predecessor_block(const EffectErrorEquationRead *equation,
    EffectErrorFiniteEdgeQuery *query,uint32_t block_index) {
    const XrXirBlock *block=&equation->head.blocks[block_index];
    if(!block->count || block->first>equation->head.instruction_count ||
        block->count>equation->head.instruction_count-block->first) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    if(block->panic==query->to && block->panic) for(uint32_t i=block->first;i-block->first<block->count;++i) {
        EffectErrorEdgeRead edge={block_index,query->to,i,EFFECT_ERROR_EDGE_PANIC};
        status=error_finite_edge(equation,&edge,query);if(status!=XR_XIR_OK) return status;
    }
    uint32_t targets[2]={0},count=0;
    status=error_finite_successors(equation,block->first+block->count-1,targets,&count);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t side=0;side<count;++side) {
        EffectErrorEdgeRead edge={block_index,targets[side],block->first+block->count,
            side?EFFECT_ERROR_EDGE_ALTERNATE:EFFECT_ERROR_EDGE_NORMAL};
        status=error_finite_edge(equation,&edge,query);if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
/* One machine word names every possible source block in a compact function.
 * Only candidate positions are retained; each selected block still replays
 * all parallel/panic edges and reads its current strict-rank reachability. */
static XrXirStatus error_finite_predecessor_index(const EffectErrorEquationRead *equation,
    EffectErrorFiniteView *current) {
    EffectErrorPredecessors *index=&current->predecessors;
    const EffectBodyFunctionHead *head=&equation->head;
    if (current->material!=equation->body.owner || current->input.material!=current->material ||
        current->input.work!=equation->work || head->block_count>64) return XR_XIR_BAD_STRUCTURE;

    if (index->ready && index->function==equation->function && index->blocks==head->block_count)
        return XR_XIR_OK;

    index->ready=false;
    memset(index->rows,0,(size_t)head->block_count*sizeof(*index->rows));

    index->local_writes=0;
    for (uint32_t b=0;b<head->block_count;++b) {
        const XrXirBlock *block=&head->blocks[b];

        if (!block->count || block->first>head->instruction_count ||
            block->count>head->instruction_count-block->first) return XR_XIR_BAD_STRUCTURE;
        /* Structural write positions expire with this callback-free view. No
         * Error, membership, rank or edge verdict is retained with them. */
        for(uint32_t i=block->first;i<block->first+block->count;++i) {

            XrXirOp opcode=XR_XIR_INVALID;
            XrXirStatus status=error_equation_current_opcode(equation,i,&opcode);
            if(status!=XR_XIR_OK) return status;

            if(opcode==XR_XIR_LOCAL_WRITE || opcode==XR_XIR_OWNED_LOCAL_WRITE ||
                opcode==XR_XIR_SCALAR_LOCAL_WRITE) {

                index->local_writes|=UINT64_C(1)<<b;
            }
        }
        uint32_t targets[2]={0},count=0;
        XrXirStatus status=error_finite_successors(equation,block->first+block->count-1,targets,&count);
        if (status!=XR_XIR_OK) return status;
        if (block->panic) {
            if (block->panic>=head->block_count) return XR_XIR_BAD_STRUCTURE;

            index->rows[block->panic]|=UINT64_C(1)<<b;
        }
        for (uint32_t side=0;side<count;++side) {
            if (targets[side]>=head->block_count) return XR_XIR_BAD_STRUCTURE;

            index->rows[targets[side]]|=UINT64_C(1)<<b;
        }
    }

    index->function=equation->function;index->blocks=head->block_count;index->ready=true;
    return XR_XIR_OK;
}
/* Reachability occupies the first coordinates of each framed, sorted fact row.
 * This read-only equation walk can enumerate those actual facts once, applying
 * the same strict rank limit as a Boolean lookup. No cursor survives insertion,
 * growth or any allocator callback, and every selected edge keeps its transfer. */
static XrXirStatus error_finite_predecessors(const EffectErrorEquationRead *equation,
    EffectErrorFiniteEdgeQuery *query) {
    const EffectErrorFiniteRead *read=equation->owner;
    if(!read->current) {
        for(uint32_t b=0;b<equation->head.block_count;++b) {
            bool reachable=false;XrXirStatus status=error_finite_reachable(read,equation->context,b,&reachable);
            if(status==XR_XIR_OK && reachable) status=error_finite_predecessor_block(equation,query,b);
            if(status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    const EffectErrorFinite *proof=read->proof;
    if(read->current->work!=read->work || read->current->proof!=proof ||
        equation->context>=proof->contexts) return XR_XIR_BAD_STRUCTURE;
    const EffectErrorFiniteRow *row=&proof->rows[equation->context];

    if(row->blocks!=equation->head.block_count || row->first>UINT64_MAX-row->blocks)
        return XR_XIR_BAD_STRUCTURE;
    if (row->blocks<=64) {
        if (query->to>=row->blocks) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=error_finite_predecessor_index(equation,read->current);
        if (status!=XR_XIR_OK) return status;

        uint64_t mask=read->current->predecessors.rows[query->to];
        while (mask) {

            uint32_t b=(uint32_t)__builtin_ctzll(mask);mask&=mask-1;
            if (b>=row->blocks) return XR_XIR_BAD_STRUCTURE;
            bool reachable=false;status=error_finite_reachable(read,equation->context,b,&reachable);
            if (status==XR_XIR_OK && reachable) status=error_finite_predecessor_block(equation,query,b);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    uint32_t at=0;XrXirStatus status=error_finite_lower(read,row->first,&at);
    if(status!=XR_XIR_OK) return status;
    for(;at<proof->count;++at) {

        const EffectErrorFiniteFact *fact=&proof->facts[at];
        if(fact->coordinate>=row->first+row->blocks) break;
        if(fact->coordinate<row->first) return XR_XIR_BAD_STRUCTURE;
        if(fact->rank>=read->limit) continue;

        uint32_t block=(uint32_t)(fact->coordinate-row->first);
        status=error_finite_predecessor_block(equation,query,block);
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_summary(const EffectErrorEquationRead *equation,
    uint32_t word,uint64_t *output) {
    const EffectErrorFiniteRead *read=equation->owner;uint64_t result=0;
    bool returned=read->proof->rows[equation->context].returned!=0;
    for(uint32_t b=0;b<equation->head.block_count;++b) {
        bool reachable=false;XrXirStatus status=error_finite_reachable(read,equation->context,b,&reachable);
        if(status!=XR_XIR_OK) return status;
        if(!reachable) continue;
        const XrXirBlock *block=&equation->head.blocks[b];
        for(uint32_t i=block->first;i-block->first<block->count;++i) {
            if(equation->body.current && effect_body_request_sequence_owned(&equation->body)) {
                XrXirOp opcode=XR_XIR_INVALID;status=error_equation_current_opcode(equation,i,&opcode);
                if(status!=XR_XIR_OK) return status;
                bool contributes=returned?opcode==XR_XIR_RETURN:
                    opcode==XR_XIR_THROW || opcode==XR_XIR_CALL || opcode==XR_XIR_CALL_DEFAULT ||
                    opcode==XR_XIR_CALL_INDIRECT || opcode==XR_XIR_CALL_REQUIREMENT;
                if(!contributes) continue;
            }
            XrXirInstruction op;uint64_t contribution=0;status=error_equation_operation(equation,i,&op);
            if(status!=XR_XIR_OK) return status;
            if(returned) {
                if(op.op==XR_XIR_RETURN)
                    status=error_equation_value(equation,b,i,op.args[0],word,&contribution);
            } else if(op.op==XR_XIR_THROW) status=error_equation_value(equation,b,i,op.args[0],word,&contribution);
            else if(op.op==XR_XIR_CALL || op.op==XR_XIR_CALL_DEFAULT || op.op==XR_XIR_CALL_INDIRECT ||
                op.op==XR_XIR_CALL_REQUIREMENT) status=equation->call(equation,i,word,&contribution);
            if(status!=XR_XIR_OK) return status;
            result|=contribution;
        }
    }
    *output=result;return XR_XIR_OK;
}
/* A coordinate is decoded only through dimensions recomputed from current Input.
 * The finite rank is not a permission and never suppresses a closure check. */
static XrXirStatus error_finite_rhs_equation(const EffectErrorFiniteRead *read,
    uint32_t function,uint64_t coordinate,const EffectErrorEquationRead *equation,bool eligible,
    uint64_t *output,uint32_t *bits) {
    const EffectErrorFinite *proof=read->proof;const EffectErrorFiniteRow *row=&proof->rows[function];
    XrXirStatus status=XR_XIR_OK;
    uint64_t local=coordinate-row->first,summary=row->end-row->first-(uint64_t)proof->words*64;
    if(local<row->blocks) {

        EffectErrorFiniteEdgeQuery query={.to=(uint32_t)local,.reach=true,.result=local?0:1};
        status=error_finite_predecessors(equation,&query);*output=query.result;*bits=1;return status;
    }
    *bits=64;
    if(local>=summary) return error_finite_summary(equation,(uint32_t)((local-summary)/64),output);
    uint64_t entry=(local-row->blocks)/64;uint32_t word=(uint32_t)(entry%proof->words);
    entry/=proof->words;uint32_t value=(uint32_t)(entry%row->values),block=(uint32_t)(entry/row->values);
    bool reachable=false;status=error_finite_reachable(read,function,block,&reachable);
    if(status!=XR_XIR_OK) return status;
    if(!reachable) { *output=0;return XR_XIR_OK; }

    EffectErrorFiniteEdgeQuery query={.to=block,.value=value,.word=word,.eligible=eligible?equation:NULL};
    XrXirType type=XR_XIR_UNIT;
    if(!eligible) {
        bool active=false;status=error_equation_membership(equation,value,&active,&type);
        if(status!=XR_XIR_OK) return status;
        if(!active) { *output=0;return XR_XIR_OK; }
    } else if(!block && value<equation->head.parameter_count) {
        if(!equation->head.parameters) return XR_XIR_BAD_STRUCTURE;

        type=equation->head.parameters[value];
    }
    if(!block && value<equation->head.parameter_count) {
        status=error_equation_type_word(equation,type,word,&query.result);if(status!=XR_XIR_OK) return status;
    }
    status=error_finite_predecessors(equation,&query);*output=query.result;return status;
}
static XrXirStatus error_finite_rhs(const EffectErrorFiniteRead *read,
    uint32_t function,uint64_t coordinate,uint64_t *output,uint32_t *bits) {
    const EffectErrorFinite *proof=read->proof;const EffectErrorFiniteRow *row=&proof->rows[function];
    if(coordinate<row->first || coordinate>=row->end) return XR_XIR_BAD_STRUCTURE;
    EffectErrorEquationRead equation;XrXirStatus status=error_finite_equation(read,function,&equation);
    if(status!=XR_XIR_OK) return status;
    return error_finite_rhs_equation(read,function,coordinate,&equation,false,output,bits);
}
/* A current walk already framed this head and selected every carrier from its
 * present opcode and type. The loan ends before growth; strict support uses
 * the independent reader above and never consumes this eligibility. */
static XrXirStatus error_finite_rhs_walk(const EffectErrorFiniteRead *read,
    const EffectErrorEquationRead *equation,uint32_t function,uint64_t coordinate,uint64_t *output,uint32_t *bits) {

    if(!read->current) return error_finite_rhs(read,function,coordinate,output,bits);

    const EffectErrorFinite *proof=read->proof;
    if(equation->owner!=read || equation->work!=read->work || equation->context!=function ||
        equation->body.owner!=proof->material || equation->body.current!=&read->current->input ||
        read->current->work!=read->work || read->current->proof!=proof ||
        read->current->material!=proof->material || function>=proof->contexts) return XR_XIR_BAD_STRUCTURE;
    const EffectErrorFiniteRow *row=&proof->rows[function];
    if(coordinate<row->first || coordinate>=row->end) return XR_XIR_BAD_STRUCTURE;
    return error_finite_rhs_equation(read,function,coordinate,equation,true,output,bits);
}
#include "xxir_effect_error_finite_walk.inc.c"

static XrXirStatus error_finite_atoms(const XrXirCompileContext *work,const EffectErrorFinite *proof) {
    const XrXirTypes *types=error_finite_types(proof);
    uint32_t source_types=proof->material->domain.types->count;
    XrXirStatus status=effect_role_range(work,types->nodes,types->count,sizeof(*types->nodes));
    if(status!=XR_XIR_OK) return status;
    const XrXirNominalTable *nominals=types->nominals;
    if(nominals) {
        status=effect_role_range(work,nominals,1,sizeof(*nominals));if(status!=XR_XIR_OK) return status;
        if(!!nominals->declarations==!!nominals->identities) return XR_XIR_BAD_STRUCTURE;
        if(nominals->declarations) status=effect_role_range(work,nominals->declarations,nominals->count,sizeof(*nominals->declarations));
        else status=effect_role_range(work,nominals->identities,nominals->count,sizeof(*nominals->identities));
        if(status!=XR_XIR_OK) return status;
    }
    uint32_t at=0,parameters=0;
    if(proof->material->generics) {
        status=effect_role_range(work,proof->material->generics,proof->functions,sizeof(*proof->material->generics));
        if(status!=XR_XIR_OK) return status;
        for(uint32_t f=0;f<proof->functions;++f) {

            if(proof->material->generics[f].parameter_count>parameters) parameters=proof->material->generics[f].parameter_count;
        }
    }
    for(uint32_t t=0;t<source_types;++t) {

        const XrXirTypeNode *node=&types->nodes[t];if(node->kind!=XR_XIR_TYPE_NOMINAL) continue;
        if(!nominals || node->nominal.declaration>=nominals->count) return XR_XIR_BAD_STRUCTURE;
        uint32_t d=node->nominal.declaration;
        uint32_t kind=nominals->declarations?nominals->declarations[d].kind:nominals->identities[d].kind;
        if(kind!=XR_XIR_NOMINAL_ENUM) continue;
        uint32_t variants=nominals->declarations?nominals->declarations[d].variant_count:nominals->identities[d].variant_count;
        if(variants>proof->atom_count-at) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t v=0;v<variants;++v) {

            if(proof->atoms[at].type!=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t) || proof->atoms[at].variant!=v)
                return XR_XIR_BAD_STRUCTURE;
            ++at;
        }
    }
    if(parameters>proof->atom_count-at) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<parameters;++p,++at) {

        if(proof->atoms[at].type!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p) ||
            proof->atoms[at].variant!=XR_XIR_ERROR_SYMBOLIC_VARIANT) return XR_XIR_BAD_STRUCTURE;
    }
    uint32_t seeded=at;
    for(;at<proof->atom_count;++at) {

        const EffectErrorAtom *atom=&proof->atoms[at];uint32_t type=(uint32_t)atom->type;
        if(type<XR_XIR_CONSTRUCTED_TYPE_BASE || type>=XR_XIR_CONSTRUCTED_TYPE_LIMIT ||
            type-XR_XIR_CONSTRUCTED_TYPE_BASE<source_types ||
            type-XR_XIR_CONSTRUCTED_TYPE_BASE>=types->count || !xr_xir_type_is_enum(types,atom->type) ||
            atom->variant>=error_variant_count(types,type-XR_XIR_CONSTRUCTED_TYPE_BASE)) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t prior=seeded;prior<at;++prior) {

            if(proof->atoms[prior].type==atom->type && proof->atoms[prior].variant==atom->variant)
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_geometry(const XrXirCompileContext *work,const EffectErrorFinite *proof) {
    XrXirStatus status=effect_role_range(work,proof,1,sizeof(*proof));if(status!=XR_XIR_OK) return status;
    if(proof->resources!=work->resources || proof->count>proof->capacity || !proof->functions ||
        !!proof->facts!=!!proof->capacity || !!proof->atoms!=!!proof->atom_count || !proof->words ||
        proof->words!=((uint64_t)proof->atom_count+65)/64) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_material_frame(work,proof->material);if(status!=XR_XIR_OK) return status;
    status=error_finite_types_frame(work,proof);if(status!=XR_XIR_OK) return status;
    if(proof->functions!=effect_body_count(proof->material)) return XR_XIR_BAD_STRUCTURE;
    uint32_t contexts=0;bool contextual=false;
    status=error_finite_context_count(work,proof,&contexts,&contextual);
    if(status!=XR_XIR_OK) return status;
    if(proof->contexts!=contexts || proof->contextual!=contextual) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,proof->material->functions,proof->functions,sizeof(*proof->material->functions));
    if(status!=XR_XIR_OK) return status;
    status=effect_role_range(work,proof->rows,proof->contexts,sizeof(*proof->rows));
    if(status==XR_XIR_OK) status=effect_role_range(work,proof->facts,proof->capacity,sizeof(*proof->facts));
    if(status==XR_XIR_OK) status=effect_role_range(work,proof->atoms,proof->atom_count,sizeof(*proof->atoms));
    if(status!=XR_XIR_OK) return status;
    status=error_finite_atoms(work,proof);if(status!=XR_XIR_OK) return status;
    uint64_t first=0;
    for(uint32_t f=0;f<proof->contexts;++f) {
        uint32_t node=UINT32_MAX;status=error_finite_context_node(work,proof,f,&node);
        if(status!=XR_XIR_OK) return status;
        const EffectErrorFiniteRow *row=&proof->rows[f];
        uint32_t returned=proof->contextual && f>=proof->invocations->count;
        if(row->node!=node || row->returned!=returned ||
            proof->invocations->nodes[node].body>=proof->functions) return XR_XIR_BAD_STRUCTURE;
        const EffectBodyFunction *head=&proof->material->functions[proof->invocations->nodes[node].body];
        status=effect_role_range(work,head->blocks,head->block_count,sizeof(*head->blocks));
        if(status==XR_XIR_OK) status=effect_role_range(work,head->parameters,head->parameter_count,sizeof(*head->parameters));
        if(status==XR_XIR_OK) status=effect_role_range(work,head->operands,head->operand_count,sizeof(*head->operands));
        if(status==XR_XIR_OK) status=effect_role_range(work,head->runs,head->run_capacity,sizeof(*head->runs));
        if(status!=XR_XIR_OK) return status;
        uint64_t values=(uint64_t)head->parameter_count+head->instruction_count;
        uint64_t summary=(uint64_t)proof->words*64;
        if(values>UINT32_MAX || !values || !head->block_count || first>UINT64_MAX-summary ||
            head->block_count>UINT64_MAX-first-summary ||
            (uint64_t)head->block_count*values>(UINT64_MAX-first-head->block_count-summary)/summary)
            return XR_XIR_BAD_STRUCTURE;
        uint64_t size=head->block_count+(uint64_t)head->block_count*values*proof->words*64+(uint64_t)proof->words*64;
        if(size>UINT64_MAX-first || row->first!=first || row->end!=first+size ||
            row->blocks!=head->block_count || row->values!=values) return XR_XIR_BAD_STRUCTURE;
        first+=size;
    }
    for(uint32_t i=0;i<proof->count;++i) {

        if(!proof->facts[i].rank || proof->facts[i].rank==UINT32_MAX || proof->facts[i].coordinate>=first ||
            (i && proof->facts[i-1].coordinate>=proof->facts[i].coordinate)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
/* Expiration clears every borrowed identity before a callback. Ordinal rows
 * stay unread until their charged per-function initialization is needed. */
static void error_finite_view_reset(EffectErrorFiniteView *current) {
    current->work=NULL;current->proof=NULL;current->material=NULL;
    current->input=(EffectBodyCurrentRead){0};
    current->predecessors.ready=false;
}
static XrXirStatus error_finite_view_open(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,EffectErrorFiniteView *output) {
    if(!output || output->work || output->proof || output->material) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_finite_geometry(work,proof);if(status!=XR_XIR_OK) return status;
    EffectBodyCurrentRead input={0};
    status=effect_body_current_open(work,proof->material,&input);if(status!=XR_XIR_OK) return status;

    output->work=work;output->proof=proof;output->material=proof->material;
    output->input=input;output->predecessors.ready=false;
    return XR_XIR_OK;
}
static XrXirStatus error_finite_check_current(EffectErrorFiniteView *current) {
    const EffectErrorFinite *proof=current->proof;const XrXirCompileContext *work=current->work;
    XrXirStatus status=XR_XIR_OK;EffectErrorFiniteRead read={proof,work,UINT32_MAX,current};
    for(uint32_t f=0;f<proof->contexts;++f) {
        const EffectErrorFiniteRow *row=&proof->rows[f];
        bool empty=false;status=error_finite_walk(&read,f,error_finite_compare,NULL,&empty);
        if(status!=XR_XIR_OK) return status;
        uint32_t start=0;status=error_finite_lower(&read,row->first,&start);if(status!=XR_XIR_OK) return status;
        if(empty) {
            if(start<proof->count && proof->facts[start].coordinate<row->end) return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        for(uint32_t i=start;i<proof->count && proof->facts[i].coordinate<row->end;++i) {
            const EffectErrorFiniteFact *fact=&proof->facts[i];uint64_t coordinate=fact->coordinate;
            uint32_t bit=0,bits=0;uint64_t expected=0;
            if(coordinate>=row->first+row->blocks) {
                bit=(uint32_t)((coordinate-row->first-row->blocks)%64);coordinate-=bit;
            }
            read.limit=fact->rank;status=error_finite_rhs(&read,f,coordinate,&expected,&bits);
            if(status!=XR_XIR_OK) return status;
            if(!(expected&(UINT64_C(1)<<bit))) return XR_XIR_BAD_STRUCTURE;
        }
        read.limit=UINT32_MAX;
    }
    return XR_XIR_OK;
}

static XrXirStatus error_finite_check(const XrXirCompileContext *work,const EffectErrorFinite *proof) {
    EffectErrorFiniteView current;error_finite_view_reset(&current);
    XrXirStatus status=error_finite_view_open(work,proof,&current);
    return status==XR_XIR_OK?error_finite_check_current(&current):status;
}
