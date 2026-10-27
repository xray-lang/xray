/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * slice_u8_scalar_slot_cases.h - Actual scalar publication with initializer byte views
 */
#ifndef SLICE_U8_SCALAR_SLOT_CASES_H
#define SLICE_U8_SCALAR_SLOT_CASES_H
#include "xir/xxir_construction.h"
#include "xir/xxir_view_prepare_internal.h"
#define SCALAR_SLOT_VIEW_LOOP "for(var i=0;i<1;i++){var text=\"abc\";var bytes:Slice<u8>=text.bytes();var size=len(bytes);size+=0}\nfn other_initializer(){}\n"
typedef struct ScalarSlotSourceCase { const char *source;XrXirType element; } ScalarSlotSourceCase;
static const ScalarSlotSourceCase scalar_slot_cases[]={
 {"var scalar:i64=7\n" SCALAR_SLOT_VIEW_LOOP "export fn storage_receiver()->i64{return scalar-6}",XR_XIR_I64},
 {"var scalar:bool=true\n" SCALAR_SLOT_VIEW_LOOP "export fn storage_receiver()->i64{if(scalar){return 1};return 0}",XR_XIR_BOOL},
 {"var scalar:rune='A'\n" SCALAR_SLOT_VIEW_LOOP "export fn storage_receiver()->i64{if(scalar==('A')){return 1};return 0}",XR_XIR_RUNE},
 {"var scalar:f64=1.25\n" SCALAR_SLOT_VIEW_LOOP "export fn storage_receiver()->i64{if(scalar==1.25){return 1};return 0}",XR_XIR_F64}
};
#endif
