/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_function.h - Actual callback metadata survives nested admission
 */
#ifndef TYPE_RECEIVING_FUNCTION_H
#define TYPE_RECEIVING_FUNCTION_H
#include "xir/xxir_array.h"
typedef struct ReceivedFunctionContext {
    XrXirValueAdmission *admission;
    const XrXirTypeArena *birth;
    const XrXirValue *value;
    unsigned calls,depth,releases;
    bool reject;
} ReceivedFunctionContext;
static void received_function_release(void *owner) {
    ++((ReceivedFunctionContext *)owner)->releases;
}
static XrXirValueStatus received_function_admit(void *owner,const XrXirFunctionBinding *binding,
    XrXirType actual,uint64_t *work) {
    ReceivedFunctionContext *context=owner;
    XrXirValueAdmission *active=(XrXirValueAdmission *)
        ((unsigned char *)work - offsetof(XrXirValueAdmission,work));
    CHECK(context && binding->owner==context && active->context==context &&
        active->function==received_function_admit && active->domain==context->admission->domain);
    ++context->calls;
    if (active->arena!=context->birth || actual!=type(0) || context->reject)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (context->value && !context->depth) {
        ++context->depth;
        CHECK(xr_xir_value_admit(context->value,type(0),active)==XR_XIR_VALUE_OK);
        --context->depth;
        CHECK(active->arena==context->birth);
    }
    return XR_XIR_VALUE_OK;
}
static void received_function_storage(SourceFixtureOwner *compiler) {
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypeNode original[]={node,{.kind=XR_XIR_TYPE_ARRAY,.element=type(0)}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},node,
        {.kind=XR_XIR_TYPE_ARRAY,.element=type(1)}};
    XrXirTypes pools[]={{original,2,NULL,NULL},{shifted,3,NULL,NULL}};
    XrXirTypeArena *birth=NULL,*target=NULL,*unrelated=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&birth)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&unrelated)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={type(1),type(2)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,birth,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={birth,domain,received_function_admit,NULL,4000000,128*1024};
    ReceivedFunctionContext context={.admission=&admission,.birth=birth};admission.context=&context;
    XrXirFunctionBinding binding={&context,received_function_release,0,NULL,0};XrXirValue value={0},rejected={0},array={0},read={0};
    CHECK(xr_xir_function_new(domain,birth,type(0),&binding,&admission,&value)==XR_XIR_VALUE_OK && context.calls==1);
    context.value=&value;
    CHECK(xr_xir_array_new(type(1),&value,1,&admission,&array)==XR_XIR_VALUE_OK);
    admission.arena=target;context.value=&value;unsigned calls=context.calls;uint64_t work=admission.work;
    CHECK(xr_xir_value_admit(&value,type(1),&admission)==XR_XIR_VALUE_OK && context.calls==calls+2 &&
        admission.arena==target && admission.work<work && xr_xir_value_arena(&value)==birth);
    context.reject=true;calls=context.calls;work=admission.work;
    CHECK(xr_xir_value_admit(&value,type(1),&admission)==XR_XIR_VALUE_BAD_ARGUMENT && context.calls==calls+1 &&
        admission.arena==target && admission.work<work);
    context.reject=false;
    XrXirFaultDetail fault={0};calls=context.calls;
    CHECK(xr_xir_array_get(&array,0,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==value.payload &&
        read.type==value.type && context.calls==calls+2 && admission.arena==target);
    xr_xir_value_drop(&read);context.reject=true;calls=context.calls;
    CHECK(xr_xir_array_get(&array,0,&admission,&read,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type &&
        context.calls==calls+1 && admission.arena==target);context.reject=false;
    CHECK(xr_xir_function_new(domain,target,type(1),&binding,&admission,&rejected)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !rejected.type && !context.releases && admission.arena==target);
    calls=context.calls;admission.arena=unrelated;
    CHECK(xr_xir_value_admit(&value,type(1),&admission)==XR_XIR_VALUE_BAD_ARGUMENT && context.calls==calls &&
        admission.arena==unrelated);
    xr_xir_value_drop(&array);xr_xir_value_drop(&value);CHECK(context.releases==1);
    xr_xir_compile_type_arena_drop(unrelated);xr_xir_compile_type_arena_drop(target);
    xr_xir_compile_type_arena_drop(birth);xr_xir_domain_drop(domain);
}
#endif // TYPE_RECEIVING_FUNCTION_H
