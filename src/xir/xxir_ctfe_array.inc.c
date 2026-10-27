/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_array.inc.c - Compiler Array operations on authentic private roots
 *
 * KEY CONCEPT:
 *   Actual graph admission validates owners, forbidden roots and active
 *   ancestors. Private Cell places borrow canonical payloads synchronously.
 */
#include "xxir_array.h"
#include "xxir_struct.h"
typedef struct CtfeAdmissionFrame { XrXirValue value;uint32_t next,count;bool owned;const XrXirFunctionBinding *function; } CtfeAdmissionFrame;
static XrXirCtfeStatus ctfe_array_elements_admit(ScalarRun *run,CtfeOwnedState *state,
    XrXirType element) {
    CtfeTypeInfo info={0};
    return ctfe_type_info(state->context,run->module->types,element,true,&info);
}
static XrXirCtfeStatus ctfe_array_member_read(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *receiver,uint32_t index,XrXirValue *output) {
    const XrXirTypeNode *node=xr_xir_type_node(run->module->types,(XrXirType)receiver->type);
    if (!node || node->kind!=XR_XIR_TYPE_ARRAY) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirCtfeStatus status=ctfe_value_type_admit(run,state,(XrXirType)receiver->type);
    if (status==XR_XIR_CTFE_OK) status=ctfe_value_type_admit(run,state,node->element);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (ctfe_frame_spare(state)<256) return XR_XIR_CTFE_FRAME_LIMIT;
    XrXirValueAdmission admission=ctfe_value_admission(state);admission.scratch_bytes-=256;
    XrXirFaultDetail fault={0};
    return ctfe_value_status(xr_xir_array_observe(receiver,index,&admission,output,&fault));
}
static XrXirCtfeStatus ctfe_struct_member_read(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *receiver,uint32_t field,XrXirValue *output) {
    const XrXirTypeNode *node=xr_xir_type_node(run->module->types,(XrXirType)receiver->type);
    if (!node || !xr_xir_type_is_struct(run->module->types,(XrXirType)receiver->type) ||
        field>=node->nominal.field_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirCtfeStatus status=ctfe_value_type_admit(run,state,(XrXirType)receiver->type);
    if (status==XR_XIR_CTFE_OK) status=ctfe_value_type_admit(run,state,node->nominal.fields[field]);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);
    return ctfe_value_status(xr_xir_struct_observe(receiver,field,&admission,output));
}
static XrXirCtfeStatus ctfe_value_graph_visit(ScalarRun *run,CtfeOwnedState *state,const XrXirValue *input) {
    XrXirValue concrete={0};
    if (input->type==XR_XIR_ERROR) {
        XrXirCtfeStatus received=ctfe_error_concrete(run,input,&concrete);if (received!=XR_XIR_CTFE_OK) return received;input=&concrete;
    }
    XrXirCtfeStatus status=ctfe_value_type_admit(run,state,(XrXirType)input->type);
    bool cell=xr_xir_type_is_cell(run->module->types,(XrXirType)input->type);
    if (status!=XR_XIR_CTFE_OK || (!cell && !ctfe_execution_node(run->module->types,(XrXirType)input->type,true))) return status;
    if (!cell && !state->type_info[input->type-XR_XIR_CONSTRUCTED_TYPE_BASE].dynamic) return XR_XIR_CTFE_OK;
    CtfeAdmissionFrame *stack=NULL;uint32_t depth=0,capacity=0;XrXirValue current=*input;bool owned=false;uint32_t observations=0;
    for (;;) {
        if (!(observations++ & 63u)) {
            status=ctfe_control_poll(state->control);
            if (status!=XR_XIR_CTFE_OK) break;
        }
        if (current.type==XR_XIR_ERROR) {
            XrXirValue erased={0};status=ctfe_error_concrete(run,&current,&erased);if (status!=XR_XIR_CTFE_OK) break;current=erased;
        }
        status=ctfe_value_type_admit(run,state,(XrXirType)current.type);if (status!=XR_XIR_CTFE_OK) break;
        cell=xr_xir_type_is_cell(run->module->types,(XrXirType)current.type);
        const XrXirTypeNode *node=ctfe_execution_node(run->module->types,(XrXirType)current.type,true);
        bool descend=cell || (node && state->type_info[current.type-XR_XIR_CONSTRUCTED_TYPE_BASE].dynamic);
        bool identity=node && xr_xir_type_is_class(run->module->types,(XrXirType)current.type);
        uint32_t count=cell?1:node?ctfe_composite_fields(node):0;const XrXirFunctionBinding *function=NULL;
        if (cell) {
            if (!xr_xir_cell_in_domain(&current,state->domain) ||
                xr_xir_cell_is_projection(&current) || xr_xir_cell_module_storage(&current)) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
            if (state->forbidden_cell.type && xr_xir_cell_same_owner(&current,&state->forbidden_cell)) {
                status=XR_XIR_CTFE_NOT_SUPPORTED;break;
            }
        }
        if (descend && node && node->kind==XR_XIR_TYPE_CALLABLE) {
            status=ctfe_function_binding(state,&current,&function);if (status!=XR_XIR_CTFE_OK) break;
            count=function->capture_count;descend=count!=0;
        }
        if (identity) {
            if (!xr_xir_value_argument(&current,state->arena,(XrXirType)current.type)) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            if (xr_xir_class_module_storage(&current) ||
                (state->forbidden_class.type==current.type && state->forbidden_class.payload==current.payload)) {
                status=XR_XIR_CTFE_NOT_SUPPORTED;break;
            }
            descend=count!=0;
        }
        if (descend && node && node->kind==XR_XIR_TYPE_ARRAY) {
            XrXirValueAdmission admission=ctfe_value_admission(state);int64_t length=0;
            status=ctfe_value_status(xr_xir_array_len(&current,&admission,&length));
            if (status==XR_XIR_CTFE_OK) status=ctfe_array_elements_admit(run,state,node->element);
            if (status!=XR_XIR_CTFE_OK) break;
            CtfeTypeInfo shape={0};status=ctfe_type_info(state->context,run->module->types,node->element,true,&shape);
            if (status!=XR_XIR_CTFE_OK) break;
            if (shape.dynamic && (uint64_t)length>UINT32_MAX) { status=XR_XIR_CTFE_OVERFLOW;break; }
            descend=shape.dynamic && length!=0;count=descend?(uint32_t)length:0;
        }
        if (descend && node && node->kind==XR_XIR_TYPE_NULLABLE) {
            bool some=false;const XrXirValue *payload=NULL;
            if (!xr_xir_nullable_view(&current,&some,&payload)) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            if (!some) descend=false;
        }
        if (descend && node && xr_xir_type_is_enum(run->module->types,(XrXirType)current.type)) {
            XrXirEnumBorrow borrowed={0};status=ctfe_value_status(xr_xir_enum_borrow(&current,&borrowed));
            if (status!=XR_XIR_CTFE_OK) break;count=borrowed.field_count;if (!count) descend=false;
        }
        if (descend) {
            /* Active ancestors detect actual object cycles; shared siblings
             * remain legal and are inspected under their own real ownership. */
            for (uint32_t i=0;i<depth;++i) if (stack[i].value.type==current.type &&
                stack[i].value.payload==current.payload) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
            if (status!=XR_XIR_CTFE_OK) break;
            if (depth==capacity) {
                uint64_t next=capacity?(uint64_t)capacity*2:1,bytes=next*sizeof(*stack),old=(uint64_t)capacity*sizeof(*stack);
                if (next>UINT32_MAX || bytes>SIZE_MAX || old>ctfe_frame_spare(state) || bytes>ctfe_frame_spare(state)-old) { status=XR_XIR_CTFE_FRAME_LIMIT;break; }
                void *memory=stack;status=ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_resize(run->compile_resources,&memory,(size_t)bytes)));
                if (status!=XR_XIR_CTFE_OK) break;stack=memory;capacity=(uint32_t)next;
            }
            /* The borrowed binding lives in the retained immutable Function
             * object, never in the growing traversal vector. */
            stack[depth++]=(CtfeAdmissionFrame){current,0,count,owned,function};current=(XrXirValue){0};owned=false;
        } else { if (owned) xr_xir_value_drop(&current);current=(XrXirValue){0};owned=false; }
        for (;;) {
            if (!depth) goto done;
            CtfeAdmissionFrame *frame=&stack[depth-1];node=ctfe_execution_node(run->module->types,(XrXirType)frame->value.type,true);
            if (frame->next==frame->count) { if (frame->owned) xr_xir_value_drop(&frame->value);--depth;continue; }
            status=ctfe_value_type_admit(run,state,(XrXirType)frame->value.type);if (status!=XR_XIR_CTFE_OK) goto done;
            if (frame->function) {
                const XrXirValue *child=&frame->function->captures[frame->next];
                status=ctfe_value_type_admit(run,state,(XrXirType)child->type);if (status!=XR_XIR_CTFE_OK) goto done;
                status=ctfe_value_status(xr_xir_value_copy(child,&current));++frame->next;
            }
            else if (!node) { status=ctfe_value_status(xr_xir_cell_authorized_read(&frame->value,&state->authority,&current));++frame->next; }
            else if (xr_xir_type_is_class(run->module->types,(XrXirType)frame->value.type)) {
                CtfeOwnedState bounded=*state;bounded.frame_spare-=(uint64_t)capacity*sizeof(*stack);
                status=ctfe_class_member_read(run,&bounded,&frame->value,frame->next++,&current);state->type_info=bounded.type_info;
            }
            else if (node->kind==XR_XIR_TYPE_ARRAY) {
                CtfeOwnedState bounded=*state;bounded.frame_spare-=(uint64_t)capacity*sizeof(*stack);
                status=ctfe_array_member_read(run,&bounded,&frame->value,frame->next++,&current);state->type_info=bounded.type_info;
            }
            else if (node->kind==XR_XIR_TYPE_TUPLE) status=ctfe_value_status(xr_xir_tuple_get(&frame->value,frame->next++,&current));
            else if (xr_xir_type_is_struct(run->module->types,(XrXirType)frame->value.type)) {
                CtfeOwnedState bounded=*state;bounded.frame_spare-=(uint64_t)capacity*sizeof(*stack);
                status=ctfe_struct_member_read(run,&bounded,&frame->value,frame->next++,&current);state->type_info=bounded.type_info;
            }
            else if (node->kind==XR_XIR_TYPE_NOMINAL) {
                XrXirEnumBorrow borrowed={0};status=ctfe_value_status(xr_xir_enum_borrow(&frame->value,&borrowed));
                if (status==XR_XIR_CTFE_OK) status=ctfe_value_status(xr_xir_value_copy(&borrowed.fields[frame->next++],&current));
            } else {
                bool some=false;const XrXirValue *payload=NULL;
                if (!xr_xir_nullable_view(&frame->value,&some,&payload) || !some) { status=XR_XIR_CTFE_BAD_ARTIFACT;goto done; }
                status=ctfe_value_status(xr_xir_value_copy(payload,&current));++frame->next;
            }
            if (status!=XR_XIR_CTFE_OK) goto done;owned=true;break;
        }
    }
done:
    if (owned) xr_xir_value_drop(&current);
    while (depth) { if (stack[depth-1].owned) xr_xir_value_drop(&stack[depth-1].value);--depth; }
    xr_compile_resources_free(stack);return status;
}
static XrXirCtfeStatus ctfe_value_graph_admit(ScalarRun *run,CtfeOwnedState *state,const XrXirValue *value) {
    return ctfe_value_graph_visit(run,state,value);
}
static bool ctfe_array_opcode(XrXirOp op) {
    return op==XR_XIR_CELL_PROJECT || op==XR_XIR_RANGE_CHECK || op==XR_XIR_ARRAY_NEW || op==XR_XIR_ARRAY_REPEAT || op==XR_XIR_ARRAY_GET || op==XR_XIR_ARRAY_LEN ||
        op==XR_XIR_ARRAY_SET || op==XR_XIR_ARRAY_PUSH || op==XR_XIR_ARRAY_CAPACITY ||
        op==XR_XIR_ARRAY_WITH_CAPACITY || op==XR_XIR_ARRAY_RESERVE || op==XR_XIR_OBJECT_PLACE || op==XR_XIR_CELL_PLACE || op==XR_XIR_INDEX_PLACE || op==XR_XIR_FIELD_PLACE ||
        op==XR_XIR_PLACE_READ || op==XR_XIR_PLACE_WRITE;
}
static XrXirValue ctfe_array_operand(const ScalarRun *run,uint32_t id) {
    return (XrXirValue){(uint32_t)xr_xir_operand_type(run->function,id),0,xr_xir_scalar_load(run->frame,run->layout->offsets[id])};
}
static XrXirCtfeStatus ctfe_array_capacity_admit(ScalarRun *run,CtfeOwnedState *state,XrXirType type,uint64_t capacity) {
    XrXirLayout layout={0};
    if (!xr_xir_compile_type_arena_layout(state->arena,xr_xir_array_element(run->module->types,type),&layout)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (layout.size && capacity>UINT64_MAX/layout.size) return XR_XIR_CTFE_OVERFLOW;
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_array_incoming_admit(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *cell,const XrXirValue *incoming) {
    XrXirValue previous=state->forbidden_cell;
    if (cell->type) state->forbidden_cell=*cell;
    XrXirCtfeStatus status=ctfe_mutation_admit(run,state,cell,incoming);
    state->forbidden_cell=previous;return status;
}
static XrXirCtfeStatus ctfe_array_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    if (op->op==XR_XIR_CELL_PROJECT) return ctfe_projection_step(run,vm,state);
    if (op->op==XR_XIR_RANGE_CHECK) {
        const uint32_t *range=&run->function->operands[op->args[0]];
        if (!xr_xir_range_valid(ctfe_array_operand(run,range[0]).payload,
                ctfe_array_operand(run,range[1]).payload,ctfe_array_operand(run,range[2]).payload)) return XR_XIR_CTFE_NUMERIC_RANGE;
        ++vm->instruction;return XR_XIR_CTFE_OK;
    }
    if (op->op==XR_XIR_OBJECT_PLACE || op->op==XR_XIR_CELL_PLACE || op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_FIELD_PLACE) { ++vm->instruction;return XR_XIR_CTFE_OK; }
    XrXirValueAdmission admission=ctfe_value_admission(state);
    XrXirValue result={0},receiver={0},receiver_cell={0};XrXirValuePlace place={0};XrXirFaultDetail fault={0};
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;XrXirValueStatus value_status=XR_XIR_VALUE_OK;
    const uint32_t *ids=op->op==XR_XIR_ARRAY_SET?&run->function->operands[op->args[0]]:op->args;
    bool create=op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_REPEAT || op->op==XR_XIR_ARRAY_WITH_CAPACITY;
    if (!create && (xr_xir_place_kind(run->function,ids[0])==XR_XIR_PLACE_OBJECT || xr_xir_place_kind(run->function,ids[0])==XR_XIR_PLACE_INDEX || xr_xir_place_kind(run->function,ids[0])==XR_XIR_PLACE_FIELD))
        return ctfe_index_path_step(run,vm,state);
    if (!create) {
        if (xr_xir_place_kind(run->function,ids[0])==XR_XIR_PLACE_CELL) {
            const XrXirInstruction *root=&run->function->instructions[ids[0]-run->function->parameter_count];
            XrXirValue cell=ctfe_array_operand(run,root->args[0]);
            receiver_cell=cell;
            if (xr_xir_cell_is_projection(&cell)) return ctfe_index_path_step(run,vm,state);
            if (!xr_xir_cell_in_domain(&cell,state->domain)) return XR_XIR_CTFE_BAD_ARTIFACT;
            value_status=xr_xir_cell_authorized_place(&cell,&state->authority,&admission,&place);
            if (value_status!=XR_XIR_VALUE_OK) return ctfe_value_status(value_status);
            receiver.type=(uint32_t)place.type;memcpy(&receiver.payload,place.payload,sizeof(receiver.payload));
        } else receiver=ctfe_array_operand(run,ids[0]);
        status=ctfe_value_graph_admit(run,state,&receiver);if (status!=XR_XIR_CTFE_OK) return status;
    }
    if (create) { status=ctfe_value_type_admit(run,state,op->type);if (status!=XR_XIR_CTFE_OK) return status; }
    if (op->op==XR_XIR_ARRAY_NEW) {
        uint32_t count=op->args[1];if (count && (!vm->arguments || count>run->layout->outgoing_count)) return XR_XIR_CTFE_BAD_ARTIFACT;
        for (uint32_t i=0;i<count;++i) {
            vm->arguments[i]=ctfe_array_operand(run,run->function->operands[op->args[0]+i]);
            status=ctfe_value_graph_admit(run,state,&vm->arguments[i]);if (status!=XR_XIR_CTFE_OK) return status;
        }
        status=ctfe_array_elements_admit(run,state,xr_xir_array_element(run->module->types,op->type));
        if (status==XR_XIR_CTFE_OK) status=ctfe_array_capacity_admit(run,state,op->type,count<4 && count?4:(uint64_t)count*2);
        if (status!=XR_XIR_CTFE_OK) return status;
        value_status=xr_xir_array_new(op->type,vm->arguments,count,&admission,&result);
    } else if (op->op==XR_XIR_ARRAY_REPEAT || op->op==XR_XIR_ARRAY_WITH_CAPACITY) {
        int64_t count=ctfe_array_operand(run,op->args[0]).payload;if (count<0) return XR_XIR_CTFE_NUMERIC_RANGE;
        status=ctfe_array_capacity_admit(run,state,op->type,(uint64_t)count<4 && count?4:(uint64_t)count*2);
        if (status!=XR_XIR_CTFE_OK) return status;
        if (op->op==XR_XIR_ARRAY_WITH_CAPACITY) value_status=xr_xir_array_with_capacity(op->type,count,&admission,&result);
        else {
            XrXirValue fill=ctfe_array_operand(run,op->args[1]);status=ctfe_value_graph_admit(run,state,&fill);
            if (status==XR_XIR_CTFE_OK) status=ctfe_array_elements_admit(run,state,(XrXirType)fill.type);
            if (status!=XR_XIR_CTFE_OK) return status;value_status=xr_xir_array_repeat(op->type,count,&fill,&admission,&result);
        }
    } else if (op->op==XR_XIR_PLACE_READ) value_status=xr_xir_value_copy(&receiver,&result);
    else if (op->op==XR_XIR_PLACE_WRITE) {
        if (!place.payload) return XR_XIR_CTFE_NOT_SUPPORTED;
        XrXirValue value=ctfe_array_operand(run,ids[1]);status=ctfe_array_incoming_admit(run,state,&receiver_cell,&value);
        if (status!=XR_XIR_CTFE_OK) return status;admission=ctfe_value_admission(state);XrXirValuePath path={0};
        value_status=xr_xir_value_path_write(&place,&path,&value,&admission,&fault);
    } else if (op->op==XR_XIR_ARRAY_GET) value_status=xr_xir_array_get(&receiver,ctfe_array_operand(run,ids[1]).payload,&admission,&result,&fault);
    else if (op->op==XR_XIR_ARRAY_LEN || op->op==XR_XIR_ARRAY_CAPACITY) {
        int64_t number=0;value_status=op->op==XR_XIR_ARRAY_LEN?xr_xir_array_len(&receiver,&admission,&number):xr_xir_array_capacity(&receiver,&admission,&number);
        result=(XrXirValue){XR_XIR_I64,0,number};
    } else if (op->op==XR_XIR_ARRAY_RESERVE) {
        int64_t capacity=ctfe_array_operand(run,ids[1]).payload;if (capacity<0) return XR_XIR_CTFE_NUMERIC_RANGE;
        status=ctfe_array_capacity_admit(run,state,(XrXirType)receiver.type,(uint64_t)capacity);if (status!=XR_XIR_CTFE_OK) return status;
        value_status=xr_xir_array_reserve(&receiver,capacity,&admission,&result);
    } else {
        if (!place.payload) return XR_XIR_CTFE_NOT_SUPPORTED;
        XrXirValue element=ctfe_array_operand(run,ids[op->op==XR_XIR_ARRAY_SET?2:1]);
        status=ctfe_array_incoming_admit(run,state,&receiver_cell,&element);
        if (status!=XR_XIR_CTFE_OK) return status;admission=ctfe_value_admission(state);
        int64_t capacity=0;value_status=xr_xir_array_capacity(&receiver,&admission,&capacity);
        if (value_status!=XR_XIR_VALUE_OK) return ctfe_value_status(value_status);
        if ((uint64_t)capacity>(UINT64_MAX-4)/2) return XR_XIR_CTFE_OVERFLOW;
        status=ctfe_array_capacity_admit(run,state,(XrXirType)receiver.type,(uint64_t)capacity*2+4);if (status!=XR_XIR_CTFE_OK) return status;
        value_status=op->op==XR_XIR_ARRAY_SET?xr_xir_array_set(&place,ctfe_array_operand(run,ids[1]).payload,&element,&admission,&fault):xr_xir_array_push(&place,&element,&admission);
    }
    status=value_status==XR_XIR_VALUE_BOUNDS?ctfe_bounds_fault(state,fault):ctfe_value_status(value_status);
    if (status==XR_XIR_CTFE_OK && op->type!=XR_XIR_UNIT) {
        uint32_t offset=run->layout->offsets[run->function->parameter_count+vm->instruction];
        if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,offset,&result);
        else xr_xir_scalar_store(run->frame,offset,result.payload);
    }
    xr_xir_value_drop(&result);if (status==XR_XIR_CTFE_OK) ++vm->instruction;return status;
}
