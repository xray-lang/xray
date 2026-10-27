/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_affine.c - Borrowed owner availability and affine SSA transfers
 *
 * KEY CONCEPT:
 *   Explicit END kills Shared and Mut owners; affine transfers kill sources.
 *   Copyable Shared values keep their independent owners. This grants no
 *   root lifetime, reborrow permission or executable move implementation.
 */
#include "xxir_view_types_internal.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "xxir_compile_memory.h"
#include "../base/xchecks.h"

typedef struct AffineRule { uint8_t operands,edges; } AffineRule;
static const AffineRule affine_rules[XR_XIR_OP_COUNT]={
    {0,0},
#define XR_XIR_OP(name,stages,result,operands,edges,terminal) {operands,edges},
#include "xxir_ops.def"
#undef XR_XIR_OP
};
typedef struct AffineCheck {
    const XrXirCompileContext *context;
    const XrXirModule *module;
    const XrXirFunction *function;
    const uint8_t *types;
    XrXirDiagnostic location;
    uint32_t *indices,*queue;
    uint8_t *reachable;
    uint64_t *storage,*inputs,*current,*edge,*borrowed,*initial;
    uint32_t count,words;
} AffineCheck;

static uint32_t affine_flags(const AffineCheck *a,XrXirType type) {
    return xr_xir_type_node(a->module->types,type) ?
        a->types[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE]&3u : 0;
}
static bool affine_owner(const AffineCheck *a,uint32_t value) {
    if (value>=a->function->parameter_count) {
        XrXirOp op=a->function->instructions[value-a->function->parameter_count].op;
        if (op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT || op==XR_XIR_INVOKE_DEFAULT) return false;
    }
    return affine_flags(a,xr_xir_operand_type(a->function,value))!=0;
}
static bool affine_mutable(const AffineCheck *a,uint32_t value) {
    return (affine_flags(a,xr_xir_operand_type(a->function,value))&XR_XIR_VIEW_MUTABLE)!=0;
}
static uint32_t affine_index(const AffineCheck *a,uint32_t value) { return a->indices[value]; }
static bool affine_has(const uint64_t *bits,uint32_t id) {
    return (bits[id/64]&(UINT64_C(1)<<(id%64)))!=0;
}
static void affine_set(uint64_t *bits,uint32_t id,bool available) {
    uint64_t mask=UINT64_C(1)<<(id%64);
    if (available) bits[id/64]|=mask;else bits[id/64]&=~mask;
}
static XrXirStatus affine_copy(AffineCheck *a,uint64_t *to,const uint64_t *from) {

    memcpy(to,from,(size_t)a->words*sizeof(*to));return XR_XIR_OK;
}
static void affine_free(AffineCheck *a) {
    xr_compile_resources_free(a->indices);xr_compile_resources_free(a->queue);
    xr_compile_resources_free(a->reachable);xr_compile_resources_free(a->storage);
}
static XrXirStatus affine_allocate(AffineCheck *a) {
    const XrXirFunction *f=a->function;uint64_t values=(uint64_t)f->parameter_count+f->instruction_count;
    if (values>UINT32_MAX || values>SIZE_MAX/sizeof(uint32_t)) return XR_XIR_BUDGET;
    for (uint32_t v=0;v<(uint32_t)values;++v) {

        a->count+=affine_owner(a,v);
    }
    if (!a->count) return XR_XIR_OK;
    a->words=(uint32_t)(((uint64_t)a->count+63)/64);
    uint64_t cells=((uint64_t)f->block_count+4)*a->words;
    uint64_t bytes=values*sizeof(uint32_t)+(uint64_t)f->block_count*5;
    if (cells>SIZE_MAX/sizeof(uint64_t) || bytes>a->context->limits.frame_bytes ||
        cells>(a->context->limits.frame_bytes-bytes)/sizeof(uint64_t)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    a->indices=xir_compile_alloc(a->context,(size_t)values*sizeof(*a->indices),&status);
    a->queue=xir_compile_calloc(a->context,f->block_count,sizeof(*a->queue),&status);
    a->reachable=xir_compile_calloc(a->context,f->block_count,sizeof(*a->reachable),&status);
    a->storage=xir_compile_calloc(a->context,(size_t)cells,sizeof(*a->storage),&status);
    if (status!=XR_XIR_OK) return status;
    uint32_t index=0;
    for (uint32_t v=0;v<(uint32_t)values;++v) {

        a->indices[v]=affine_owner(a,v) ? index++ : UINT32_MAX;
    }
    XR_CHECK(index==a->count,"borrowed owner inventory stays immutable during CFG checking");
    a->inputs=a->storage;a->current=a->inputs+(size_t)f->block_count*a->words;
    a->edge=a->current+a->words;a->borrowed=a->edge+a->words;a->initial=a->borrowed+a->words;
    for (uint32_t p=0;p<f->parameter_count;++p) {

        if (a->indices[p]!=UINT32_MAX) affine_set(a->initial,a->indices[p],true);
    }
    for (uint32_t b=0;b<f->block_count;++b) for (uint32_t w=0;w<a->words;++w) {

        a->inputs[(size_t)b*a->words+w]=b ? UINT64_MAX : a->initial[w];
    }
    return XR_XIR_OK;
}
static XrXirStatus affine_reachable(AffineCheck *a) {
    uint32_t head=0,tail=1;a->queue[0]=0;a->reachable[0]=1;
    while (head<tail) {
        uint32_t b=a->queue[head++];const XrXirBlock *block=&a->function->blocks[b];
        const XrXirInstruction *term=&a->function->instructions[block->first+block->count-1];
        for (uint32_t e=0;e<3;++e) {

            if (e<2 ? e>=affine_rules[term->op].edges : !block->panic) continue;
            uint32_t target=e<2 ? term->targets[e] : block->panic;
            if (!a->reachable[target]) {a->reachable[target]=1;a->queue[tail++]=target;}
        }
    }
    return XR_XIR_OK;
}
static bool affine_call(XrXirOp op) {
    return op==XR_XIR_CALL || op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT;
}
static uint32_t affine_operands(const AffineCheck *a,const XrXirInstruction *op) {
    if (op->op==XR_XIR_RETURN) return a->function->result!=XR_XIR_UNIT;
    if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
        const XrXirDeclarations *d=a->module->declarations;
        return d ? xr_xir_slot_payload_operands(d->slots,d->slot_count,op) : 1;
    }
    if (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE ||
        op->op==XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType cell=op->op==XR_XIR_CELL_NEW ? op->type : xr_xir_operand_type(a->function,op->args[0]);
        return xr_xir_cell_payload_operands(a->module->types,cell,op->op);
    }
    return xr_xir_op_uses_operand_table(op->op) ? op->args[1] : affine_rules[op->op].operands;
}
static XrXirStatus affine_use(AffineCheck *a,uint32_t value,bool consume,bool borrow,bool check) {

    uint32_t id=affine_index(a,value);if (id==UINT32_MAX) return XR_XIR_OK;
    if (check && (!affine_has(a->current,id) || (borrow && affine_has(a->borrowed,id))))
        return XR_XIR_BAD_VALUE;
    if (consume) affine_set(a->current,id,false);
    if (borrow) affine_set(a->borrowed,id,true);
    return XR_XIR_OK;
}
static XrXirStatus affine_instruction(AffineCheck *a,uint32_t i,bool check) {
    const XrXirFunction *f=a->function;const XrXirInstruction *op=&f->instructions[i];
    uint32_t result=affine_index(a,f->parameter_count+i);a->location.instruction=i;
    if (op->op==XR_XIR_VIEW_CLEANUP) {
        for (uint32_t p=0;p<op->args[1];++p) {

            uint32_t id=affine_index(a,f->operands[op->args[0]+p]);
            if (id==UINT32_MAX) return XR_XIR_BAD_TYPE;
            affine_set(a->current,id,false);
        }
        return XR_XIR_OK;
    }
    bool call=affine_call(op->op),aggregate=op->op==XR_XIR_NULLABLE_SOME || op->op==XR_XIR_TUPLE_NEW;
    bool consume=op->op==XR_XIR_RETURN || op->op==XR_XIR_VIEW_END;
    bool shared_copy=op->op==XR_XIR_COPY || op->op==XR_XIR_OWNED_RETAIN ||
        op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_UNWRAP;
    bool definition=op->op==XR_XIR_NULLABLE_NONE || op->op==XR_XIR_NULLABLE_SOME ||
        op->op==XR_XIR_TUPLE_NEW || op->op==XR_XIR_PHI || call || op->op==XR_XIR_INVOKE_RESULT ||
        op->op==XR_XIR_VIEW_NEW || op->op==XR_XIR_VIEW_REBORROW ||
        (shared_copy && !affine_mutable(a,f->parameter_count+i));
    if (check && result!=UINT32_MAX && !definition) return XR_XIR_BAD_VALUE;
    if (call) {

        memset(a->borrowed,0,(size_t)a->words*sizeof(*a->borrowed));
    }
    if (op->op!=XR_XIR_PHI) for (uint32_t p=0;p<affine_operands(a,op);++p) {
        uint32_t v=xr_xir_op_uses_operand_table(op->op) ? f->operands[op->args[0]+p] : op->args[p];
        bool mutable=affine_mutable(a,v),take=consume || (aggregate && mutable);
        bool view_read=op->op>=XR_XIR_VIEW_REBORROW && op->op<=XR_XIR_VIEW_COPY;
        bool copy=!mutable && (shared_copy || aggregate);
        if (check && affine_index(a,v)!=UINT32_MAX && !take && !call && !copy && !view_read && op->op!=XR_XIR_NULLABLE_IS_SOME)
            return XR_XIR_BAD_VALUE;
        XrXirStatus status=affine_use(a,v,take,call && mutable,check);if (status!=XR_XIR_OK) return status;
    }
    /* The callee of an indirect call is an owned callable, whose signature
     * never makes the callable payload itself an affine borrowed value. */
    if (result!=UINT32_MAX) affine_set(a->current,result,true);
    return XR_XIR_OK;
}
static XrXirStatus affine_transfer(AffineCheck *a,uint32_t b,bool check) {
    a->location.block=b;
    XrXirStatus status=affine_copy(a,a->current,a->inputs+(size_t)b*a->words);
    const XrXirBlock *block=&a->function->blocks[b];
    for (uint32_t i=block->first;i<block->first+block->count && status==XR_XIR_OK;++i) {

        status=affine_instruction(a,i,check);
    }
    return status;
}
/* Mut PHI inputs transfer simultaneously on their normal predecessor edge.
 * Shared PHIs copy an independent owner and preserve the source's availability. */
static XrXirStatus affine_phi_edge(AffineCheck *a,uint32_t from,uint32_t target,bool check) {
    const XrXirFunction *f=a->function;const XrXirBlock *block=&f->blocks[target];
    for (uint32_t i=block->first;i<block->first+block->count;++i) {

        const XrXirInstruction *op=&f->instructions[i];if (op->op!=XR_XIR_PHI) continue;
        for (uint32_t p=0;p<op->args[1];p+=2) {

            if (f->operands[op->args[0]+p]!=from) continue;
            uint32_t id=affine_index(a,f->operands[op->args[0]+p+1]);
            if (id!=UINT32_MAX) {
                if (check && !affine_has(a->edge,id)) {
                    a->location.block=target;a->location.instruction=i;return XR_XIR_BAD_VALUE;
                }
                if (affine_mutable(a,f->operands[op->args[0]+p+1])) affine_set(a->edge,id,false);
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus affine_edges(AffineCheck *a,uint32_t b,bool check,bool *changed) {
    const XrXirBlock *block=&a->function->blocks[b];
    const XrXirInstruction *term=&a->function->instructions[block->first+block->count-1];
    for (uint32_t e=0;e<3;++e) {

        if (e<2 ? e>=affine_rules[term->op].edges : !block->panic) continue;
        uint32_t target=e<2 ? term->targets[e] : block->panic;
        XrXirStatus status=affine_copy(a,a->edge,a->current);if (status!=XR_XIR_OK) return status;
        if (e<2) {
            status=affine_phi_edge(a,b,target,check);if (status!=XR_XIR_OK) return status;
        } else for (uint32_t w=0;w<a->words;++w) {

            /* A panic can happen at any prefix. Local definitions cannot
             * escape through it, and a prior transfer cannot be undone. */
            a->edge[w]&=a->inputs[(size_t)b*a->words+w];
        }
        if (check) continue;
        for (uint32_t w=0;w<a->words;++w) {

            uint64_t *input=&a->inputs[(size_t)target*a->words+w],meet=*input&a->edge[w];
            if (*input!=meet) {*input=meet;*changed=true;}
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus affine_function(AffineCheck *a) {
    XrXirStatus status=affine_allocate(a);if (status!=XR_XIR_OK || !a->count) return status;
    status=affine_reachable(a);if (status!=XR_XIR_OK) return status;
    bool changed=true;
    while (changed && status==XR_XIR_OK) {
        changed=false;
        for (uint32_t b=0;b<a->function->block_count && status==XR_XIR_OK;++b) {

            if (!a->reachable[b]) continue;
            status=affine_transfer(a,b,false);
            if (status==XR_XIR_OK) status=affine_edges(a,b,false,&changed);
        }
    }
    for (uint32_t b=0;b<a->function->block_count && status==XR_XIR_OK;++b) {

        status=affine_transfer(a,b,true);
        if (status==XR_XIR_OK && a->reachable[b]) status=affine_edges(a,b,true,&changed);
    }
    return status;
}
XR_FUNC XrXirStatus xir_compile_view_affine_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    uint8_t *types=NULL;XrXirStatus status=xir_compile_view_flags_new(context,module->types,&types);
    if (status!=XR_XIR_OK || !types) return status;
    for (uint32_t f=0;f<module->function_count && status==XR_XIR_OK;++f) {

        AffineCheck a={.context=context,.module=module,.function=&module->functions[f],.types=types,
            .location={XR_XIR_OK,f,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE}};
        status=affine_function(&a);
        if (diagnostic && status!=XR_XIR_OK) {*diagnostic=a.location;diagnostic->status=status;}
        affine_free(&a);
    }
    xr_compile_resources_free(types);return status;
}

#include "xxir_view_liveness.inc.c"
#include "xxir_view_normalize.inc.c"
#include "xxir_view_may_owned.inc.c"
