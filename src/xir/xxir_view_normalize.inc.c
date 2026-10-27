/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_normalize.inc.c - Explicit conditional owner cleanup over real CFGs
 */
#include "xxir_view_normalize_internal.h"
static const uint8_t view_normalize_terminals[XR_XIR_OP_COUNT]={
    0,
#define XR_XIR_OP(name,stages,result,operands,edges,terminal) terminal,
#include "xxir_ops.def"
#undef XR_XIR_OP
};
typedef struct ViewDropPlan { uint32_t count; uint32_t *values; } ViewDropPlan;
typedef struct ViewNormalizeFunction {
    ViewDropPlan **before,**after;
    uint32_t groups,operands;
} ViewNormalizeFunction;
typedef struct ViewNormalize {
    const XrXirModule *module;
    const XirViewNormalizationSource *source;
    ViewNormalizeFunction *plans;
    uint32_t marker;
} ViewNormalize;
static bool view_normalize_has(const XrXirViewLiveSet *set,uint32_t index) {
    return (set->bits[index/64u]&(UINT64_C(1)<<(index%64u)))!=0;
}
static XrXirStatus view_normalize_group(const XrXirCompileContext *context,
    const XrXirViewLiveEvent *event,ViewDropPlan **output,bool entry,uint32_t result) {
    uint32_t count=0;const XrXirViewLiveSet *before=&event->before,*after=&event->after;
    for (uint32_t i=0;i<before->count;++i) {

        bool dead=entry ? !view_normalize_has(before,i) :
            !view_normalize_has(after,i) && (view_normalize_has(before,i) || before->values[i]==result);
        count+=dead;
    }
    if (!count) return XR_XIR_OK;
    if (count>65536u) return XR_XIR_BUDGET;
    uint64_t bytes=sizeof(ViewDropPlan)+(uint64_t)count*sizeof(uint32_t);
    if (bytes>SIZE_MAX || bytes>context->limits.frame_bytes) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;ViewDropPlan *plan=xir_compile_alloc(context,(size_t)bytes,&status);
    if (!plan) return status;
    *output=plan;plan->count=count;plan->values=(uint32_t *)(plan+1);count=0;
    for (uint32_t i=0;i<before->count;++i) {

        bool dead=entry ? !view_normalize_has(before,i) :
            !view_normalize_has(after,i) && (view_normalize_has(before,i) || before->values[i]==result);
        if (dead) plan->values[count++]=before->values[i];
    }
    return XR_XIR_OK;
}
static XrXirStatus view_normalize_preserved(const ViewNormalize *n,uint32_t f,bool *preserved) {
    *preserved=false;
    if (!n->source) return XR_XIR_OK;
    if (n->source->generation!=n->module || !n->source->owner || !n->source->checked_copy)
        return XR_XIR_BAD_STRUCTURE;
    return n->source->checked_copy(n->source->owner,n->module,f,preserved);
}
static XrXirStatus view_normalize_visit(void *user,const XrXirCompileContext *context,
    const XrXirViewLiveEvent *event) {
    ViewNormalize *n=user;bool preserved=false;
    XrXirStatus copy_status=view_normalize_preserved(n,event->function,&preserved);
    if (copy_status!=XR_XIR_OK || preserved) return copy_status;
    const XrXirFunction *f=&n->module->functions[event->function];
    ViewNormalizeFunction *plan=n->plans+event->function;XrXirStatus status=XR_XIR_OK;
    if (event->kind==XR_XIR_VIEW_LIVE_EDGE) return XR_XIR_OK;
    if (!plan->before) {
        uint64_t bytes=(uint64_t)f->instruction_count*2u*sizeof(ViewDropPlan *);
        if (bytes>SIZE_MAX || bytes>context->limits.frame_bytes) return XR_XIR_BUDGET;
        plan->before=xir_compile_calloc(context,(size_t)f->instruction_count*2u,sizeof(*plan->before),&status);
        if (!plan->before) return status;plan->after=plan->before+f->instruction_count;
    }
    if (event->kind==XR_XIR_VIEW_LIVE_ENTRY) {
        const XrXirBlock *block=f->blocks+event->block;uint32_t at=block->first;
        while (at<block->first+block->count && f->instructions[at].op==XR_XIR_PHI) {
            ++at;
        }
        if (at<block->first+block->count) {
            XrXirOp op=f->instructions[at].op;
            if (op==XR_XIR_PANIC_CATCH || op==XR_XIR_ERROR_NARROW || op==XR_XIR_INVOKE_RESULT ||
                op==XR_XIR_INVOKE_ERROR || op==XR_XIR_INVOKE_DISCARD) ++at;
        }
        n->marker=at;return XR_XIR_OK;
    }
    uint32_t i=event->instruction;const XrXirInstruction *op=f->instructions+i;
    if (i==n->marker) status=view_normalize_group(context,event,plan->before+i,true,UINT32_MAX);
    bool selector=op->op==XR_XIR_PANIC_CATCH || op->op==XR_XIR_ERROR_NARROW || op->op==XR_XIR_INVOKE_RESULT ||
        op->op==XR_XIR_INVOKE_ERROR || op->op==XR_XIR_INVOKE_DISCARD;
    if (status==XR_XIR_OK && !view_normalize_terminals[op->op] && op->op!=XR_XIR_PHI && !selector)
        status=view_normalize_group(context,event,plan->after+i,false,f->parameter_count+i);
    return status;
}
static void view_normalize_plans_free(ViewNormalize *n) {
    if (!n->plans) return;
    for (uint32_t f=0;f<n->module->function_count;++f) {
        ViewNormalizeFunction *plan=n->plans+f;if (!plan->before) continue;
        for (uint32_t i=0;i<n->module->functions[f].instruction_count;++i) {
            xr_compile_resources_free(plan->before[i]);xr_compile_resources_free(plan->after[i]);
        }
        xr_compile_resources_free(plan->before);
    }
    xr_compile_resources_free(n->plans);
}
XR_FUNC void xir_compile_view_normalization_free(XirViewNormalization *n) {
    if (!n) return;
    if (n->functions) for (uint32_t f=0;f<n->module.function_count;++f) {
        XirViewNormalizedFunction *row=n->functions+f;
        xr_compile_resources_free(row->instructions);xr_compile_resources_free(row->blocks);
        xr_compile_resources_free(row->operands);xr_compile_resources_free(row->values);
        xr_compile_resources_free(row->boundaries);
    }
    xr_compile_resources_free(n->functions);xr_compile_resources_free(n->bodies);xr_compile_resources_free(n);
}
static bool view_relocate_value(const XirViewRelocation *r,uint32_t *value) {
    if (*value==UINT32_MAX) return true;
    if (*value>=(uint64_t)r->source->parameter_count+r->source->instruction_count) return false;
    *value=r->values[*value];
    return *value<(uint64_t)r->destination->parameter_count+r->destination->instruction_count;
}
static bool view_relocate_instruction(const XirViewRelocation *r,uint32_t *instruction,bool frontier) {
    if (frontier && !*instruction) return true;
    uint32_t index=*instruction-(frontier?1u:0u);
    if (index>=r->source->instruction_count) return false;
    *instruction=r->values[r->source->parameter_count+index]-r->source->parameter_count+(frontier?1u:0u);
    return true;
}
XR_FUNC XrXirStatus xir_compile_view_relocate(const XrXirCompileContext *context,const XirViewRelocation *r) {
    if (!xir_compile_context_valid(context) || !r || !r->source || !r->destination || !r->module ||
        !r->values || !r->boundaries) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *s=r->source;XrXirFunction *d=r->destination;
    if (r->first_instruction>s->instruction_count || r->first_block>s->block_count ||
        s->parameter_count!=d->parameter_count || s->block_count!=d->block_count ||
        !s->instructions || !s->blocks || !d->instructions || !d->blocks) return XR_XIR_BAD_STRUCTURE;
    uint32_t start=r->boundaries[r->first_instruction];ViewLive counts={.module=r->module,.function=s};
    for (uint32_t i=start;i<d->instruction_count;++i) {

        XrXirInstruction *op=(XrXirInstruction *)d->instructions+i;
        if (xr_xir_op_uses_operand_table(op->op)) {
            for (uint32_t p=0;p<op->args[1];++p) {

                if (op->op==XR_XIR_PHI && !(p%2u)) continue;
                if (!view_relocate_value(r,(uint32_t *)d->operands+op->args[0]+p)) return XR_XIR_BAD_STRUCTURE;
            }
        } else {
            uint32_t count=view_live_operands(&counts,op);
            for (uint32_t p=0;p<count;++p) {

                if (!view_relocate_value(r,op->args+p)) return XR_XIR_BAD_STRUCTURE;
            }
        }
        if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
            if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            uint32_t value=(uint32_t)op->immediate;if (!view_relocate_value(r,&value)) return XR_XIR_BAD_STRUCTURE;
            op->immediate=value;
        } else if (op->op==XR_XIR_INVOKE_RESULT || op->op==XR_XIR_INVOKE_ERROR || op->op==XR_XIR_INVOKE_DISCARD ||
            op->op==XR_XIR_CLEANUP_LEAVE || op->op==XR_XIR_CLEANUP_ERROR) {
            bool frontier=op->op==XR_XIR_CLEANUP_LEAVE || op->op==XR_XIR_CLEANUP_ERROR;
            if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            uint32_t value=(uint32_t)op->immediate;if (!view_relocate_instruction(r,&value,frontier)) return XR_XIR_BAD_STRUCTURE;
            op->immediate=value;
        }
    }
    for (uint32_t b=r->first_block;b<s->block_count;++b) {

        XrXirBlock *block=(XrXirBlock *)d->blocks+b;const XrXirBlock *old=s->blocks+b;
        block->first=r->boundaries[old->first];block->count=r->boundaries[old->first+old->count]-block->first;
        if (!view_relocate_instruction(r,&block->frontier,true)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_normalize_allocate(const XrXirCompileContext *context,
    const XrXirFunction *source,ViewNormalizeFunction *plan,XirViewNormalizedFunction *row) {
    if (!plan->before) return XR_XIR_OK;
    uint64_t groups=0,operands=source->operand_count;
    for (uint32_t i=0;i<source->instruction_count;++i) for (uint32_t side=0;side<2;++side) {

        ViewDropPlan *p=side?plan->after[i]:plan->before[i];if (!p) continue;
        ++groups;operands+=p->count;
    }
    if (!groups) return XR_XIR_OK;
    uint64_t instructions=source->instruction_count+groups,total=source->parameter_count+(uint64_t)source->instruction_count;
    uint64_t bytes=instructions*sizeof(XrXirInstruction)+(uint64_t)source->block_count*sizeof(XrXirBlock)+
        (operands+total+source->instruction_count+1u)*sizeof(uint32_t);
    if (instructions>context->limits.instructions || operands>UINT32_MAX ||
        instructions>UINT32_MAX-source->parameter_count || bytes>SIZE_MAX || bytes>context->limits.frame_bytes)
        return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;plan->groups=(uint32_t)groups;plan->operands=(uint32_t)operands;
    row->old_instructions=source->instruction_count;
    row->instructions=xir_compile_calloc(context,(size_t)instructions,sizeof(*row->instructions),&status);
    row->blocks=xir_compile_copy(context,source->blocks,(size_t)source->block_count*sizeof(*row->blocks),&status);
    row->operands=operands?xir_compile_alloc(context,(size_t)operands*sizeof(*row->operands),&status):NULL;
    row->values=xir_compile_alloc(context,(size_t)total*sizeof(*row->values),&status);
    row->boundaries=xir_compile_alloc(context,((size_t)source->instruction_count+1u)*sizeof(*row->boundaries),&status);
    return status;
}
static XrXirStatus view_normalize_drop(ViewDropPlan *plan,
    XirViewNormalizedFunction *row,uint32_t *instructions,uint32_t *operands) {
    if (!plan) return XR_XIR_OK;

    row->instructions[(*instructions)++]=(XrXirInstruction){.op=XR_XIR_VIEW_CLEANUP,.args={*operands,plan->count}};
    memcpy(row->operands+*operands,plan->values,(size_t)plan->count*sizeof(*plan->values));*operands+=plan->count;
    return XR_XIR_OK;
}
static XrXirStatus view_normalize_body(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t f,ViewNormalizeFunction *plan,XirViewNormalization *n) {
    const XrXirFunction *source=module->functions+f;XirViewNormalizedFunction *row=n->functions+f;
    XrXirStatus status=view_normalize_allocate(context,source,plan,row);
    if (status!=XR_XIR_OK || !row->instructions) return status;
    uint32_t count=0,operands=0;
    for (uint32_t p=0;p<source->parameter_count;++p) row->values[p]=p;
    for (uint32_t i=0;i<source->instruction_count;++i) {

        row->boundaries[i]=count;status=view_normalize_drop(plan->before[i],row,&count,&operands);
        if (status!=XR_XIR_OK) return status;
        row->values[source->parameter_count+i]=source->parameter_count+count;
        XrXirInstruction op=source->instructions[i];
        if (xr_xir_op_uses_operand_table(op.op)) {

            if (op.args[1]) memcpy(row->operands+operands,source->operands+op.args[0],(size_t)op.args[1]*sizeof(*row->operands));
            op.args[0]=op.args[1]?operands:0;operands+=op.args[1];
        }
        row->instructions[count++]=op;status=view_normalize_drop(plan->after[i],row,&count,&operands);
        if (status!=XR_XIR_OK) return status;
    }
    row->boundaries[source->instruction_count]=count;
    XR_CHECK(operands==plan->operands && count==source->instruction_count+plan->groups,"lifetime output matches its finite count");
    XrXirFunction *destination=n->bodies+f;destination->instructions=row->instructions;destination->instruction_count=count;
    destination->blocks=row->blocks;destination->operands=row->operands;destination->operand_count=operands;
    XirViewRelocation relocation={module,source,destination,row->values,row->boundaries,0,0};
    return xir_compile_view_relocate(context,&relocation);
}
static XrXirStatus view_normalize_verified(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewNormalizationSource *source,
    XirViewNormalization **output,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !output || *output ||
        !module->function_count || !module->functions) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;ViewNormalize builder={.module=module,.source=source};
    for (uint32_t f=0;f<module->function_count;++f) {
        bool preserved=false;status=view_normalize_preserved(&builder,f,&preserved);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {

            if (!preserved && module->functions[f].instructions[i].op==XR_XIR_VIEW_CLEANUP)
                return XR_XIR_BAD_STAGE;
        }
    }
    XirViewNormalization *n=xir_compile_calloc(context,1,sizeof(*n),&status);
    if (!n) return status;n->module=*module;
    builder.plans=xir_compile_calloc(context,module->function_count,sizeof(*builder.plans),&status);
    n->functions=xir_compile_calloc(context,module->function_count,sizeof(*n->functions),&status);
    n->bodies=xir_compile_copy(context,module->functions,(size_t)module->function_count*sizeof(*n->bodies),&status);
    if (status==XR_XIR_OK) status=xir_compile_view_liveness_verified(context,module,view_normalize_visit,&builder,diagnostic);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f) {
        bool preserved=false;status=view_normalize_preserved(&builder,f,&preserved);
        if (status==XR_XIR_OK && !preserved) status=view_normalize_body(context,module,f,builder.plans+f,n);
        if (diagnostic && status!=XR_XIR_OK) *diagnostic=(XrXirDiagnostic){status,f,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    }
    view_normalize_plans_free(&builder);
    if (status!=XR_XIR_OK) {xir_compile_view_normalization_free(n);return status;}
    n->module.functions=n->bodies;*output=n;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_view_normalize_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XirViewNormalization **output,XrXirDiagnostic *diagnostic) {
    return view_normalize_verified(context,module,NULL,output,diagnostic);
}
XR_FUNC XrXirStatus xir_compile_view_normalize_source_verified(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewNormalizationSource *source,
    XirViewNormalization **output,XrXirDiagnostic *diagnostic) {
    if (!source) return XR_XIR_BAD_STRUCTURE;
    return view_normalize_verified(context,module,source,output,diagnostic);
}
