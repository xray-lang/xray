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
enum { EFFECT_CAPTURE_SCALAR, EFFECT_CAPTURE_CALLABLE, EFFECT_CAPTURE_CELL, EFFECT_CAPTURE_CLASS };

typedef struct EffectCaptureHead {
    const XrXirType *parameters;
    const uint32_t *operands;
    uint32_t parameter_count,instruction_count,operand_count;
    XrXirType result;
} EffectCaptureHead;
typedef struct EffectCaptureNode {
    EffectCaptureHead head;
    EffectInvocationBasis basis;
    uint32_t node,values;
} EffectCaptureNode;
typedef struct EffectCaptureSchedule EffectCaptureSchedule;
typedef struct EffectCaptureActualRead EffectCaptureActualRead;
typedef struct EffectCaptureCompleteTypes EffectCaptureCompleteTypes;
typedef struct EffectCaptureRead {
    const XrXirCompileContext *work;
    const EffectInvocationFinal *final;
    EffectBodyRequest body;
    EffectCaptureProof *builder;
    EffectCaptureSchedule *schedule;
    const EffectCaptureFact *selected;
    uint32_t selected_count;
    uint64_t selected_mask;
    const EffectCaptureNode *node_view,*node_views,*edge_views;
    EffectCaptureActualRead *actuals;
    EffectCaptureCompleteTypes *complete_types;
    const uint32_t *fact_nodes;
    uint64_t *support_bits;
    uint32_t support_first,support_count;
    uint32_t node_view_count,edge_view_count,maximum;
    bool fixed_only,fallback,class_results,active,changed,supported,complete_scope;
} EffectCaptureRead;

static XrXirStatus effect_capture_schedule_changed(EffectCaptureRead *read,uint32_t node);

static bool effect_capture_before(const EffectCaptureFact *a,const EffectCaptureFact *b) {
    return a->node<b->node || (a->node==b->node && (a->value<b->value ||
        (a->value==b->value && (a->word<b->word || (a->word==b->word && a->bit<b->bit)))));
}
static XrXirStatus effect_capture_lower(const EffectCaptureRead *read,const EffectCaptureFact *key,uint32_t *output) {
    const EffectCaptureProof *proof=&read->final->capture;uint32_t lo=0,hi=proof->count;
    if(read->fact_nodes && key->node<read->final->count) {
        if(read->builder) return read->builder?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET;
        lo=read->fact_nodes[key->node];hi=read->fact_nodes[key->node+1];
        if(lo>hi || hi>proof->count) return XR_XIR_BAD_STRUCTURE;
    }
    while(lo<hi) {

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
/* Clauses may share one uninterrupted read at the same node and value.
 * Each coordinate still needs its own descending cause; support is the
 * conjunction of all witnesses, including their individual rank bounds. */
static XrXirStatus effect_capture_support_emit(EffectCaptureRead *read,const EffectCaptureFact *fact) {
    if(!read->selected_count || read->selected_count>64) return XR_XIR_BAD_STRUCTURE;
    if(fact->node!=read->selected->node || fact->value!=read->selected->value) return XR_XIR_OK;
    uint32_t lo=0,hi=read->selected_count;
    while(lo<hi) {

        uint32_t mid=lo+(hi-lo)/2;const EffectCaptureFact *wanted=&read->selected[mid];
        if(wanted->word<fact->word || (wanted->word==fact->word && wanted->bit<fact->bit)) lo=mid+1;else hi=mid;
    }
    if(lo<read->selected_count) {

        const EffectCaptureFact *wanted=&read->selected[lo];
        if(wanted->word==fact->word && wanted->bit==fact->bit && fact->rank<=wanted->rank) {

            read->selected_mask|=UINT64_C(1)<<lo;
            uint64_t all=read->selected_count==64?UINT64_MAX:(UINT64_C(1)<<read->selected_count)-1;
            read->supported=read->selected_mask==all;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_emit(EffectCaptureRead *read,EffectCaptureFact fact) {
    if(read->complete_scope && (fact.word!=EFFECT_CAPTURE_FLAGS || fact.bit<EFFECT_CAPTURE_COMPLETE)) return XR_XIR_BAD_STRUCTURE;
    if(read->fixed_only && (fact.word!=EFFECT_CAPTURE_FLAGS || fact.bit!=EFFECT_CAPTURE_FIXED)) return XR_XIR_OK;
    if(read->selected) return effect_capture_support_emit(read,&fact);
    uint32_t at=0;XrXirStatus status=effect_capture_lower(read,&fact,&at);if(status!=XR_XIR_OK) return status;
    const EffectCaptureProof *current=&read->final->capture;
    const EffectCaptureFact *found=at<current->count?&current->facts[at]:NULL;
    if(found && effect_capture_before(&fact,found)) found=NULL;
    if(!read->builder) {
        if(!found || found->rank>fact.rank) return XR_XIR_BAD_STRUCTURE;
        /* Closure bounds every actual cause from below. Equality also proves
         * this stored coordinate has its own authentic descending support. */
        if(read->support_bits && at>=read->support_first && at-read->support_first<read->support_count &&
            found->rank==fact.rank) {

            uint32_t bit=at-read->support_first;
            read->support_bits[bit/64]|=UINT64_C(1)<<(bit%64);
        }
        return XR_XIR_OK;
    }
    EffectCaptureProof *proof=read->builder;
    if(found) {
        if(fact.rank<found->rank) {

            proof->facts[at].rank=fact.rank;read->changed=true;
            return effect_capture_schedule_changed(read,fact.node);
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
        /* Lookup loans never cross either allocator or release callbacks. */
        read->edge_views=NULL;read->edge_view_count=0;read->node_view=NULL;
        if(read->complete_scope) { read->node_views=NULL;read->node_view_count=0;read->actuals=NULL; }
        EffectCaptureFact *facts=xir_compile_alloc(read->work,(size_t)capacity*sizeof(*facts),&status);
        if(!facts) return status;

        if(proof->count) memcpy(facts,proof->facts,(size_t)proof->count*sizeof(*facts));
        xr_compile_resources_free(proof->facts);proof->facts=facts;proof->capacity=capacity;
    }

    memmove(proof->facts+at+1,proof->facts+at,(size_t)(proof->count-at)*sizeof(fact));
    proof->facts[at]=fact;++proof->count;read->changed=true;
    return effect_capture_schedule_changed(read,fact.node);
}
/* Capture clauses consume no names or block vectors. The owning adapter uses
 * the same complete head-shape predicate and copies only their current fields;
 * a delegated reader still supplies its full original head response. */
static XrXirStatus effect_capture_head(const EffectCaptureRead *read,uint32_t function,EffectCaptureHead *output) {
    if(effect_body_request_owned_matches(read->work,&read->body,read->final->material)) {
        const EffectBodyFunction *head=effect_body_owned_function(&read->body,function);
        if(!head) return XR_XIR_BAD_STRUCTURE;

        *output=(EffectCaptureHead){head->parameters,head->operands,head->parameter_count,
            head->instruction_count,head->operand_count,head->result};
        return XR_XIR_OK;
    }
    EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(&read->body,function,&head);
    if(status!=XR_XIR_OK) return status;

    *output=(EffectCaptureHead){head.parameters,head.operands,head.parameter_count,
        head.instruction_count,head.operand_count,head.result};
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_node(const EffectCaptureRead *read,uint32_t index,EffectCaptureNode *output) {
    if(index>=read->final->count) return XR_XIR_BAD_STRUCTURE;
    if(read->node_views && index<read->node_view_count) {
        if(read->node_views[index].node!=index) return XR_XIR_BAD_STRUCTURE;

        *output=read->node_views[index];return XR_XIR_OK;
    }
    for(uint32_t n=0;read->edge_views && n<read->edge_view_count;++n) {

        if(read->edge_views[n].node!=index) continue;

        *output=read->edge_views[n];return XR_XIR_OK;
    }
    if(read->node_view && read->node_view->node==index) {

        *output=*read->node_view;return XR_XIR_OK;
    }
    const EffectInvocationNode *node=&read->final->nodes[index];
    if(node->body>=read->body.domain.function_count || node->root>=read->body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectCaptureNode local={.node=index};EffectBodyFunctionDimensions root={0};
    XrXirStatus status=effect_capture_head(read,node->body,&local.head);

    if(status==XR_XIR_OK && (node->parameter_count!=local.head.parameter_count ||
        node->instruction_count!=local.head.instruction_count)) status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK) status=effect_body_request_dimensions(&read->body,node->root,&root);
    if(status==XR_XIR_OK) status=effect_invocation_basis_parameters_read(read->work,read->final,root.parameter_count,&local.basis);
    if(status!=XR_XIR_OK) return status;
    uint64_t values=(uint64_t)local.head.parameter_count+local.head.instruction_count;
    if(values+local.head.parameter_count>=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    local.values=(uint32_t)values;

    *output=local;return XR_XIR_OK;
}
/* Receiving borrows current heads within its uninterrupted read. A constructor
 * edge loan is shorter: every possible fact growth drops it before callbacks. */
static XrXirStatus effect_capture_node_borrow(const EffectCaptureRead *read,uint32_t index,
    EffectCaptureNode *storage,const EffectCaptureNode **output) {
    if(index>=read->final->count) return XR_XIR_BAD_STRUCTURE;
    if(read->node_views && index<read->node_view_count) {
        if(read->node_views[index].node!=index) return XR_XIR_BAD_STRUCTURE;

        *output=&read->node_views[index];return XR_XIR_OK;
    }
    for(uint32_t n=0;read->edge_views && n<read->edge_view_count;++n) {

        if(read->edge_views[n].node!=index) continue;

        *output=&read->edge_views[n];return XR_XIR_OK;
    }
    if(read->node_view && read->node_view->node==index) {

        *output=read->node_view;return XR_XIR_OK;
    }
    XrXirStatus status=effect_capture_node(read,index,storage);
    if(status==XR_XIR_OK) *output=storage;
    return status;
}
static uint32_t effect_capture_type_kind(const XrXirTypes *types,XrXirType type) {
    if(effect_physical_origin(types,type)) return EFFECT_CAPTURE_CALLABLE;
    if(xr_xir_type_is_cell(types,type)) return EFFECT_CAPTURE_CELL;
    if(xr_xir_type_is_class(types,type)) return EFFECT_CAPTURE_CLASS;
    return EFFECT_CAPTURE_SCALAR;
}
static XrXirStatus effect_capture_kind(const EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t value,uint32_t *output,XrXirType *provided) {
    XrXirType type=0;XrXirStatus status=XR_XIR_OK;
    if(value<node->values) status=effect_body_request_type(&read->body,read->final->nodes[node->node].body,value,&type);
    else if(value==node->values) {
        const XrXirDeclarations *d=read->body.domain.declarations;
        uint32_t body=read->final->nodes[node->node].body;
        type=d && d->functions && (d->functions[body].promises&XR_XIR_FUNCTION_GENERATOR)?
            XR_XIR_UNIT:node->head.result;
    }
    else if(value-node->values-1<node->head.parameter_count) type=node->head.parameters[value-node->values-1];
    else return XR_XIR_BAD_STRUCTURE;
    if(status!=XR_XIR_OK) return status;
    *output=effect_capture_type_kind(read->body.domain.types,type);
    if(provided) *provided=type;return XR_XIR_OK;
}
static XrXirStatus effect_capture_word(const EffectCaptureRead *read,EffectCaptureFact key,uint64_t *output) {
    key.bit=0;uint32_t at=0;XrXirStatus status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
    uint64_t bits=0;
    for(;at<read->final->capture.count;++at) {

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
    if(read->selected && (read->selected->node!=edge->to || read->selected->value!=edge->destination)) return XR_XIR_OK;
    EffectCaptureNode target_storage={0};
    const EffectCaptureNode *target=NULL;
    XrXirStatus status=effect_capture_node_borrow(read,edge->to,&target_storage,&target);
    if(status!=XR_XIR_OK) return status;
    uint32_t initial=edge->destination-target->values-1;
    if(edge->initial && (edge->destination<=target->values || initial>=target->head.parameter_count))
        return XR_XIR_BAD_STRUCTURE;
    if(read->selected) {
        for(uint32_t p=0;p<read->selected_count;++p) {

            const EffectCaptureFact *selected=&read->selected[p];
            if(selected->word<edge->word || selected->word-edge->word>=edge->words) continue;
            if(edge->initial) {
                const uint64_t *input=read->final->nodes[edge->to].input+(uint64_t)initial*target->basis.row_words;
                if(!(input[selected->word]&(UINT64_C(1)<<selected->bit))) continue;
            }
            const EffectCaptureFact *source=NULL;
            EffectCaptureFact key={edge->from,edge->value,edge->first+selected->word-edge->word,selected->bit,0};
            status=effect_capture_find(read,key,&source);if(status!=XR_XIR_OK) return status;
            if(!source) continue;
            uint64_t rank=source->rank>edge->rank?source->rank:edge->rank;
            if(rank==UINT64_MAX) return XR_XIR_BAD_STRUCTURE;
            status=effect_capture_emit(read,(EffectCaptureFact){edge->to,edge->destination,selected->word,selected->bit,rank+1});
            if(status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    EffectCaptureFact key={edge->from,edge->value,0,0,0};uint32_t at=0;
    status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
    while(at<read->final->capture.count) {

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

static XrXirStatus effect_capture_frame_current(EffectCaptureRead *read,const EffectBodyCurrentRead *current) {
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
    for(uint32_t f=0;f<effect_body_count(final->material);++f) {

        uint32_t root=final->roots[f],space=final->root_spaces[f];
        if(root>=final->count || space>=effect_body_count(final->material) ||
            final->nodes[root].body!=f || final->nodes[root].root!=space) return XR_XIR_BAD_STRUCTURE;
    }
    read->body=effect_body_owned_request(read->work,final->material);
    if(current) {

        if(current->work!=read->work || current->material!=final->material) return XR_XIR_BAD_STRUCTURE;
        read->body.current=current;
    }
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

static XrXirStatus effect_capture_frame(EffectCaptureRead *read) {
    return effect_capture_frame_current(read,NULL);
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
        status=effect_body_request_operation(&read->body,read->final->nodes[node->node].body,own,&storage,&op);
        if(status==XR_XIR_OK) status=effect_capture_instruction(read,node,own,op);
        if(status!=XR_XIR_OK || read->supported) return status;
    }
    bool fixed=false;
    for(uint32_t p=0;p<read->selected_count;++p) {

        fixed|=read->selected[p].word==EFFECT_CAPTURE_FLAGS && read->selected[p].bit==EFFECT_CAPTURE_FIXED;
    }
    uint32_t selection=value==node->values?EFFECT_FINAL_SELECT_RETURN:
        fixed?EFFECT_FINAL_SELECT_BARRIER:EFFECT_FINAL_SELECT_WRITE;
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
/* Every non-scalar result and type-independent clause needs its original
 * transfer. Other scalar rows contribute no capture fact. This selects only
 * operand decoding; complete input framing, closure and support still run. */
static bool effect_capture_operand_read(const XrXirTypes *types,uint32_t opcode,XrXirType type) {
    return effect_capture_type_kind(types,type)!=EFFECT_CAPTURE_SCALAR ||
        (effect_physical_opcode((XrXirOp)opcode)&EFFECT_PHYSICAL_BARRIER) ||
        opcode==XR_XIR_LOCAL_WRITE || opcode==XR_XIR_SCALAR_LOCAL_WRITE || opcode==XR_XIR_OWNED_LOCAL_WRITE ||
        opcode==XR_XIR_FUNCTION_REF || opcode==XR_XIR_GENERATOR_NEW || opcode==XR_XIR_RETURN;
}
/* A missing result means this current scalar row has no capture transfer.
 * Its full record was framed before projection. No allocation occurs here. */
static XrXirStatus effect_capture_operation_ref(const EffectCaptureRead *read,
    const EffectBodyRun *ref,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    if(ref->mask && i!=ref->first) {
        uint32_t opcode=XR_XIR_INVALID,type=XR_XIR_UNIT;
        XrXirStatus status=effect_body_run_word(&read->body.read,ref,i,0,&opcode);
        if(status==XR_XIR_OK) status=effect_body_run_word(&read->body.read,ref,i,1,&type);
        if(status!=XR_XIR_OK) return status;

        if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        if(!effect_capture_operand_read(read->body.domain.types,opcode,(XrXirType)type)) return XR_XIR_OK;
    }
    return effect_body_operation_ref(&read->body.read,ref,i,storage,output);
}
/* Construction can grow facts between operations. Only the numeric cursor
 * survives; each selection reopens the current record before its transfer.
 * Delegated readers retain their original full-operation behavior. */
static XrXirStatus effect_capture_sequence_operation(const EffectCaptureRead *read,
    uint32_t function,uint32_t i,EffectBodyRecordCursor *cursor,XrXirInstruction *storage,
    const XrXirInstruction **output) {
    if(!effect_body_request_sequence_owned(&read->body))
        return effect_body_request_sequence_operation(&read->body,function,i,cursor,storage,output);
    const EffectBodyRun *ref=NULL;
    XrXirStatus status=effect_body_request_sequence_record(&read->body,function,i,cursor,&ref);
    return status==XR_XIR_OK?effect_capture_operation_ref(read,ref,i,storage,output):status;
}
/* Receiving cannot allocate or release. Varying rows first project their
 * current opcode and type; contributing rows keep their full original replay.
 * Constant rows already borrow the complete base without materialization.
 * The ordinary finish reader independently reopens the final record and EOF. */
static XrXirStatus effect_capture_current_body(EffectCaptureRead *read,const EffectCaptureNode *node,bool *returned) {
    const EffectBodyCurrentRead *current=read->body.current;
    if(read->builder || !current || current->work!=read->work || current->material!=read->final->material ||
        !effect_body_request_owned_matches(read->work,&read->body,read->final->material)) return XR_XIR_BAD_STRUCTURE;
    uint32_t function=read->final->nodes[node->node].body;
    const EffectBodyFunction *head=&read->final->material->functions[function];
    if(head->instruction_count!=node->head.instruction_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor,walk={0,0,UINT32_MAX};
    XrXirStatus status=effect_body_request_sequence_begin(&read->body,&cursor);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t r=0;r<head->run_count;++r) {
        const EffectBodyRun *ref=NULL;
        status=effect_body_range_ref(&read->body.read,head,&walk,&ref);
        if(status!=XR_XIR_OK) return status;
        uint32_t end=ref->first+ref->count;
        for(uint32_t i=ref->first;i<end;++i) {

            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_capture_operation_ref(read,ref,i,&storage,&op);
            if(status==XR_XIR_OK && op) {
                if(op->op==XR_XIR_RETURN) *returned=true;
                status=effect_capture_instruction(read,node,i,op);
            }
            if(status!=XR_XIR_OK) return status;
        }

        cursor=(EffectBodyRecordCursor){ref->first,r,end-1};
    }

    if(walk.first!=head->instruction_count || walk.run!=head->run_count) return XR_XIR_BAD_STRUCTURE;
    return effect_body_request_sequence_finish(&read->body,function,&cursor);
}
static XrXirStatus effect_capture_node_body(EffectCaptureRead *read,const EffectCaptureNode *node,bool seeds_only) {
    XrXirStatus status=effect_capture_parameters(read,node);
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
            status=effect_final_input_select_next(&read->body,read->final->nodes[node->node].body,&barriers,&i,&op,&found);
            if(status!=XR_XIR_OK) return status;
            if(!found) break;
            status=effect_capture_barrier(read,node,&op);if(status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    bool returned=false;
    if(!read->builder && read->body.current &&
        effect_body_request_owned_matches(read->work,&read->body,read->final->material))
        status=effect_capture_current_body(read,node,&returned);
    else {
        EffectBodyRecordCursor cursor;status=effect_body_request_sequence_begin(&read->body,&cursor);
        if(status!=XR_XIR_OK) return status;
        for(uint32_t i=0;i<node->head.instruction_count;++i) {

            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_capture_sequence_operation(read,read->final->nodes[node->node].body,i,&cursor,&storage,&op);
            if(status==XR_XIR_OK && op) {
                if(op->op==XR_XIR_RETURN) returned=true;
                status=effect_capture_instruction(read,node,i,op);
            }
            if(status!=XR_XIR_OK) return status;
        }
        status=effect_body_request_sequence_finish(&read->body,read->final->nodes[node->node].body,&cursor);
    }
    if(status!=XR_XIR_OK) return status;
    if(!read->fixed_only && !returned && xr_xir_type_is_class(read->body.domain.types,node->head.result)) {
        status=effect_capture_bit(read,node,node->values,node->basis.fn_words,node->basis.parameters+2,0);
        if(status==XR_XIR_OK) status=effect_capture_bit(read,node,node->values,node->basis.fn_words,node->basis.parameters+1,0);
        if(status==XR_XIR_OK) status=effect_capture_emit(read,(EffectCaptureFact){node->node,node->values,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0});
    }
    return status;
}
static XrXirStatus effect_capture_node_clauses(EffectCaptureRead *read,uint32_t n,bool seeds_only) {
    if(n>=read->final->count) return XR_XIR_BAD_STRUCTURE;
    if(read->selected && read->selected->node!=n) return XR_XIR_OK;
    EffectCaptureNode node={0};XrXirStatus status=effect_capture_node(read,n,&node);
    if(status!=XR_XIR_OK) return status;
    if(read->selected) {
        const EffectCaptureNode *saved=read->node_view;read->node_view=&node;
        status=effect_capture_selected_node(read,&node);read->node_view=saved;
        return status;
    }
    bool loan=read->builder && !read->node_view && !read->node_views;
    if(loan) read->node_view=&node;
    status=effect_capture_node_body(read,&node,seeds_only);
    if(loan) read->node_view=NULL;
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

        const EffectCaptureFact *fact=&read->final->capture.facts[p];
        if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_FIXED) { seeds_only=false;break; }
    }
    for(uint32_t n=0;n<read->final->count;++n) {
        XrXirStatus status=effect_capture_node_clauses(read,n,seeds_only);
        if(status!=XR_XIR_OK || read->supported) return status;
    }
    return read->fixed_only?XR_XIR_OK:effect_capture_edges(read);
}
/* Fallback adds only local sources to the preceding positive fixed point.
 * An unchanged local pass cannot change any already closed edge. Once a
 * source changes, preserve the original edge pass and ordered full sweeps. */
static XrXirStatus effect_capture_fallback_build(EffectCaptureRead *read) {
    if(!read->builder || read->selected || read->fixed_only || !read->fallback || read->class_results)
        return XR_XIR_BAD_STRUCTURE;
    read->changed=false;XrXirStatus status=XR_XIR_OK;
    for(uint32_t n=0;n<read->final->count && status==XR_XIR_OK;++n)
        status=effect_capture_node_clauses(read,n,false);
    if(status!=XR_XIR_OK || !read->changed) return status;
    status=effect_capture_edges(read);
    while(status==XR_XIR_OK && read->changed) {
        read->changed=false;status=effect_capture_clauses(read);
    }
    return status;
}
#include "xxir_effect_capture_scalar.inc.c"
#include "xxir_effect_capture_schedule.inc.c"
/* One complete current-clause replay proves closure and marks each individual
 * positive witness. A bounded stack window avoids allocation and keeps every
 * mark inside this read; larger proofs repeat the same full clauses per window.
 * Fixed support is finished before origins may consume a negative fixed query. */
static XrXirStatus effect_capture_receiving_domain(EffectCaptureRead *read) {
    uint64_t support[8];uint32_t first=0,count=read->final->capture.count;
    do {
        uint32_t length=count-first;
        if(length>XR_COUNTOF(support)*64) length=XR_COUNTOF(support)*64;
        uint32_t words=(length+63)/64;

        memset(support,0,(size_t)words*sizeof(*support));
        read->support_bits=support;read->support_first=first;read->support_count=length;
        XrXirStatus status=effect_capture_clauses(read);
        read->support_bits=NULL;read->support_first=read->support_count=0;
        if(status!=XR_XIR_OK) return status;
        for(uint32_t p=0;p<length;++p) {

            const EffectCaptureFact *fact=&read->final->capture.facts[first+p];
            if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit>=EFFECT_CAPTURE_COMPLETE) continue;
            if(read->fixed_only && (fact->word!=EFFECT_CAPTURE_FLAGS || fact->bit!=EFFECT_CAPTURE_FIXED)) continue;
            if(!(support[p/64]&(UINT64_C(1)<<(p%64)))) return XR_XIR_BAD_STRUCTURE;
        }
        first+=length;
    } while(first<count);
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_current_match(const XrXirCompileContext *work,
    const EffectInvocationFinal *final,const EffectBodyCurrentRead *current) {
    if(!final) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureRead read={.work=work,.final=final,.fallback=true,.fixed_only=true,.class_results=true};
    XrXirStatus status=effect_capture_frame_current(&read,current);if(status!=XR_XIR_OK || !read.active) return status;
    EffectBodyCurrentRead fresh={0};
    if(!current) {
        status=effect_body_current_open(work,final->material,&fresh);
        if(status!=XR_XIR_OK) return status;
        read.body.current=&fresh;
    }
    bool scalar=false;status=effect_capture_scalar_domain(&read,&scalar);
    if(status!=XR_XIR_OK) return status;
    if(scalar) return final->capture.count?XR_XIR_BAD_STRUCTURE:effect_capture_inputs_match(&read);
    /* These are fresh projections of the actual framed Input. The complete
     * request is allocation-free; no borrowed projection survives a callback
     * or this call, and every semantic closure/support check still runs. */
    EffectCaptureNode views[32];EffectCaptureActualRead actuals;uint32_t fact_nodes[33];
    if(final->count<=XR_COUNTOF(views)) {
        uint32_t at=0;
        for(uint32_t n=0;n<final->count;++n) {

            fact_nodes[n]=at;
            while(at<final->capture.count) {

                if(final->capture.facts[at].node!=n) break;
                ++at;
            }
        }
        if(at!=final->capture.count) return XR_XIR_BAD_STRUCTURE;

        fact_nodes[final->count]=at;read.fact_nodes=fact_nodes;
        for(uint32_t n=0;n<final->count;++n) {
            status=effect_capture_node(&read,n,&views[n]);if(status!=XR_XIR_OK) return status;
        }
        read.node_views=views;read.node_view_count=final->count;
    }
    /* Fixedness is independent of origins, so negative fixed queries consume
     * its complete local closure and positive support before origin clauses. */
    for(uint32_t domain=0;domain<2;++domain) {
        read.fixed_only=domain==0;read.selected=NULL;read.node_view=NULL;
        if(domain) {
            status=effect_capture_actuals_read(&read,&actuals);if(status!=XR_XIR_OK) return status;
            if(actuals.edges) read.actuals=&actuals;
        }
        status=effect_capture_receiving_domain(&read);if(status!=XR_XIR_OK) return status;
    }
    read.selected=NULL;read.node_view=NULL;
    status=effect_capture_complete_clauses(&read);if(status!=XR_XIR_OK) return status;
    return effect_capture_inputs_match(&read);
}
static XrXirStatus effect_capture_match(const XrXirCompileContext *work,const EffectInvocationFinal *final) {
    return effect_capture_current_match(work,final,NULL);
}
/* Construction and its immediate receiver own the same private Input. The
 * parent descriptor is discarded before every possible cleanup callback;
 * no child stack projection or semantic result crosses this boundary. */
static XrXirStatus effect_capture_build(const XrXirCompileContext *work,EffectInvocationFinal *final,
    const EffectInvocationWorkspace *discovery) {
    if(!final || !discovery || !discovery->read.work || work->resources!=discovery->read.work->resources ||
        final->material!=discovery->final.material || final->nodes!=discovery->final.nodes ||
        final->count!=discovery->final.count || final->capacity!=discovery->final.capacity ||
        final->capture.facts || final->capture.count || final->capture.capacity) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureRead read={.work=work,.final=final,.fixed_only=true};XrXirStatus status=effect_capture_frame(&read);
    /* The fresh actual-input negative proof admits only an empty relation.
     * No callback occurs before returning these empty construction bytes. */
    if(status==XR_XIR_OK && !read.active) return XR_XIR_OK;
    bool scalar=false;
    if(status==XR_XIR_OK) status=effect_capture_scalar_domain(&read,&scalar);
    if(status!=XR_XIR_OK) return status;
    if(scalar) return effect_capture_inputs_match(&read);
    EffectCaptureSchedule *schedule=NULL;
    status=effect_capture_schedule_create(&read,&schedule);
    if(status==XR_XIR_OK) status=effect_capture_frame(&read);
    if(status==XR_XIR_OK) {
        read.builder=&final->capture;
        for(uint32_t domain=0;domain<4 && status==XR_XIR_OK;++domain) {
            if(domain==3) {
                bool proposed=false;
                for(uint32_t n=0;n<final->count;++n) {

                    proposed|=discovery->states[n].classes.count!=0 || discovery->states[n].classes.return_mask!=0;
                }
                /* Builder proposals select work only. The complete receiving
                 * clauses below still require every current Class result. */
                if(status!=XR_XIR_OK || !proposed) break;
            }
            read.fixed_only=domain==0;read.fallback=domain>=2;read.class_results=domain==3;
            if(domain==1) status=effect_capture_class_proposals(&read,discovery);
            if(status!=XR_XIR_OK) break;
            if(domain==1) {
                status=effect_capture_schedule_run(&read,schedule);
            } else if(domain==2) status=effect_capture_fallback_build(&read);
            else do { read.changed=false;status=effect_capture_clauses(&read); } while(status==XR_XIR_OK && read.changed);
        }
    }
    read.schedule=NULL;
    xr_compile_resources_free(schedule);
    if(status==XR_XIR_OK) status=effect_capture_frame(&read);
    EffectBodyCurrentRead current={0};
    if(status==XR_XIR_OK) {
        read.fixed_only=false;
        status=effect_capture_complete_construct(&read,&current);
    }
    if(status==XR_XIR_OK) status=effect_capture_current_match(work,final,current.material?&current:NULL);
    current=(EffectBodyCurrentRead){0};
    if(status!=XR_XIR_OK) { xr_compile_resources_free(final->capture.facts);final->capture=(EffectCaptureProof){0}; }
    return status;
}
