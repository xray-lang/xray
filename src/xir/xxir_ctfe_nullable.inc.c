/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_nullable.inc.c - Compiler-owned active payload operations
 *
 * KEY CONCEPT:
 *   Existing Value operations own each typed sum. Complete family admission
 *   preserves active payload types and refuses unsupported authority.
 */
#include "xxir_equal.h"
#include "../shared/xr_string_parse_core.h"
static bool ctfe_nullable_opcode(const XrXirInstruction *op) {
    return op->op==XR_XIR_I64_TRY_PARSE || op->op==XR_XIR_F64_TRY_PARSE ||
        op->op==XR_XIR_BOOL_TRY_PARSE ||
        op->op==XR_XIR_NULLABLE_NONE || op->op==XR_XIR_NULLABLE_SOME ||
        op->op==XR_XIR_NULLABLE_IS_SOME || op->op==XR_XIR_NULLABLE_UNWRAP;
}
static XrXirCtfeStatus ctfe_nullable_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];XrXirValue result={0};
    bool parse=op->op==XR_XIR_I64_TRY_PARSE || op->op==XR_XIR_F64_TRY_PARSE ||
        op->op==XR_XIR_BOOL_TRY_PARSE;
    bool create=parse || op->op==XR_XIR_NULLABLE_NONE || op->op==XR_XIR_NULLABLE_SOME;
    XrXirType type=create?op->type:xr_xir_operand_type(run->function,op->args[0]);
    XrXirCtfeStatus status=ctfe_value_type_admit(run,state,type);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);
    XrXirValueStatus value_status=XR_XIR_VALUE_OK;
    if (parse) {
        XrXirValue text={XR_XIR_STRING,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        if (op->op==XR_XIR_F64_TRY_PARSE) {
            value_status=xr_xir_f64_try_parse(&text,type,&admission,&result);
        } else if (op->op==XR_XIR_BOOL_TRY_PARSE) {
            const char *bytes=NULL; size_t length=0;
            if (!xr_xir_string_view(&text,&bytes,&length)) return XR_XIR_CTFE_BAD_ARTIFACT;
            XrStringParseBoolResult parsed=xr_string_parse_bool(bytes,length);
            XrXirValue payload={XR_XIR_BOOL,0,parsed.value?1:0};
            value_status=xr_xir_nullable_new(type,parsed.failure==XR_NUMBER_PARSE_OK?&payload:NULL,&admission,&result);
        } else {
            const char *bytes=NULL; size_t length=0;
            if (!xr_xir_string_view(&text,&bytes,&length)) return XR_XIR_CTFE_BAD_ARTIFACT;
            XrStringParseIntResult parsed=xr_string_parse_int64(bytes,length);
            XrXirValue payload={XR_XIR_I64,0,(uint64_t)parsed.value};
            value_status=xr_xir_nullable_new(type,parsed.failure==XR_NUMBER_PARSE_OK?&payload:NULL,&admission,&result);
        }
    } else if (create) {
        XrXirValue payload={0};
        if (op->op==XR_XIR_NULLABLE_SOME) {
            XrXirType element=xr_xir_nullable_element(run->module->types,type);
            if (element!=XR_XIR_UNIT) payload=(XrXirValue){(uint32_t)element,0,
                xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        }
        status=ctfe_value_graph_admit(run,state,&payload);if (status!=XR_XIR_CTFE_OK) return status;
        value_status=xr_xir_nullable_new(type,op->op==XR_XIR_NULLABLE_SOME?&payload:NULL,&admission,&result);
    } else {
        XrXirValue receiver={(uint32_t)type,0,xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])};
        {
            bool some=false;const XrXirValue *payload=NULL;
            if (!xr_xir_nullable_view(&receiver,&some,&payload)) return XR_XIR_CTFE_BAD_ARTIFACT;
            if (op->op==XR_XIR_NULLABLE_IS_SOME) result=(XrXirValue){XR_XIR_BOOL,0,op->immediate?!some:some};
            else {
                if (!some) return XR_XIR_CTFE_NULL_UNWRAP;
                if (op->type==XR_XIR_ERROR) {
                    status=ctfe_value_type_admit(run,state,op->type);if (status!=XR_XIR_CTFE_OK) return status;
                }
                value_status=xr_xir_value_copy(payload,&result);
            }
        }
    }
    status=ctfe_value_status(value_status);
    if (status==XR_XIR_CTFE_OK && op->type!=XR_XIR_UNIT) {
        uint32_t offset=run->layout->offsets[run->function->parameter_count+*cursor];
        if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,offset,&result);
        else xr_xir_scalar_store(run->frame,offset,result.payload);
    }
    xr_xir_value_drop(&result);if (status==XR_XIR_CTFE_OK) ++*cursor;return status;
}
