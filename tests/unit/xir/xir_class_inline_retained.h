/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_inline_retained.h - Owned Array fields outlive all execution owners
 *
 * KEY CONCEPT:
 *   Data copies retain actual metadata without restoring execution authority.
 */
#ifndef XIR_CLASS_INLINE_RETAINED_H
#define XIR_CLASS_INLINE_RETAINED_H
#include "xir/xxir_struct.h"
static void class_inline_retained(XrXirValue *receiver) {
    XrXirDomain *domain=NULL;C(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(receiver),domain,NULL,NULL,10000,65536};
    XrXirValue record={0},number={0},text={0};
    C(xr_xir_class_get(receiver,0,&admission,&record)==XR_XIR_VALUE_OK);
    C(xr_xir_struct_get(&record,0,&admission,&number)==XR_XIR_VALUE_OK);
    C(number.type==XR_XIR_I64 && number.payload==40);xr_xir_value_drop(&number);
    C(xr_xir_struct_get(&record,1,&admission,&text)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(receiver);xr_xir_value_drop(&record);xr_xir_domain_drop(domain);
    const char *bytes=NULL;size_t length=0;
    C(xr_xir_string_view(&text,&bytes,&length)&&length==6&&!memcmp(bytes,"mapped",6));
    xr_xir_value_drop(&text);
}
#endif /* XIR_CLASS_INLINE_RETAINED_H */
