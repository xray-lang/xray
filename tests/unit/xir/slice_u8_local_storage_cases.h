/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * slice_u8_local_storage_cases.h - Real Source local storage probes
 */
/* Real Source probes for conservative mutable local borrowed storage. */
#ifndef SLICE_U8_LOCAL_STORAGE_CASES_H
#define SLICE_U8_LOCAL_STORAGE_CASES_H
#include "xir_effect_execution_owner.h"
#include "xir/xxir_view_types_internal.h"
#define SLICE_LOCAL_STORAGE_SOURCE \
    "fn local_storage_probes(flag:bool)->i64{var text=\"abc\";var localView:Slice<u8>=text.bytes();" \
    "var alias=localView;if(alias[0]!=97){return -41};var total=0;" \
    "for(var i=0;i<3;i++){total+=localView[i] as i64};if(total!=294){return -42};" \
    "if(flag){if(len(localView)!=3){return -43}}else{if(localView[2]!=99){return -44}};" \
    "return 1}\n"
/* Eligibility refusals below preserve currently unsupported borrowed rebind/capture semantics; they are not claims that these source programs are language-invalid. */
typedef struct LocalStorageRefusal { const char *source;XrXirStatus status;const char *message; } LocalStorageRefusal;
static const LocalStorageRefusal local_storage_refusals[]={
    {"export fn run()->i64{var s=\"a\";var v:Slice<u8>=s.bytes();v=s.bytes();return len(v)}",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"export fn run(flag:bool)->i64{var s=\"a\";var v:Slice<u8>=s.bytes();if(flag){v=s.bytes()};return len(v)}",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"export fn run()->i64{var s=\"a\";var v:Slice<u8>=s.bytes();for(var i=0;i<2;i++){v=s.bytes()};return len(v)}",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"export fn run()->i64{var s=\"a\";var v:Slice<u8>=s.bytes();const f=fn()->i64{return len(v)};return f()}",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"export fn run()->i64{var s=\"a\";var v:Slice<u8>=s.bytes();defer{const n=len(v)};return len(v)}",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"fn take(v:ref Slice<u8>){}\nexport fn run()->i64{var s=\"a\";var v:Slice<u8>=s.bytes();take(ref v);return len(v)}",XR_XIR_BAD_TYPE,"source declaration type structure is invalid"},
    {"export fn run()->i64{var s=\"a\";var v:Slice<u8>=s.bytes();if(false){v=s.bytes()};return len(v)}",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"export fn run()->i64{var s=\"a\";var v:Slice<u8>=s.bytes();{var v=0;v=1};return len(v)}",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"var text=\"a\"\nvar persistent:Slice<u8>=text.bytes()",XR_XIR_BAD_TYPE,"before view lifetime normalization"},
    {"export fn run()->Slice<u8>{var s=\"a\";var v:Slice<u8>=s.bytes();return v}",XR_XIR_BAD_TYPE,"before view lifetime normalization"}
};
static const char local_storage_owned_capture_text[]="export fn capture_probe()->i64{var owned=\"a\";const reader=fn()->i64{return len(owned)};owned=\"abcd\";if(reader()!=4){return 0};return 1}";
static const char local_storage_resource_text[]="export fn probe(text:string)->i64{var view:Slice<u8>=text.bytes();return len(view)}";
#endif
