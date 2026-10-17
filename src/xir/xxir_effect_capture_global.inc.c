/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_global.inc.c - Finite origin and capture support across actual keys
 *
 * KEY CONCEPT:
 *   Physical values, return vertices and initial-key cells share one sparse
 *   descending relation. An initial key is separate from its computed SSA row.
 *   Closure and positive support consume the sole Input and Final without a solver.
 */
#define EFFECT_CAPTURE_FLAGS UINT32_MAX
enum { EFFECT_CAPTURE_FIXED, EFFECT_CAPTURE_RAW, EFFECT_CAPTURE_COMPLETE };
enum { EFFECT_CAPTURE_SCALAR, EFFECT_CAPTURE_CALLABLE, EFFECT_CAPTURE_CELL, EFFECT_CAPTURE_CLASS };

typedef struct EffectCaptureNode {
    EffectBodyFunctionHead head;
    EffectInvocationBasis basis;
    uint32_t node,values;
} EffectCaptureNode;
typedef struct EffectCaptureRead {
    const XrXirCompileContext *work;
    const EffectInvocationFinal *final;
    EffectBodyRequest body;
    EffectCaptureProof *builder;
    const EffectCaptureFact *selected;
    const EffectCaptureNode *node_view;
    uint32_t maximum;
    bool fixed_only,fallback,class_results,active,changed,supported;
} EffectCaptureRead;

static bool effect_capture_before(const EffectCaptureFact *a,const EffectCaptureFact *b) {
    return a->node<b->node || (a->node==b->node && (a->value<b->value ||
        (a->value==b->value && (a->word<b->word || (a->word==b->word && a->bit<b->bit)))));
}
static XrXirStatus effect_capture_lower(const EffectCaptureRead *read,const EffectCaptureFact *key,uint32_t *output) {
    const EffectCaptureProof *proof=&read->final->capture;uint32_t lo=0,hi=proof->count;
    while(lo<hi) {
        if(!xir_compile_work(read->work,6)) return XR_XIR_BUDGET;
        uint32_t mid=lo+(hi-lo)/2;
        if(effect_capture_before(&proof->facts[mid],key)) lo=mid+1;else hi=mid;
    }
    *output=lo;return XR_XIR_OK;
}
static XrXirStatus effect_capture_find(const EffectCaptureRead *read,EffectCaptureFact key,
    const EffectCaptureFact **output) {
    uint32_t at=0;*output=NULL;XrXirStatus status=effect_capture_lower(read,&key,&at);
    if(status!=XR_XIR_OK) return status;
    if(at<read->final->capture.count && !effect_capture_before(&key,&read->final->capture.facts[at]))
        *output=&read->final->capture.facts[at];
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_emit(EffectCaptureRead *read,EffectCaptureFact fact) {
    if(read->fixed_only && (fact.word!=EFFECT_CAPTURE_FLAGS || fact.bit!=EFFECT_CAPTURE_FIXED)) return XR_XIR_OK;
    if(read->selected) {
        if(!effect_capture_before(&fact,read->selected) && !effect_capture_before(read->selected,&fact) &&
            fact.rank<=read->selected->rank) read->supported=true;
        return XR_XIR_OK;
    }
    uint32_t at=0;XrXirStatus status=effect_capture_lower(read,&fact,&at);if(status!=XR_XIR_OK) return status;
    const EffectCaptureProof *current=&read->final->capture;
    const EffectCaptureFact *found=at<current->count?&current->facts[at]:NULL;
    if(found && effect_capture_before(&fact,found)) found=NULL;
    if(!read->builder) return found && found->rank<=fact.rank?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    EffectCaptureProof *proof=read->builder;
    if(found) {
        if(fact.rank<found->rank) {
            if(!xir_compile_work(read->work,sizeof(fact.rank))) return XR_XIR_BUDGET;
            proof->facts[at].rank=fact.rank;read->changed=true;
        }
        return XR_XIR_OK;
    }
    /* An untrusted top proposal cannot transfer into a new coordinate. Real
     * seeds must lower its rank before construction can use that cause. */
    if(fact.rank>=read->maximum) return XR_XIR_OK;
    if(proof->count==proof->capacity) {
        uint32_t capacity=proof->capacity?proof->capacity:16;
        if(capacity>read->maximum) capacity=read->maximum;
        if(proof->capacity) capacity=capacity>read->maximum/2?read->maximum:capacity*2;
        if(capacity<=proof->count || (uint64_t)capacity>SIZE_MAX/sizeof(fact)) return XR_XIR_BUDGET;
        EffectCaptureFact *facts=xir_compile_alloc(read->work,(size_t)capacity*sizeof(*facts),&status);
        if(!facts) return status;
        if(!xir_compile_work(read->work,(uint64_t)proof->count*sizeof(*facts))) {
            xr_compile_resources_free(facts);return XR_XIR_BUDGET;
        }
        if(proof->count) memcpy(facts,proof->facts,(size_t)proof->count*sizeof(*facts));
        xr_compile_resources_free(proof->facts);proof->facts=facts;proof->capacity=capacity;
    }
    if(!xir_compile_work(read->work,((uint64_t)proof->count-at+1)*sizeof(fact))) return XR_XIR_BUDGET;
    memmove(proof->facts+at+1,proof->facts+at,(size_t)(proof->count-at)*sizeof(fact));
    proof->facts[at]=fact;++proof->count;read->changed=true;return XR_XIR_OK;
}
static XrXirStatus effect_capture_node(const EffectCaptureRead *read,uint32_t index,EffectCaptureNode *output) {
    if(index>=read->final->count) return XR_XIR_BAD_STRUCTURE;
    if(read->node_view && read->node_view->node==index) {
        if(!xir_compile_work(read->work,sizeof(*output))) return XR_XIR_BUDGET;
        *output=*read->node_view;return XR_XIR_OK;
    }
    const EffectInvocationNode *node=&read->final->nodes[index];
    if(node->body>=read->body.domain.function_count || node->root>=read->body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectCaptureNode local={.node=index};EffectBodyFunctionDimensions root={0};
    XrXirStatus status=effect_body_request_head(&read->body,node->body,&local.head);
    if(status==XR_XIR_OK) status=effect_body_request_dimensions(&read->body,node->root,&root);
    if(status==XR_XIR_OK) status=effect_invocation_basis_parameters_read(read->work,read->final,root.parameter_count,&local.basis);
    if(status!=XR_XIR_OK) return status;
    uint64_t values=(uint64_t)local.head.parameter_count+local.head.instruction_count;
    if(values+local.head.parameter_count>=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    local.values=(uint32_t)values;
    if(!xir_compile_work(read->work,sizeof(local))) return XR_XIR_BUDGET;
    *output=local;return XR_XIR_OK;
}
/* A receiving clause may borrow its current stack head. Construction has no
 * such view: an emit can allocate and invalidate every mutable array address. */
static XrXirStatus effect_capture_node_borrow(const EffectCaptureRead *read,uint32_t index,
    EffectCaptureNode *storage,const EffectCaptureNode **output) {
    if(read->node_view && read->node_view->node==index) {
        if(!xir_compile_work(read->work,sizeof(*output))) return XR_XIR_BUDGET;
        *output=read->node_view;return XR_XIR_OK;
    }
    XrXirStatus status=effect_capture_node(read,index,storage);
    if(status==XR_XIR_OK) *output=storage;
    return status;
}
static XrXirStatus effect_capture_kind(const EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t value,uint32_t *output,XrXirType *provided) {
    XrXirType type=0;XrXirStatus status=XR_XIR_OK;
    if(value<node->values) status=effect_body_request_type(&read->body,read->final->nodes[node->node].body,value,&type);
    else if(value==node->values) type=node->head.result;
    else if(value-node->values-1<node->head.parameter_count) type=node->head.parameters[value-node->values-1];
    else return XR_XIR_BAD_STRUCTURE;
    if(status!=XR_XIR_OK) return status;
    uint32_t kind=EFFECT_CAPTURE_SCALAR;
    if(xr_xir_callable_signature(read->body.domain.types,type)) kind=EFFECT_CAPTURE_CALLABLE;
    else if(xr_xir_type_is_cell(read->body.domain.types,type)) kind=EFFECT_CAPTURE_CELL;
    else if(xr_xir_type_is_class(read->body.domain.types,type)) kind=EFFECT_CAPTURE_CLASS;
    *output=kind;if(provided) *provided=type;return XR_XIR_OK;
}
static XrXirStatus effect_capture_word(const EffectCaptureRead *read,EffectCaptureFact key,uint64_t *output) {
    key.bit=0;uint32_t at=0;XrXirStatus status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
    uint64_t bits=0;
    for(;at<read->final->capture.count;++at) {
        if(!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
        const EffectCaptureFact *fact=&read->final->capture.facts[at];
        if(fact->node!=key.node || fact->value!=key.value || fact->word!=key.word) break;
        if(fact->bit>=64) return XR_XIR_BAD_STRUCTURE;
        bits|=UINT64_C(1)<<fact->bit;
    }
    *output=bits;return XR_XIR_OK;
}

typedef struct EffectCaptureTransfer {
    uint32_t from,value,to,destination,first,word,words;
    uint64_t rank;
    bool initial;
} EffectCaptureTransfer;

/* Sorted insertions can move both the allocation and the next source record.
 * Only copied scalar coordinates survive an emit or an allocator callback. */
static XrXirStatus effect_capture_transfer(EffectCaptureRead *read,const EffectCaptureTransfer *edge) {
    if(read->selected && (read->selected->node!=edge->to || read->selected->value!=edge->destination ||
        read->selected->word<edge->word || read->selected->word-edge->word>=edge->words)) return XR_XIR_OK;
    EffectCaptureNode target_storage={0};
    const EffectCaptureNode *target=NULL;
    XrXirStatus status=effect_capture_node_borrow(read,edge->to,&target_storage,&target);
    if(status!=XR_XIR_OK) return status;
    uint32_t initial=edge->destination-target->values-1;
    if(edge->initial && (edge->destination<=target->values || initial>=target->head.parameter_count))
        return XR_XIR_BAD_STRUCTURE;
    if(read->selected) {
        const EffectCaptureFact *selected=read->selected;
        if(selected->node!=edge->to || selected->value!=edge->destination || selected->word<edge->word ||
            selected->word-edge->word>=edge->words) return XR_XIR_OK;
        if(edge->initial) {
            const uint64_t *input=read->final->nodes[edge->to].input+(uint64_t)initial*target->basis.row_words;
            if(!(input[selected->word]&(UINT64_C(1)<<selected->bit))) return XR_XIR_OK;
        }
        const EffectCaptureFact *source=NULL;
        EffectCaptureFact key={edge->from,edge->value,edge->first+selected->word-edge->word,selected->bit,0};
        status=effect_capture_find(read,key,&source);if(status!=XR_XIR_OK || !source) return status;
        uint64_t rank=source->rank>edge->rank?source->rank:edge->rank;
        if(rank==UINT64_MAX) return XR_XIR_BAD_STRUCTURE;
        return effect_capture_emit(read,(EffectCaptureFact){edge->to,edge->destination,selected->word,selected->bit,rank+1});
    }
    EffectCaptureFact key={edge->from,edge->value,0,0,0};uint32_t at=0;
    status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
    while(at<read->final->capture.count) {
        if(!xir_compile_work(read->work,7)) return XR_XIR_BUDGET;
        EffectCaptureFact source=read->final->capture.facts[at];
        if(source.node!=edge->from || source.value!=edge->value) break;
        bool selected=source.word>=edge->first && source.word-edge->first<edge->words;
        if(selected) {
            uint32_t word=edge->word+source.word-edge->first;
            bool claimed=true;
            if(edge->initial) {
                const uint64_t *input=read->final->nodes[edge->to].input+(uint64_t)initial*target->basis.row_words;
                if(word>=target->basis.row_words || source.bit>=64) return XR_XIR_BAD_STRUCTURE;
                claimed=(input[word]&(UINT64_C(1)<<source.bit))!=0;
            }
            if(claimed) {
                uint64_t rank=source.rank>edge->rank?source.rank:edge->rank;
                if(rank==UINT64_MAX) return XR_XIR_BAD_STRUCTURE;
                status=effect_capture_emit(read,(EffectCaptureFact){edge->to,edge->destination,word,source.bit,rank+1});
                if(status!=XR_XIR_OK) return status;
            }
        }
        status=effect_capture_lower(read,&source,&at);if(status!=XR_XIR_OK) return status;++at;
    }
    return XR_XIR_OK;
}

/* An unused type-pool Cell does not create a value relation. The existing
 * complete local-input walk proves absence from actual heads and affine runs,
 * using fresh stack cursors and the original typed/opcode rules and charges. */
static XrXirStatus effect_capture_domain(EffectCaptureRead *read) {
    read->active=false;EffectInvocationLocalRead local={read->work,read->final->material,0};
    for(uint32_t f=0;f<read->body.domain.function_count;++f) {
        bool empty=false;XrXirStatus status=effect_invocation_local_head(read->work,read->final->material,f,&empty);
        if(status!=XR_XIR_OK) return status;
        if(!empty) { read->active=true;return XR_XIR_OK; }
        const EffectBodyFunction *head=&read->final->material->functions[f];EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        EffectInvocationLocalInput output={0};
        for(uint32_t r=0;r<head->run_count;++r) {
            const EffectBodyRun *run=NULL;status=effect_body_range_ref(&read->body.read,head,&cursor,&run);
            if(status==XR_XIR_OK) status=effect_invocation_local_run(&local,f,run,&output,&empty);
            if(status!=XR_XIR_OK) return status;
            if(!empty) { read->active=true;return XR_XIR_OK; }
        }
        if(cursor.first!=head->instruction_count || cursor.run!=head->run_count) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_capture_frame(EffectCaptureRead *read) {
    XrXirStatus status=effect_role_range(read->work,read->final,1,sizeof(*read->final));
    if(status==XR_XIR_OK) status=effect_role_material_frame(read->work,read->final->material);
    if(status!=XR_XIR_OK) return status;
    const EffectInvocationFinal *final=read->final;
    if(!final->count || final->count>final->capacity || final->edge_count>final->edge_capacity) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(read->work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status==XR_XIR_OK) status=effect_role_range(read->work,final->edges,final->edge_capacity,sizeof(*final->edges));
    if(status==XR_XIR_OK) status=effect_role_range(read->work,final->roots,effect_body_count(final->material),sizeof(*final->roots));
    if(status==XR_XIR_OK) status=effect_role_range(read->work,final->root_spaces,effect_body_count(final->material),sizeof(*final->root_spaces));
    if(status!=XR_XIR_OK) return status;
    read->body=effect_body_owned_request(read->work,final->material);
    status=effect_final_inventory_sites(read->work,final->material,final);if(status!=XR_XIR_OK) return status;
    status=effect_capture_domain(read);if(status!=XR_XIR_OK) return status;
    uint64_t maximum=0;
    for(uint32_t n=0;n<final->count;++n) {
        EffectCaptureNode node={0};status=effect_capture_node(read,n,&node);if(status!=XR_XIR_OK) return status;
        uint64_t width=(uint64_t)node.basis.row_words*64+3+final->binding_count;
        uint64_t values=(uint64_t)node.values+node.head.parameter_count+1;
        if(width && values>(UINT32_MAX-maximum)/width) maximum=UINT32_MAX;else maximum+=values*width;
        status=effect_role_range(read->work,final->nodes[n].input,(uint64_t)node.head.parameter_count*node.basis.row_words,sizeof(uint64_t));
        if(status!=XR_XIR_OK) return status;
    }
    read->maximum=(uint32_t)maximum;const EffectCaptureProof *proof=&final->capture;
    if(proof->count>proof->capacity || proof->capacity>maximum || !!proof->facts!=!!proof->capacity) return XR_XIR_BAD_STRUCTURE;
    if(!read->active && (proof->count || proof->capacity || final->site_count || final->edge_count ||
        final->count!=read->body.domain.function_count)) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(read->work,proof->facts,proof->capacity,sizeof(*proof->facts));if(status!=XR_XIR_OK) return status;
    EffectCaptureNode node={0};uint32_t kind=0,value=0;bool have_node=false,have_kind=false;
    for(uint32_t p=0;p<proof->count;++p) {
        if(!xir_compile_work(read->work,8)) return XR_XIR_BUDGET;
        const EffectCaptureFact *fact=&proof->facts[p];
        if(fact->node>=final->count || fact->rank>=proof->count || (p && !effect_capture_before(&proof->facts[p-1],fact)))
            return XR_XIR_BAD_STRUCTURE;
        if(!have_node || node.node!=fact->node) {
            status=effect_capture_node(read,fact->node,&node);if(status!=XR_XIR_OK) return status;
            have_node=true;have_kind=false;
        }
        /* Sorted coordinates share their actual head and SSA type within this
         * uninterrupted frame read. No callback or saved verdict intervenes. */
        if(!have_kind || value!=fact->value) {
            status=effect_capture_kind(read,&node,fact->value,&kind,NULL);if(status!=XR_XIR_OK) return status;
            value=fact->value;have_kind=true;
        }
        if(fact->word==EFFECT_CAPTURE_FLAGS) {
            bool callable=kind==EFFECT_CAPTURE_CALLABLE &&
                ((fact->value<node.values && fact->bit<=EFFECT_CAPTURE_RAW) ||
                 (fact->bit>=EFFECT_CAPTURE_COMPLETE && fact->bit-EFFECT_CAPTURE_COMPLETE<=final->binding_count));
            bool object=kind==EFFECT_CAPTURE_CLASS && fact->value<=node.values && fact->bit==EFFECT_CAPTURE_RAW;
            if(!callable && !object) return XR_XIR_BAD_STRUCTURE;
        } else {
            if(fact->bit>=64 || kind==EFFECT_CAPTURE_SCALAR) return XR_XIR_BAD_STRUCTURE;
            if(kind==EFFECT_CAPTURE_CALLABLE) { if(fact->word>=node.basis.fn_words) return XR_XIR_BAD_STRUCTURE; }
            else if(fact->word<node.basis.fn_words || fact->word>=node.basis.row_words ||
                (uint64_t)(fact->word-node.basis.fn_words)*64+fact->bit>=(uint64_t)node.basis.parameters+3)
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}

#include "xxir_effect_capture_reference.inc.c"
#include "xxir_effect_capture_values.inc.c"
#include "xxir_effect_capture_edges.inc.c"
#include "xxir_effect_capture_complete.inc.c"

static XrXirStatus effect_capture_selected_node(EffectCaptureRead *read,const EffectCaptureNode *node) {
    const EffectCaptureFact *fact=read->selected;uint32_t value=fact->value;
    XrXirStatus status=effect_capture_parameters(read,node);if(status!=XR_XIR_OK || read->supported) return status;
    if(value>node->values) return XR_XIR_OK;
    uint32_t own=UINT32_MAX;
    if(value>=node->head.parameter_count && value<node->values) {
        own=value-node->head.parameter_count;XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_body_request_instruction(&read->body,read->final->nodes[node->node].body,own,&storage,&op);
        if(status==XR_XIR_OK) status=effect_capture_instruction(read,node,own,op);
        if(status!=XR_XIR_OK || read->supported) return status;
    }
    uint32_t selection=value==node->values?EFFECT_FINAL_SELECT_RETURN:
        fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_FIXED?EFFECT_FINAL_SELECT_BARRIER:EFFECT_FINAL_SELECT_WRITE;
    bool returned=false;
    for(uint32_t pass=0;pass<2;++pass) {
        if(pass && (selection==EFFECT_FINAL_SELECT_RETURN || selection==EFFECT_FINAL_SELECT_WRITE)) break;
        uint32_t current=pass?EFFECT_FINAL_SELECT_WRITE:selection;EffectFinalInputSelect cursor;
        status=effect_final_input_select_begin(&read->body,current,&cursor);if(status!=XR_XIR_OK) return status;
        for(;;) {
            XrXirInstruction op;uint32_t i=0;bool found=false;
            status=effect_final_input_select_next(&read->body,read->final->nodes[node->node].body,&cursor,&i,&op,&found);
            if(status!=XR_XIR_OK) return status;
            if(!found) break;
            if(current==EFFECT_FINAL_SELECT_RETURN) returned=true;
            if(i==own || (current==EFFECT_FINAL_SELECT_WRITE && op.args[0]!=value)) continue;
            if(current==EFFECT_FINAL_SELECT_BARRIER) status=effect_capture_barrier(read,node,&op);
            else status=effect_capture_instruction(read,node,i,&op);
            if(status!=XR_XIR_OK || read->supported) return status;
        }
    }
    if(!read->fixed_only && value==node->values && !returned && xr_xir_type_is_class(read->body.domain.types,node->head.result)) {
        status=effect_capture_bit(read,node,node->values,node->basis.fn_words,node->basis.parameters+2,0);
        if(status==XR_XIR_OK) status=effect_capture_bit(read,node,node->values,node->basis.fn_words,node->basis.parameters+1,0);
        if(status==XR_XIR_OK) status=effect_capture_emit(read,(EffectCaptureFact){node->node,node->values,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0});
    }
    return status;
}
static XrXirStatus effect_capture_clauses(EffectCaptureRead *read) {
    if(!read->active) return XR_XIR_OK;
    if(read->selected && read->node_view && read->selected->node==read->node_view->node) {
        XrXirStatus status=effect_capture_selected_node(read,read->node_view);
        if(status!=XR_XIR_OK || read->supported || read->fixed_only) return status;
        return effect_capture_edges(read);
    }
    bool seeds_only=read->fixed_only && !read->selected;
    if(seeds_only) for(uint32_t p=0;p<read->final->capture.count;++p) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        const EffectCaptureFact *fact=&read->final->capture.facts[p];
        if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_FIXED) { seeds_only=false;break; }
    }
    for(uint32_t n=0;n<read->final->count;++n) {
        if(read->selected && read->selected->node!=n) continue;
        EffectCaptureNode node={0};XrXirStatus status=effect_capture_node(read,n,&node);
        if(status==XR_XIR_OK && read->selected) {
            const EffectCaptureNode *saved=read->node_view;read->node_view=&node;
            status=effect_capture_selected_node(read,&node);read->node_view=saved;
            if(status!=XR_XIR_OK) return status;
            if(read->supported) return XR_XIR_OK;
            continue;
        }
        if(status==XR_XIR_OK) status=effect_capture_parameters(read,&node);
        if(status!=XR_XIR_OK) return status;
        /* Empty fixed relations have no alias causes. Actual storage barriers
         * are their only seeds; finding one makes the next build round use
         * the full transfer rules. Receiving still checks every real seed. */
        if(seeds_only) {
            EffectFinalInputSelect barriers;
            status=effect_final_input_select_begin(&read->body,EFFECT_FINAL_SELECT_BARRIER,&barriers);
            if(status!=XR_XIR_OK) return status;
            for(;;) {
                XrXirInstruction op;uint32_t i=0;bool found=false;
                status=effect_final_input_select_next(&read->body,read->final->nodes[n].body,&barriers,&i,&op,&found);
                if(status!=XR_XIR_OK) return status;
                if(!found) break;
                status=effect_capture_barrier(read,&node,&op);if(status!=XR_XIR_OK) return status;
            }
            continue;
        }
        EffectBodyRecordCursor cursor;status=effect_body_request_sequence_begin(&read->body,&cursor);
        if(status!=XR_XIR_OK) return status;
        bool returned=false;
        for(uint32_t i=0;i<node.head.instruction_count;++i) {
            if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_body_request_sequence_instruction(&read->body,read->final->nodes[n].body,i,&cursor,&storage,&op);
            if(status==XR_XIR_OK) {
                if(op->op==XR_XIR_RETURN) returned=true;
                status=effect_capture_instruction(read,&node,i,op);
            }
            if(status!=XR_XIR_OK) return status;
        }
        status=effect_body_request_sequence_finish(&read->body,read->final->nodes[n].body,&cursor);
        if(status!=XR_XIR_OK) return status;
        if(!read->fixed_only && !returned && xr_xir_type_is_class(read->body.domain.types,node.head.result)) {
            status=effect_capture_bit(read,&node,node.values,node.basis.fn_words,node.basis.parameters+2,0);
            if(status==XR_XIR_OK) status=effect_capture_bit(read,&node,node.values,node.basis.fn_words,node.basis.parameters+1,0);
            if(status==XR_XIR_OK) status=effect_capture_emit(read,(EffectCaptureFact){n,node.values,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0});
            if(status!=XR_XIR_OK) return status;
        }
    }
    return read->fixed_only?XR_XIR_OK:effect_capture_edges(read);
}
static XrXirStatus effect_capture_match(const XrXirCompileContext *work,const EffectInvocationFinal *final) {
    if(!final) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureRead read={.work=work,.final=final,.fallback=true,.fixed_only=true,.class_results=true};
    XrXirStatus status=effect_capture_frame(&read);if(status!=XR_XIR_OK || !read.active) return status;
    EffectBodyCurrentRead current={0};status=effect_body_current_open(work,final->material,&current);
    if(status!=XR_XIR_OK) return status;
    read.body.current=&current;
    /* Fixedness is independent of origins, so negative fixed queries consume
     * its complete local closure and positive support before origin clauses. */
    EffectCaptureNode node={0};
    for(uint32_t domain=0;domain<2;++domain) {
        read.fixed_only=domain==0;read.selected=NULL;read.node_view=NULL;
        status=effect_capture_clauses(&read);if(status!=XR_XIR_OK) return status;
        for(uint32_t p=0;p<final->capture.count;++p) {
            const EffectCaptureFact *fact=&final->capture.facts[p];
            if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit>=EFFECT_CAPTURE_COMPLETE) continue;
            if(read.fixed_only && (fact->word!=EFFECT_CAPTURE_FLAGS || fact->bit!=EFFECT_CAPTURE_FIXED)) continue;
            /* This head is borrowed only within one uninterrupted, allocation-
             * free receiving request. Construction never retains this view. */
            if(!read.node_view || read.node_view->node!=fact->node) {
                read.node_view=NULL;status=effect_capture_node(&read,fact->node,&node);
                if(status!=XR_XIR_OK) return status;
                read.node_view=&node;
            }
            read.selected=fact;read.supported=false;
            status=effect_capture_clauses(&read);if(status!=XR_XIR_OK) return status;
            if(!read.supported) return XR_XIR_BAD_STRUCTURE;
        }
    }
    read.selected=NULL;read.node_view=NULL;
    status=effect_capture_complete_clauses(&read);if(status!=XR_XIR_OK) return status;
    return effect_capture_inputs_match(&read);
}
static XrXirStatus effect_capture_build(const XrXirCompileContext *work,EffectInvocationFinal *final) {
    if(!final || final->capture.facts || final->capture.count || final->capture.capacity) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureRead read={.work=work,.final=final,.fixed_only=true};XrXirStatus status=effect_capture_frame(&read);
    /* The fresh actual-input negative proof admits only an empty relation.
     * No callback occurs before returning these empty construction bytes. */
    if(status==XR_XIR_OK && !read.active) return XR_XIR_OK;
    if(status==XR_XIR_OK) {
        read.builder=&final->capture;
        for(uint32_t domain=0;domain<4 && status==XR_XIR_OK;++domain) {
            if(domain==3) {
                bool proposed=false;
                for(uint32_t n=0;n<final->count;++n) {
                    if(!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
                    proposed|=final->nodes[n].class_count!=0 || final->nodes[n].class_return_mask!=0;
                }
                /* Builder proposals select work only. The complete receiving
                 * clauses below still require every current Class result. */
                if(status!=XR_XIR_OK || !proposed) break;
            }
            read.fixed_only=domain==0;read.fallback=domain>=2;read.class_results=domain==3;
            if(domain==1) status=effect_capture_class_proposals(&read);
            if(status!=XR_XIR_OK) break;
            do { read.changed=false;status=effect_capture_clauses(&read); } while(status==XR_XIR_OK && read.changed);
        }
    }
    if(status==XR_XIR_OK) {
        read.fixed_only=false;
        do { read.changed=false;status=effect_capture_complete_clauses(&read); } while(status==XR_XIR_OK && read.changed);
    }
    if(status==XR_XIR_OK) status=effect_capture_match(work,final);
    if(status!=XR_XIR_OK) { xr_compile_resources_free(final->capture.facts);final->capture=(EffectCaptureProof){0}; }
    return status;
}
