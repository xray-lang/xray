/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * A group is a real local producer, never an ordinary Task-shaped permission.
 * Backward CFG witnesses reject use after join and stale aliases across a new
 * execution of the producer; scratch is local to this complete verification. */
#include "xxir_receive_control.h"
static XrXirStatus task_scope_origin(const XrXirFunction *function,uint32_t value,
    XrCompileRequestControl *control,uint32_t *origin) {
    *origin=UINT32_MAX;uint32_t steps=0;
    while(value>=function->parameter_count) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        uint32_t i=value-function->parameter_count;
        if(i>=function->instruction_count)return XR_XIR_BAD_VALUE;
        const XrXirInstruction *op=function->instructions+i;
        if(op->op==XR_XIR_SCOPE_BEGIN){*origin=i;return XR_XIR_OK;}
        if(op->op!=XR_XIR_COPY&&op->op!=XR_XIR_OWNED_RETAIN)return XR_XIR_OK;
        if(op->args[0]>=value)return XR_XIR_BAD_STRUCTURE;
        value=op->args[0];
    }
    return XR_XIR_OK;
}
static XrXirStatus task_scope_shape(const XrXirFunction *function,const XrXirInstruction *op,
    const XrXirModule *module) {
    if(op->op!=XR_XIR_SCOPE_BEGIN&&op->op!=XR_XIR_SCOPED_GO&&op->op!=XR_XIR_SCOPE_JOIN)return XR_XIR_OK;
    uint32_t origin=UINT32_MAX,value=0;
    XrXirType type=op->type;
    if(op->op!=XR_XIR_SCOPE_BEGIN) {
        if(op->op==XR_XIR_SCOPED_GO) {
            if(!op->args[1]||op->args[0]>=function->operand_count)return XR_XIR_BAD_STRUCTURE;
            value=function->operands[op->args[0]];
        } else value=op->args[0];
        XrXirStatus status=task_scope_origin(function,value,NULL,&origin);
        if(status!=XR_XIR_OK)return status;
        if(origin==UINT32_MAX)return XR_XIR_BAD_TYPE;
        type=xr_xir_operand_type(function,value);
        if(type!=function->instructions[origin].type)return XR_XIR_BAD_TYPE;
    }
    const XrXirTypeNode *group=xr_xir_type_node(module->types,type);
    if(!group||group->kind!=XR_XIR_TYPE_TASK||group->element!=XR_XIR_UNIT)return XR_XIR_BAD_TYPE;
    /* Non-Never propagation lands with its real Source and exit consumers. */
    if(group->result!=XR_XIR_NEVER)return XR_XIR_UNSUPPORTED;
    if(op->op==XR_XIR_SCOPE_BEGIN&&op->immediate!=0&&op->immediate!=2)return XR_XIR_BAD_STRUCTURE;
    if(op->op==XR_XIR_SCOPE_JOIN&&op->immediate!=0&&op->immediate!=1)return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}
typedef struct TaskScopeScratch {uint32_t *queue;unsigned char *seen;uint32_t states;} TaskScopeScratch;
static XrXirStatus task_scope_between(const Graph *graph,const XrXirFunction *function,
    const uint32_t points[3],TaskScopeScratch *scratch,XrCompileRequestControl *control) {
    uint32_t root=points[0],stop=points[1],use=points[2],steps=0;
    for(uint32_t i=0;i<scratch->states;++i) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        scratch->seen[i]=0;
    }
    uint32_t front=0,count=1;scratch->queue[0]=scratch->states;bool reached=false;
    while(front<count) {
        XrXirStatus status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        uint32_t state=scratch->queue[front++],block=state==scratch->states?graph->owner[use]:state/2;
        const XrXirBlock *range=function->blocks+block;
        uint32_t end=state==scratch->states?use:state%2?range->first:range->first+range->count;
        bool found=false;
        for(uint32_t i=end;i>range->first;) {
            --i;status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
            if(i==stop){found=true;reached=true;break;}
            if(i==root)return XR_XIR_BAD_VALUE;
            const XrXirInstruction *op=function->instructions+i;
            if(op->op==XR_XIR_SCOPE_JOIN) {
                uint32_t joined=UINT32_MAX;status=task_scope_origin(function,op->args[0],control,&joined);
                if(status!=XR_XIR_OK)return status;
                if(joined==root)return XR_XIR_BAD_VALUE;
            }
        }
        if(found)continue;
        bool predecessor=false;
        for(uint32_t fault=0;fault<2;++fault)
            for(uint32_t edge=fault?graph->fault_head[block]:graph->head[block];edge!=UINT32_MAX;edge=graph->next[edge]) {
                status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
                uint32_t incoming=graph->predecessor[edge]*2+fault;predecessor=true;
                if(scratch->seen[incoming])continue;
                scratch->seen[incoming]=1;scratch->queue[count++]=incoming;
            }
        if(!predecessor)return XR_XIR_BAD_VALUE;
    }
    return reached?XR_XIR_OK:XR_XIR_BAD_VALUE;
}
static XrXirStatus task_scope_uses(const Graph *graph,const XrXirFunction *function,VerifyContext *context) {
    bool present=false;
    for(uint32_t i=0;i<function->instruction_count;++i)present|=function->instructions[i].op==XR_XIR_SCOPE_BEGIN;
    if(!present)return XR_XIR_OK;
    if(function->block_count>(UINT32_MAX-1)/2)return XR_XIR_OVERFLOW;
    uint32_t states=function->block_count*2;
    if((uint64_t)states+1>SIZE_MAX/(sizeof(uint32_t)+1))return XR_XIR_OVERFLOW;
    size_t bytes=((size_t)states+1)*(sizeof(uint32_t)+1);
    if(context->graph_capacity>context->remaining.limits.frame_bytes||
        bytes>context->remaining.limits.frame_bytes-context->graph_capacity)return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    void *memory=xir_compile_alloc(&context->remaining,bytes,&status);
    if(!memory)return status;
    TaskScopeScratch scratch={memory,(unsigned char *)((uint32_t *)memory+states+1),states};
    for(uint32_t i=0;status==XR_XIR_OK&&i<function->instruction_count;++i) {
        const XrXirInstruction *op=function->instructions+i;
        context->location.instruction=i;context->location.block=graph->owner[i];
        if(op->op==XR_XIR_SCOPE_BEGIN) {
            uint32_t frontier=function->blocks[graph->owner[i]].frontier;
            if(!frontier||frontier>function->instruction_count)return xr_compile_resources_free(memory),XR_XIR_BAD_STRUCTURE;
            const XrXirInstruction *registration=function->instructions+frontier-1;
            if(registration->op!=XR_XIR_CLEANUP_REGISTER||registration->args[1]||registration->immediate<0||
                (uint64_t)registration->immediate>=context->module->function_count||!context->module->declarations)
                return xr_compile_resources_free(memory),XR_XIR_BAD_STRUCTURE;
            uint32_t helper=(uint32_t)registration->immediate;
            const XrXirFunction *cleanup=context->module->functions+helper;
            const XrXirFunctionIdentity *identity=context->module->declarations->functions+helper;
            if(identity->cleanup_owner!=context->location.function+1||cleanup->parameter_count||
                cleanup->result!=XR_XIR_UNIT||cleanup->error_type!=XR_XIR_NEVER||cleanup->instruction_count!=1||
                cleanup->instructions[0].op!=XR_XIR_RETURN||cleanup->instructions[0].args[0])
                return xr_compile_resources_free(memory),XR_XIR_BAD_STRUCTURE;
            for(uint32_t p=0;p<i;++p) if(function->instructions[p].op==XR_XIR_SCOPE_BEGIN&&
                function->blocks[graph->owner[p]].frontier==frontier)
                return xr_compile_resources_free(memory),XR_XIR_BAD_STRUCTURE;
        }
        uint32_t count=operand_count(function,op,context->module);
        for(uint32_t a=0;status==XR_XIR_OK&&a<count;++a) {
            if(op->op==XR_XIR_PHI){++a;if(a>=count){status=XR_XIR_BAD_STRUCTURE;break;}}
            uint32_t value=xr_xir_op_uses_operand_table(op->op)?function->operands[op->args[0]+a]:op->args[a],root;
            status=task_scope_origin(function,value,NULL,&root);
            if(status!=XR_XIR_OK||root==UINT32_MAX)continue;
            if(a||!(op->op==XR_XIR_SCOPED_GO||op->op==XR_XIR_SCOPE_JOIN||op->op==XR_XIR_COPY||op->op==XR_XIR_OWNED_RETAIN)) {
                status=XR_XIR_BAD_TYPE;break;
            }
            uint32_t frontier=function->blocks[graph->owner[i]].frontier;
            uint32_t owner=function->blocks[graph->owner[root]].frontier;
            while(frontier!=owner&&frontier) {
                if(frontier>function->instruction_count||function->instructions[frontier-1].op!=XR_XIR_CLEANUP_REGISTER) {
                    status=XR_XIR_BAD_STRUCTURE;break;
                }
                uint32_t parent=function->blocks[graph->owner[frontier-1]].frontier;
                if(parent>=frontier){status=XR_XIR_BAD_STRUCTURE;break;}frontier=parent;
            }
            if(status!=XR_XIR_OK||frontier!=owner){status=XR_XIR_BAD_VALUE;break;}
            uint32_t use=i;
            for(;;) {
                uint32_t stop=value-function->parameter_count;
                status=task_scope_between(graph,function,(uint32_t[]){root,stop,use},&scratch,NULL);
                if(status!=XR_XIR_OK||stop==root)break;
                use=stop;value=function->instructions[stop].args[0];
            }
        }
    }
    xr_compile_resources_free(memory);return status;
}
