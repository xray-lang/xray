/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_state_typed_class_observer.h - Real class construction and physical reclaim
 *
 * KEY CONCEPT:
 *   Successful Source construction registers its actual result; allocator free
 *   witnesses reclaim independently of Cell publication or slot release events.
 */
#ifndef XIR_LIBRARY_STATE_TYPED_CLASS_OBSERVER_H
#define XIR_LIBRARY_STATE_TYPED_CLASS_OBSERVER_H
#include "base/xmalloc.h"
#include "xir/xxir_class.h"
typedef struct LibraryStateTypedClassRecord {
    uintptr_t address;
    bool live;
} LibraryStateTypedClassRecord;
static LibraryStateTypedClassRecord library_state_typed_classes[2];
static uint32_t library_state_typed_class_count,library_state_typed_class_frees;
static bool library_state_typed_class_active;
static void library_state_typed_class_free(void *pointer) {
    if (pointer && library_state_typed_class_active) {
        for (uint32_t n=0;n<library_state_typed_class_count;++n) {
            LibraryStateTypedClassRecord *record=&library_state_typed_classes[n];
            if (record->live && record->address==(uintptr_t)pointer) {
                record->live=false;++library_state_typed_class_frees;
            }
        }
    }
    xr_free(pointer);
}
/* runtime_free retains its ordinary physical accounting and calls this actual
 * host free boundary. Only the canonical constructor body receives a new name. */
#undef xr_free
#define xr_free(pointer) library_state_typed_class_free(pointer)
#define xr_xir_class_new library_state_typed_class_real_new
#include "xir_runtime_allocations.h"
#undef xr_xir_class_new
XR_FUNC XrXirValueStatus xr_xir_class_new(XrXirType type,const XrXirValue *fields,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output) {
    XrXirValueStatus status=library_state_typed_class_real_new(type,fields,count,admission,output);
    if (status==XR_XIR_VALUE_OK && library_state_typed_class_active) {
        CHECK(count==2 && library_state_typed_class_count<2 && output->payload && xr_xir_value_valid(output));
        CHECK(fields[0].type==XR_XIR_I64 && fields[0].payload==0);
        const char *text=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&fields[1],&text,&length) && length==4 && !memcmp(text,"seed",4));
        for (uint32_t n=0;n<library_state_typed_class_count;++n)
            CHECK(!library_state_typed_classes[n].live ||
                library_state_typed_classes[n].address!=(uintptr_t)output->payload);
        library_state_typed_classes[library_state_typed_class_count++]=
            (LibraryStateTypedClassRecord){(uintptr_t)output->payload,true};
    }
    return status;
}
static void library_state_typed_class_begin(void) {
    CHECK(!library_state_typed_class_active);
    memset(library_state_typed_classes,0,sizeof(library_state_typed_classes));
    library_state_typed_class_count=library_state_typed_class_frees=0;
    library_state_typed_class_active=true;
}
static void library_state_typed_class_end(bool complete) {
    CHECK(library_state_typed_class_active);
    CHECK(library_state_typed_class_count==library_state_typed_class_frees);
    for (uint32_t n=0;n<library_state_typed_class_count;++n) CHECK(!library_state_typed_classes[n].live);
    if (complete) CHECK(library_state_typed_class_count==2);
    library_state_typed_class_active=false;
}
#endif // XIR_LIBRARY_STATE_TYPED_CLASS_OBSERVER_H
