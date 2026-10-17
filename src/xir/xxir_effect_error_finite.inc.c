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
typedef struct EffectErrorFiniteRow { uint64_t first,end;uint32_t blocks,values,node; } EffectErrorFiniteRow;
typedef struct EffectErrorMarkers EffectErrorMarkers;
static void error_markers_free(EffectErrorMarkers *markers);
typedef struct EffectErrorFinite {
    XrCompileResources *resources;
    const EffectBodyMaterial *material;
    const EffectInvocationFinal *invocations;
    const EffectErrorAtom *atoms;
    EffectErrorFiniteRow *rows;
    EffectErrorFiniteFact *facts;
    EffectErrorMarkers *markers;
    uint32_t functions,contexts,atom_count,words,count,capacity;
    bool contextual;
} EffectErrorFinite;
/* This identity belongs only to the current callback-free receiving walk.
 * It retains no rows, computed facts or admission result. A full construction
 * buffer unwinds its walk before allocating and opens a fresh view afterward. */
typedef struct EffectErrorFiniteView {
    const XrXirCompileContext *work;
    const EffectErrorFinite *proof;
    const EffectBodyMaterial *material;
    EffectBodyCurrentRead input;
} EffectErrorFiniteView;
typedef struct EffectErrorFiniteRead {
    const EffectErrorFinite *proof;
    const XrXirCompileContext *work;
    uint32_t limit;
    const EffectErrorFiniteView *current;
} EffectErrorFiniteRead;
#include "xxir_effect_error_context_namespace.inc.c"

static XrXirStatus error_finite_lower(const EffectErrorFiniteRead *read,uint64_t coordinate,uint32_t *output) {
    const EffectErrorFinite *proof=read->proof;
    uint32_t lo=0,hi=proof->count;
    while(lo<hi) {
        if(!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
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
    if(!xir_compile_work(read->work,1+sizeof(*output))) return XR_XIR_BUDGET;
    for(;at<proof->count && proof->facts[at].coordinate<coordinate+bits;++at) {
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
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
        .entry=error_finite_entry,.call=error_finite_call};
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
} EffectErrorFiniteEdgeQuery;
static XrXirStatus error_finite_edge(const EffectErrorEquationRead *equation,
    const EffectErrorEdgeRead *edge,EffectErrorFiniteEdgeQuery *query) {
    if(edge->to!=query->to) return XR_XIR_OK;
    bool possible=false;uint64_t result=0;XrXirStatus status;
    if(query->reach) {
        EffectErrorFilterRead filter;status=error_equation_edge_possible(equation,edge,&filter,&possible);
        if(possible) result=1;
    } else status=error_equation_edge_word(equation,edge,query->value,query->word,&result,&possible);
    if(status==XR_XIR_OK && possible) query->result|=result;
    return status;
}
static XrXirStatus error_finite_predecessors(const EffectErrorEquationRead *equation,
    EffectErrorFiniteEdgeQuery *query) {
    const EffectErrorFiniteRead *read=equation->owner;
    for(uint32_t b=0;b<equation->head.block_count;++b) {
        bool reachable=false;XrXirStatus status=error_finite_reachable(read,equation->context,b,&reachable);
        if(status!=XR_XIR_OK) return status;
        if(!reachable) continue;
        const XrXirBlock *block=&equation->head.blocks[b];
        if(!block->count || block->first>equation->head.instruction_count ||
            block->count>equation->head.instruction_count-block->first) return XR_XIR_BAD_STRUCTURE;
        if(block->panic==query->to && block->panic) for(uint32_t i=block->first;i-block->first<block->count;++i) {
            EffectErrorEdgeRead edge={b,query->to,i,EFFECT_ERROR_EDGE_PANIC};
            status=error_finite_edge(equation,&edge,query);if(status!=XR_XIR_OK) return status;
        }
        XrXirInstruction end;status=error_equation_operation(equation,block->first+block->count-1,&end);
        if(status!=XR_XIR_OK) return status;
        uint32_t count=end.op==XR_XIR_BRANCH || end.op==XR_XIR_INVOKE || end.op==XR_XIR_INVOKE_DEFAULT ||
            end.op==XR_XIR_INVOKE_INDIRECT || end.op==XR_XIR_TASK_AWAIT?2:
            end.op==XR_XIR_JUMP || end.op==XR_XIR_CLEANUP_REGISTER || end.op==XR_XIR_CLEANUP_LEAVE ||
            end.op==XR_XIR_CLEANUP_ERROR?1:0;
        for(uint32_t side=0;side<count;++side) {
            EffectErrorEdgeRead edge={b,end.targets[side],block->first+block->count,
                side?EFFECT_ERROR_EDGE_ALTERNATE:EFFECT_ERROR_EDGE_NORMAL};
            status=error_finite_edge(equation,&edge,query);if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_summary(const EffectErrorEquationRead *equation,
    uint32_t word,uint64_t *output) {
    const EffectErrorFiniteRead *read=equation->owner;uint64_t result=0;
    for(uint32_t b=0;b<equation->head.block_count;++b) {
        bool reachable=false;XrXirStatus status=error_finite_reachable(read,equation->context,b,&reachable);
        if(status!=XR_XIR_OK) return status;
        if(!reachable) continue;
        const XrXirBlock *block=&equation->head.blocks[b];
        for(uint32_t i=block->first;i-block->first<block->count;++i) {
            XrXirInstruction op;uint64_t contribution=0;status=error_equation_operation(equation,i,&op);
            if(status!=XR_XIR_OK) return status;
            if(op.op==XR_XIR_THROW) status=error_equation_value(equation,b,i,op.args[0],word,&contribution);
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
static XrXirStatus error_finite_rhs(const EffectErrorFiniteRead *read,
    uint32_t function,uint64_t coordinate,uint64_t *output,uint32_t *bits) {
    const EffectErrorFinite *proof=read->proof;const EffectErrorFiniteRow *row=&proof->rows[function];
    if(coordinate<row->first || coordinate>=row->end) return XR_XIR_BAD_STRUCTURE;
    EffectErrorEquationRead equation;XrXirStatus status=error_finite_equation(read,function,&equation);
    if(status!=XR_XIR_OK) return status;
    uint64_t local=coordinate-row->first,summary=row->end-row->first-(uint64_t)proof->words*64;
    if(local<row->blocks) {
        EffectErrorFiniteEdgeQuery query={.to=(uint32_t)local,.reach=true,.result=local?0:1};
        status=error_finite_predecessors(&equation,&query);*output=query.result;*bits=1;return status;
    }
    *bits=64;
    if(local>=summary) return error_finite_summary(&equation,(uint32_t)((local-summary)/64),output);
    uint64_t entry=(local-row->blocks)/64;uint32_t word=(uint32_t)(entry%proof->words);
    entry/=proof->words;uint32_t value=(uint32_t)(entry%row->values),block=(uint32_t)(entry/row->values);
    bool reachable=false;status=error_finite_reachable(read,function,block,&reachable);
    if(status!=XR_XIR_OK) return status;
    if(!reachable) { *output=0;return XR_XIR_OK; }
    EffectErrorFiniteEdgeQuery query={.to=block,.value=value,.word=word};
    bool active=false;XrXirType type=XR_XIR_UNIT;
    status=error_equation_membership(&equation,value,&active,&type);
    if(status!=XR_XIR_OK) return status;
    if(!active) { *output=0;return XR_XIR_OK; }
    if(!block && value<equation.head.parameter_count) {
        status=error_equation_type_word(&equation,type,word,&query.result);if(status!=XR_XIR_OK) return status;
    }
    status=error_finite_predecessors(&equation,&query);*output=query.result;return status;
}
#include "xxir_effect_error_finite_walk.inc.c"

static XrXirStatus error_finite_atoms(const XrXirCompileContext *work,const EffectErrorFinite *proof) {
    const XrXirTypes *types=proof->material->domain.types;
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
            if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if(proof->material->generics[f].parameter_count>parameters) parameters=proof->material->generics[f].parameter_count;
        }
    }
    for(uint32_t t=0;t<types->count;++t) {
        if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node=&types->nodes[t];if(node->kind!=XR_XIR_TYPE_NOMINAL) continue;
        if(!nominals || node->nominal.declaration>=nominals->count) return XR_XIR_BAD_STRUCTURE;
        uint32_t d=node->nominal.declaration;
        uint32_t kind=nominals->declarations?nominals->declarations[d].kind:nominals->identities[d].kind;
        if(kind!=XR_XIR_NOMINAL_ENUM) continue;
        uint32_t variants=nominals->declarations?nominals->declarations[d].variant_count:nominals->identities[d].variant_count;
        if(variants>proof->atom_count-at) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t v=0;v<variants;++v) {
            if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
            if(proof->atoms[at].type!=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t) || proof->atoms[at].variant!=v)
                return XR_XIR_BAD_STRUCTURE;
            ++at;
        }
    }
    if(parameters!=proof->atom_count-at) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<parameters;++p,++at) {
        if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if(proof->atoms[at].type!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p) ||
            proof->atoms[at].variant!=XR_XIR_ERROR_SYMBOLIC_VARIANT) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_geometry(const XrXirCompileContext *work,const EffectErrorFinite *proof) {
    XrXirStatus status=effect_role_range(work,proof,1,sizeof(*proof));if(status!=XR_XIR_OK) return status;
    if(proof->resources!=work->resources || proof->count>proof->capacity || !proof->functions ||
        !!proof->facts!=!!proof->capacity || !!proof->atoms!=!!proof->atom_count || !proof->words ||
        proof->words!=((uint64_t)proof->atom_count+65)/64) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_material_frame(work,proof->material);if(status!=XR_XIR_OK) return status;
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
        if(row->node!=node || proof->invocations->nodes[node].body>=proof->functions) return XR_XIR_BAD_STRUCTURE;
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
        if(!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if(!proof->facts[i].rank || proof->facts[i].rank==UINT32_MAX || proof->facts[i].coordinate>=first ||
            (i && proof->facts[i-1].coordinate>=proof->facts[i].coordinate)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_view_open(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,EffectErrorFiniteView *output) {
    if(!output || output->work || output->proof || output->material) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_finite_geometry(work,proof);if(status!=XR_XIR_OK) return status;
    EffectBodyCurrentRead input={0};
    status=effect_body_current_open(work,proof->material,&input);if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectErrorFiniteView){work,proof,proof->material,input};return XR_XIR_OK;
}
static XrXirStatus error_finite_check_current(const EffectErrorFiniteView *current) {
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
    EffectErrorFiniteView current={0};XrXirStatus status=error_finite_view_open(work,proof,&current);
    return status==XR_XIR_OK?error_finite_check_current(&current):status;
}
