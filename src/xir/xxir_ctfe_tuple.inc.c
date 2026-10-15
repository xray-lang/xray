/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_tuple.inc.c - Immutable compiler Tuples and logical byte export
 *
 * KEY CONCEPT:
 *   Complete ordered fields use real Value ownership. Type summaries bound
 *   nested validation spans; iterative export retains no host frame pointers.
 */
#include "xxir_tuple.h"
#include "xxir_nullable.h"
#include "xxir_array.h"
static XrXirCtfeStatus ctfe_validation_work(ScalarRun *run,CtfeOwnedState *state,XrXirType type) {
    if (xr_xir_type_is_cell(run->module->types,type)) type=xr_xir_cell_element(run->module->types,type);
    if (!ctfe_composite_node(run->module->types,type)) return XR_XIR_CTFE_OK;
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (!state->tuple_costs) {
        void *memory=NULL;
        XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_calloc(
            run->compile_resources,run->module->types->count,sizeof(XrXirCtfeTypeShape),&memory)));
        if (status!=XR_XIR_CTFE_OK) return status;
        state->tuple_costs=memory;
    }
    if (!state->tuple_costs[index].work) {
        XrXirCtfeStatus status=ctfe_type_info(state->context,run->module->types,type,&state->tuple_costs[index]);
        if (status!=XR_XIR_CTFE_OK) return status;
    }
    uint64_t work=state->tuple_costs[index].work;
    if (work>UINT64_MAX/16) return XR_XIR_CTFE_BUDGET;
    /* Expanded type shape includes every repeated immutable child occurrence,
     * covering Value's full nested receiving and immediate header scans. */
    return scalar_compile_work(run,work*16)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BUDGET;
}
static bool ctfe_tuple_opcode(XrXirOp op) { return op==XR_XIR_TUPLE_NEW || op==XR_XIR_TUPLE_FIELD; }
static XrXirCtfeStatus ctfe_tuple_flow_work(ScalarRun *run,const XrXirInstruction *op,CtfeOwnedState *state) {
    if (op->op==XR_XIR_OWNED_LOCAL_WRITE)
        return ctfe_validation_work(run,state,xr_xir_operand_type(run->function,op->args[0]));
    if (op->op!=XR_XIR_JUMP && op->op!=XR_XIR_BRANCH) return XR_XIR_CTFE_OK;
    uint32_t edge=op->op==XR_XIR_BRANCH && !xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])?1u:0u;
    uint32_t first=run->function->blocks[op->targets[edge]].first;
    for (uint32_t i=first;i<run->function->instruction_count;++i) {
        if (!scalar_compile_work(run,1)) return XR_XIR_CTFE_BUDGET;
        if (run->function->instructions[i].op!=XR_XIR_PHI) break;
        XrXirCtfeStatus status=ctfe_validation_work(run,state,run->function->instructions[i].type);
        if (status!=XR_XIR_CTFE_OK) return status;
    }
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_tuple_step(ScalarRun *run,VmState *state,CtfeOwnedState *owned) {
    const XrXirInstruction *op=&run->function->instructions[state->instruction];
    XrXirValue result={0};XrXirValueStatus value;
    XrXirType receiver_type=op->op==XR_XIR_TUPLE_NEW?op->type:xr_xir_operand_type(run->function,op->args[0]);
    XrXirCtfeStatus status=ctfe_validation_work(run,owned,receiver_type);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (op->op==XR_XIR_TUPLE_NEW) {
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(run->module->types,op->type);uint32_t payload=0;
        if (!tuple || !state->arguments || tuple->parameter_count>run->layout->outgoing_count) return XR_XIR_CTFE_BAD_ARTIFACT;
        if (!scalar_compile_work(run,(uint64_t)tuple->parameter_count*(2*sizeof(XrXirValue)+2))) return XR_XIR_CTFE_BUDGET;
        for (uint32_t i=0;i<tuple->parameter_count;++i) {
            if (tuple->parameters[i].type==XR_XIR_UNIT) state->arguments[i]=(XrXirValue){0};
            else {
                uint32_t id=run->function->operands[op->args[0]+payload++];
                state->arguments[i]=(XrXirValue){(uint32_t)xr_xir_operand_type(run->function,id),0,
                    xr_xir_scalar_load(run->frame,run->layout->offsets[id])};
            }
        }
        for (uint32_t i=0;i<tuple->parameter_count;++i) {
            status=ctfe_admission_work(run,owned,&state->arguments[i]);if (status!=XR_XIR_CTFE_OK) return status;
        }
        XrXirValueAdmission admission={.arena=owned->arena,.domain=owned->domain,
            .work=UINT64_MAX,.scratch_bytes=owned->frame_spare};
        value=xr_xir_tuple_new(op->type,state->arguments,tuple->parameter_count,&admission,&result);
    } else {
        XrXirValue receiver={(uint32_t)receiver_type,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        value=xr_xir_tuple_get(&receiver,(uint32_t)op->immediate,&result);
    }
    status=ctfe_value_status(value);
    if (status==XR_XIR_CTFE_OK && op->type!=XR_XIR_UNIT) {
        uint32_t destination=run->layout->offsets[run->function->parameter_count+state->instruction];
        if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,destination,&result);
        else xr_xir_scalar_store(run->frame,destination,result.payload);
    }
    xr_xir_value_drop(&result);if (status==XR_XIR_CTFE_OK) ++state->instruction;return status;
}
typedef struct CtfeTupleExportFrame { XrXirValue value;uint64_t next,count;bool owned; } CtfeTupleExportFrame;
static XrXirCtfeStatus ctfe_tuple_export_bytes(ScalarRun *run,const XrXirValue *value,
    CtfeOwnedState *state,uint64_t maximum) {
    CtfeTupleExportFrame *stack=NULL;uint32_t depth=0,capacity=0;uint64_t bytes=0;
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;XrXirValue current=*value;bool current_owned=false;
    for (;;) {
        if (!scalar_compile_work(run,sizeof(CtfeTupleExportFrame)+4)) { status=XR_XIR_CTFE_BUDGET;break; }
        const XrXirTypeNode *tuple=ctfe_composite_node(run->module->types,(XrXirType)current.type);
        if (tuple && tuple->kind==XR_XIR_TYPE_NULLABLE) {
            bool some=false;const XrXirValue *payload=NULL;
            if (!xr_xir_nullable_view(&current,&some,&payload)) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(state->arena,(XrXirType)current.type);
            if (!layout || !layout->tag_bytes) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            if (layout->tag_bytes>maximum-bytes) { status=XR_XIR_CTFE_OUTPUT_LIMIT;break; }
            bytes+=layout->tag_bytes;
            if (!some) {
                if (current_owned) xr_xir_value_drop(&current);
                current=(XrXirValue){0};current_owned=false;tuple=NULL;
            }
        }
        uint64_t count=tuple?ctfe_composite_fields(tuple):0;
        if (tuple && tuple->kind==XR_XIR_TYPE_ARRAY) {
            XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain,.work=UINT64_MAX};int64_t length=0;
            status=ctfe_value_status(xr_xir_array_len(&current,&admission,&length));
            if (status!=XR_XIR_CTFE_OK) break;count=(uint64_t)length;
        }
        if (tuple) {
            if (depth==capacity) {
                uint64_t next=capacity?(uint64_t)capacity*2:1;
                uint64_t old=(uint64_t)capacity*sizeof(*stack),size=next*sizeof(*stack);
                if (next>UINT32_MAX || size>SIZE_MAX || old>state->frame_spare || size>state->frame_spare-old) { status=XR_XIR_CTFE_FRAME_LIMIT;break; }
                void *memory=stack;
                status=ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_resize(run->compile_resources,&memory,(size_t)size)));
                if (status!=XR_XIR_CTFE_OK) break;
                stack=memory;capacity=(uint32_t)next;
            }
            stack[depth++]=(CtfeTupleExportFrame){current,0,count,current_owned};current=(XrXirValue){0};current_owned=false;
        } else {
            uint64_t size=0;
            if (current.type==XR_XIR_STRING) {
                const char *text=NULL;size_t length=0;
                if (!xr_xir_string_view(&current,&text,&length)) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
                size=length;
            } else {
                XrXirLayout layout={0};const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
                status=ctfe_compile_status(xr_xir_compile_layout(state->context,run->module->types,
                    (XrXirType)current.type,&target,XR_XIR_LAYOUT_STORAGE,&layout));
                if (status!=XR_XIR_CTFE_OK) break;
                size=layout.size;
            }
            if (size>maximum-bytes) { status=XR_XIR_CTFE_OUTPUT_LIMIT;break; }
            bytes+=size;if (current_owned) xr_xir_value_drop(&current);current=(XrXirValue){0};current_owned=false;
        }
        while (depth) {
            CtfeTupleExportFrame *frame=&stack[depth-1];
            tuple=ctfe_composite_node(run->module->types,(XrXirType)frame->value.type);
            if (frame->next<frame->count) break;
            if (frame->owned) xr_xir_value_drop(&frame->value);--depth;
        }
        if (!depth) break;
        CtfeTupleExportFrame *frame=&stack[depth-1];
        status=ctfe_validation_work(run,state,(XrXirType)frame->value.type);
        if (status!=XR_XIR_CTFE_OK) break;
        tuple=ctfe_composite_node(run->module->types,(XrXirType)frame->value.type);
        if (tuple->kind==XR_XIR_TYPE_ARRAY) {
            status=ctfe_admission_work(run,state,&frame->value);if (status!=XR_XIR_CTFE_OK) break;
            XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain,.work=UINT64_MAX,.scratch_bytes=state->frame_spare-(uint64_t)capacity*sizeof(*stack)};XrXirFaultDetail fault={0};
            status=ctfe_value_status(xr_xir_array_get(&frame->value,(int64_t)frame->next++,&admission,&current,&fault));
        } else if (tuple->kind==XR_XIR_TYPE_NULLABLE) {
            bool some=false;const XrXirValue *payload=NULL;
            if (!xr_xir_nullable_view(&frame->value,&some,&payload) || !some || !payload) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            status=ctfe_value_status(xr_xir_value_copy(payload,&current));++frame->next;
        } else status=ctfe_value_status(xr_xir_tuple_get(&frame->value,(uint32_t)frame->next++,&current));
        if (status!=XR_XIR_CTFE_OK) break;
        current_owned=true;
    }
    if (current_owned) xr_xir_value_drop(&current);
    while (depth) { if (stack[depth-1].owned) xr_xir_value_drop(&stack[depth-1].value);--depth; }
    xr_compile_resources_free(stack);return status;
}
