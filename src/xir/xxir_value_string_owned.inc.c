/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_string_owned.inc.c - Owned String replacement and byte-array copies
 *
 * Input handles remain immutable. Search and copy work precede each phase;
 * allocation uses the ordinary Domain and typed Array storage, with no view.
 */
#include "xxir_string_owned_cost.h"
typedef struct XirOwnedStringInputs {
    const XirString *text,*search,*replacement;
    bool all;
} XirOwnedStringInputs;
typedef struct XirOwnedStringPlan {
    XrStringCoreReplacePlan bytes;
    size_t runes;
} XirOwnedStringPlan;
static bool owned_string_work(XrXirValueAdmission *admission,uint64_t units) {
    if(units>admission->work || !xr_xir_domain_work(admission->domain,units)) return false;
    admission->work-=units;return true;
}
static XrXirValueStatus owned_string_input(XrXirValueAdmission *admission,
    const XrXirValue *value,const XirString **output) {
    XrXirValueStatus status=xr_xir_value_admit(value,XR_XIR_STRING,admission);
    if(status!=XR_XIR_VALUE_OK) return status;
    const XirString *string=string_pointer(value);
    if(string->length>PTRDIFF_MAX || string->runes>INT64_MAX) return XR_XIR_VALUE_LIMIT;
    *output=string;return XR_XIR_VALUE_OK;
}
static bool owned_string_admission(XrXirValueAdmission *admission,const XrXirValue *output) {
    return admission && admission->domain && xr_xir_domain_current(admission->domain) && unit_value(output);
}
static XrXirValueStatus owned_string_plan_size(const XirOwnedStringInputs *input,XirOwnedStringPlan *plan) {
    if(plan->bytes.kind==XR_STRING_CORE_REPLACE_INVALID || plan->bytes.len>INT64_MAX || plan->bytes.len==SIZE_MAX)
        return XR_XIR_VALUE_LIMIT;
    if(plan->bytes.kind==XR_STRING_CORE_REPLACE_ORIGINAL) {plan->runes=input->text->runes;return XR_XIR_VALUE_OK;}
    if(!input->search->runes || plan->bytes.count>input->text->runes/input->search->runes)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t base=input->text->runes-plan->bytes.count*input->search->runes;
    if(input->replacement->runes && plan->bytes.count>(SIZE_MAX-base)/input->replacement->runes)
        return XR_XIR_VALUE_LIMIT;
    plan->runes=base+plan->bytes.count*input->replacement->runes;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus owned_string_plan(XrXirValueAdmission *admission,
    const XirOwnedStringInputs *input,XirOwnedStringPlan *plan) {
    size_t length=input->text->length,pattern=input->search->length;
    if(!admission_is_compile(admission)) {
        uint64_t calls=input->all && pattern && pattern<=length?(uint64_t)(length/pattern)+2:1,work=0;
        if(!xir_string_owned_search_work(length,pattern,calls,input->all?2:1,&work) ||
            !xir_string_owned_add_work(&work,pattern,1) || !owned_string_work(admission,work))
            return XR_XIR_VALUE_LIMIT;
    }
    plan->bytes=xr_string_core_replace_plan(input->text->bytes,length,input->search->bytes,pattern,
        input->replacement->bytes,input->replacement->length,input->all);
    return owned_string_plan_size(input,plan);
}
static XrXirValueStatus owned_string_fill(XrXirValueAdmission *admission,
    const XirOwnedStringInputs *input,const XirOwnedStringPlan *plan,XirString *output) {
    if(!admission_is_compile(admission)) {
        uint64_t work=0;
        if(!xir_string_owned_search_work(input->text->length,input->search->length,plan->bytes.count,1,&work) ||
            !xir_string_owned_add_work(&work,plan->bytes.len,1) || !owned_string_work(admission,work))
            return XR_XIR_VALUE_LIMIT;
    }
    size_t written=xr_string_core_replace_write(output->bytes,input->text->bytes,input->text->length,
        input->search->bytes,input->search->length,input->replacement->bytes,input->replacement->length,
        plan->bytes,input->all);
    if(written!=plan->bytes.len) return XR_XIR_VALUE_BAD_ARGUMENT;
    output->runes=plan->runes;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus owned_string_replace_graph(const XrXirValue *receiver,
    const XrXirValue *search,const XrXirValue *replacement,bool all,
    XrXirValueAdmission *admission,XrXirValue *output) {
    if(!owned_string_admission(admission,output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirOwnedStringInputs input={.all=all};XirOwnedStringPlan plan={0};
    XrXirValueStatus status=owned_string_input(admission,receiver,&input.text);
    if(status==XR_XIR_VALUE_OK) status=owned_string_input(admission,search,&input.search);
    if(status==XR_XIR_VALUE_OK) status=owned_string_input(admission,replacement,&input.replacement);
    if(status==XR_XIR_VALUE_OK) status=owned_string_plan(admission,&input,&plan);
    if(status!=XR_XIR_VALUE_OK) return status;
    if(plan.bytes.kind==XR_STRING_CORE_REPLACE_ORIGINAL) return xr_xir_value_copy(receiver,output);
    XirString *string=NULL;status=string_allocate(admission->domain,plan.bytes.len,&string);
    if(status!=XR_XIR_VALUE_OK) return status;
    XrXirValue result=string_value(string);
    status=owned_string_fill(admission,&input,&plan,string);
    if(status!=XR_XIR_VALUE_OK) {xr_xir_value_drop(&result);return status;}
    *output=result;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus owned_string_bytes_graph(const XrXirValue *receiver,XrXirType type,
    XrXirValueAdmission *admission,XrXirValue *output) {
    if(!owned_string_admission(admission,output) || !admission->arena) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirType element=XR_XIR_UNIT;uint32_t stride=0;
    if(!array_layout(admission->arena,type,&element,&stride) || element!=XR_XIR_U8 || stride!=1)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirString *text=NULL;XrXirValueStatus status=owned_string_input(admission,receiver,&text);
    if(status!=XR_XIR_VALUE_OK) return status;
    if((!admission_is_compile(admission) && !owned_string_work(admission,(uint64_t)text->length+1))) return XR_XIR_VALUE_LIMIT;
    size_t capacity=0;
    if(!array_capacity(text->length,stride,&capacity)) return XR_XIR_VALUE_LIMIT;
    XirArray *array=NULL;status=array_allocate(type,capacity,admission,&array);
    if(status!=XR_XIR_VALUE_OK) return status;
    if(text->length) memcpy(array->data,text->bytes,text->length);
    array->length=text->length;*output=array_value(array);return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_string_copy_bytes(const XrXirValue *receiver,
    XrXirType array_type,XrXirValueAdmission *admission,XrXirValue *output) {
    if(!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=owned_string_bytes_graph(receiver,array_type,admission,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_string_replace(const XrXirValue *receiver,
    const XrXirValue *search,const XrXirValue *replacement,bool all,
    XrXirValueAdmission *admission,XrXirValue *output) {
    if(!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=owned_string_replace_graph(receiver,search,replacement,all,admission,output);
    xr_xir_value_graph_end();return status;
}
