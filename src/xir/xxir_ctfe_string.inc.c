/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_string.inc.c - Compiler-owned String receiving and byte-sized export
 *
 * KEY CONCEPT:
 *   Entry arguments become compiler-owned snapshots; internal calls retain
 *   authentic owned slots. Logical exports contain UTF-8 bytes, never pointers.
 */
static XrXirCtfeStatus ctfe_argument_store(ScalarRun *run,uint32_t offset,
    const XrXirValue *value,CtfeOwnedState *state,bool snapshot) {
    if (!xr_xir_type_is_owned(run->module->types,(XrXirType)value->type)) {
        xr_xir_scalar_store(run->frame,offset,value->payload);return XR_XIR_CTFE_OK;
    }
    XrXirCtfeStatus validation=ctfe_value_type_admit(run,state,(XrXirType)value->type);
    if (validation!=XR_XIR_CTFE_OK) return validation;
    if (xr_xir_type_is_cell(run->module->types,(XrXirType)value->type)) {
        if (xr_xir_cell_is_projection(value)) {
            if (!xr_xir_cell_in_domain(value,state->domain) || xr_xir_cell_module_storage(value)) return XR_XIR_CTFE_NOT_SUPPORTED;
            /* A new descendant descriptor is not readable until its real loan
             * enters. Preparation authenticates it before any child executes. */
        } else {
            XrXirCtfeStatus access=ctfe_value_graph_admit(run,state,value);if (access!=XR_XIR_CTFE_OK) return access;
        }
    }
    if (!snapshot) return ctfe_value_status(xr_xir_owned_slot_copy(run->frame,offset,
        state->arena,(XrXirType)value->type,value->payload));
    if (value->type!=XR_XIR_STRING) return XR_XIR_CTFE_NOT_SUPPORTED;
    const char *bytes=NULL;size_t length=0;XrXirValue copy={0};
    if (!xr_xir_string_view(value,&bytes,&length)) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus status=ctfe_value_status(xr_xir_string_new(state->domain,bytes,length,&copy));
    if (status==XR_XIR_CTFE_OK) xr_xir_owned_slot_move(run->frame,offset,&copy);
    xr_xir_value_drop(&copy);return status;
}
static XrXirCtfeStatus ctfe_result_publish(ScalarRun *run,const XrXirValue *value,
    XrXirType type,uint64_t maximum,CtfeOwnedState *state,XrXirValue *output) {
    XrXirCtfeStatus validation=ctfe_control_poll(state->control);
    if (validation!=XR_XIR_CTFE_OK) return validation;
    validation=ctfe_value_type_admit(run,state,type);
    if (validation!=XR_XIR_CTFE_OK) return validation;
    if (!ctfe_result_value(value,type,state->arena)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (type==XR_XIR_STRING) {
        const char *bytes=NULL;size_t length=0;
        if (!xr_xir_string_view(value,&bytes,&length)) return XR_XIR_CTFE_BAD_ARTIFACT;
        if ((uint64_t)length>maximum) return XR_XIR_CTFE_OUTPUT_LIMIT;
        return ctfe_value_status(xr_xir_value_copy(value,output));
    }
    if (ctfe_composite_node(run->module->types,type)) {
        XrXirCtfeStatus status=ctfe_tuple_export_bytes(run,value,state,maximum);
        if (status!=XR_XIR_CTFE_OK) return status;
        return ctfe_value_status(xr_xir_value_copy(value,output));
    }
    *output=*value;return XR_XIR_CTFE_OK;
}
#include "xxir_ctfe_string_read.inc.c"
#include "xxir_ctfe_text.inc.c"
#include "xxir_ctfe_string_owned.inc.c"

static bool ctfe_string_opcode(const ScalarRun *run,const XrXirInstruction *op) {
    return ctfe_string_read_opcode(op->op) || op->op==XR_XIR_STRING_COPY_BYTES || op->op==XR_XIR_STRING_REPLACE || op->op==XR_XIR_TO_STRING || op->op==XR_XIR_CONST_STRING || op->op==XR_XIR_CONCAT_STRING ||
        op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN ||
        op->op==XR_XIR_STRING_LEN || op->op==XR_XIR_EQ_STRING || op->op==XR_XIR_NE_STRING ||
        (op->op==XR_XIR_EQUAL && xr_xir_operand_type(run->function,op->args[0])==XR_XIR_STRING);
}
static XrXirCtfeStatus ctfe_string_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    const XrNativeIntrinsicDeclaration *call=op->op==XR_XIR_CALL_INTRINSIC?xr_xir_intrinsic(op->immediate):NULL;
    if(op->op==XR_XIR_STRING_COPY_BYTES || op->op==XR_XIR_STRING_REPLACE ||
        (call && call->implementation==XR_NATIVE_INTRINSIC_IMPLEMENTATION_STRING_SLICE))
        return ctfe_string_owned_step(run,cursor,state);
    if (op->op==XR_XIR_TO_STRING) return ctfe_text_step(run,cursor,state);
    if (ctfe_string_read_opcode(op->op)) return ctfe_string_read_step(run,cursor);
    uint32_t destination=run->layout->offsets[run->function->parameter_count+*cursor];
    XrXirValue result={0};XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (op->op==XR_XIR_CONST_STRING) {
        const XrXirLiteral *literal=&run->module->declarations->literals[op->immediate];
        status=xr_xir_string_new(state->domain,literal->bytes,literal->length,&result);
    } else if (op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ || op->op==XR_XIR_OWNED_RETAIN) {
        XrXirCtfeStatus validation=ctfe_value_type_admit(run,state,op->type);
        if (validation!=XR_XIR_CTFE_OK) return validation;
        status=xr_xir_owned_slot_copy(run->frame,destination,state->arena,op->type,
            xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]]));
    } else {
        XrXirValue left={XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        if (op->op==XR_XIR_STRING_LEN) {
            int64_t length=0;status=xr_xir_string_length(&left,&length);
            if (status==XR_XIR_VALUE_OK) xr_xir_scalar_store(run->frame,destination,length);
        } else {
            XrXirValue right={XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[1]])};
            const char *a=NULL,*b=NULL;size_t ac=0,bc=0;
            if (!xr_xir_string_view(&left,&a,&ac) || !xr_xir_string_view(&right,&b,&bc)) return XR_XIR_CTFE_BAD_ARTIFACT;
            if (op->op==XR_XIR_CONCAT_STRING) {
                if (bc>UINT64_MAX-ac) return XR_XIR_CTFE_OVERFLOW;
                XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain};
                status=xr_xir_string_slot_concat(run->frame,destination,&admission,left.payload,right.payload);
            } else {
                bool equal=false;
                if (!xr_xir_string_equal(&left,&right,&equal)) return XR_XIR_CTFE_BAD_ARTIFACT;
                bool negate=op->op==XR_XIR_NE_STRING || (op->op==XR_XIR_EQUAL && op->immediate);
                xr_xir_scalar_store(run->frame,destination,negate?!equal:equal);
            }
        }
    }
    if (status==XR_XIR_VALUE_OK && result.type) xr_xir_owned_slot_move(run->frame,destination,&result);
    xr_xir_value_drop(&result);
    if (status==XR_XIR_VALUE_OK) ++*cursor;
    return ctfe_value_status(status);
}
