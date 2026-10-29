/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_result_catalog_cases.h - Authenticated cold native result metadata
 */
#ifndef XIR_TASK_RESULT_CATALOG_CASES_H
#define XIR_TASK_RESULT_CATALOG_CASES_H
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
#include "xir/xxir_panic.h"
static void task_catalog_nested(const XrXirValue *field,unsigned variant) {
    if(variant==1) {
        XrXirValue concrete={0};CHECK(xr_xir_error_borrow(field,&concrete));
        XrXirEnumBorrow business={0};CHECK(xr_xir_enum_borrow(&concrete,&business)==XR_XIR_VALUE_OK);
        CHECK(business.name.length==9 && !memcmp(business.name.bytes,"ColdError",9));
        CHECK(business.member.length==3 && !memcmp(business.member.bytes,"Bad",3));
        CHECK(business.field_count==1 && business.fields[0].type==XR_XIR_I64 && business.fields[0].payload==9);
    } else {
        CHECK(variant==2);XrXirFaultDetail detail={0};CHECK(xr_xir_panic_info_detail(field,&detail));
        CHECK(detail.code==XR_XIR_PANIC_TRY_ASSERT && !detail.reserved && !detail.index && !detail.length);
        XrXirValue message={0};CHECK(xr_xir_panic_info_message(field,&message)==XR_XIR_VALUE_OK);
        const char expected[]="try! failed: ColdError.Bad(9)";const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&message,&bytes,&length) && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
        xr_xir_value_drop(&message);
    }
}
static uint32_t task_catalog_word(const XrXirCheckedPacket *packet,size_t offset) {
    CHECK(offset<=packet->length && packet->length-offset>=4);
    uint32_t value=0;
    for(unsigned b=0;b<4;++b)value|=(uint32_t)packet->bytes[offset+b]<<(b*8);
    return value;
}
static size_t task_catalog_name(const XrXirCheckedPacket *packet,size_t offset,const char *name) {
    uint32_t length=task_catalog_word(packet,offset);offset+=4;
    CHECK(length==strlen(name) && offset<=packet->length && length<=packet->length-offset);
    CHECK(!memcmp(packet->bytes+offset,name,length));return offset+length;
}
static void task_catalog_rejections(const XrXirCompileContext *context,
    XrXirCheckedPacket *packet,XrXirLibraryInput *input) {
    size_t start=SIZE_MAX;
    for(size_t p=64;p<packet->length && packet->length-p>=30;++p) {
        if(task_catalog_word(packet,p)!=10 || memcmp(packet->bytes+p+4,"TaskResult",10) ||
            task_catalog_word(packet,p+26)!=XR_NATIVE_DECLARATION_TASK_RESULT)continue;
        CHECK(start==SIZE_MAX);start=p;
    }
    CHECK(start!=SIZE_MAX);size_t cursor=start+62;
    CHECK(task_catalog_word(packet,cursor)==2);cursor+=4;
    size_t constraints[2]={0};
    for(unsigned p=0;p<2;++p) {
        constraints[p]=cursor;cursor+=4;CHECK(!task_catalog_word(packet,cursor));cursor+=4;
    }
    CHECK(task_catalog_word(packet,cursor)==3);cursor+=4;
    size_t types[3]={0},flags[3]={0},names[3]={0};const char *field_names[]={"value","error","info"};
    for(unsigned f=0;f<3;++f) {
        names[f]=cursor+4;cursor=task_catalog_name(packet,cursor,field_names[f]);
        types[f]=cursor;cursor+=4;flags[f]=cursor;cursor+=4;
    }
    CHECK(task_catalog_word(packet,cursor)==4);cursor+=4;
    size_t begins[4]={0},counts[4]={0},variants[4]={0};const char *variant_names[]={"Success","Error","Panic","Cancelled"};
    for(unsigned v=0;v<4;++v) {
        variants[v]=cursor+4;cursor=task_catalog_name(packet,cursor,variant_names[v]);
        begins[v]=cursor;cursor+=4;counts[v]=cursor;cursor+=4;
    }
    const size_t offsets[]={start+26,start+30,constraints[0],constraints[1],names[0],
        types[1],types[2],flags[1],variants[1],counts[1],begins[2],counts[3]};
    const uint8_t changes[]={1,1,XR_XIR_CONSTRAINT_ERROR,XR_XIR_CONSTRAINT_ERROR,1,1,1,4,1,1,1,1};
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    uint8_t original_digest[32],original_packet[32];
    memcpy(original_digest,packet->bytes+32,32);memcpy(original_packet,input->sha256,32);
    for(unsigned n=0;n<sizeof(offsets)/sizeof(*offsets);++n) {
        packet->bytes[offsets[n]]^=changes[n];
        XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet->bytes,32);
        xr_sha256_update(&sha,packet->bytes+64,packet->length-64);xr_sha256_final(&sha,packet->bytes+32);
        xr_sha256(packet->bytes,packet->length,input->sha256);
        XrXirLibraryCatalog *refused=NULL;
        XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,input,1,&refused);
        CHECK((status==XR_XIR_BAD_STRUCTURE || status==XR_XIR_BAD_TYPE) && !refused);
        XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.held_bytes==before.held_bytes);packet->bytes[offsets[n]]^=changes[n];
    }
    memcpy(packet->bytes+32,original_digest,32);xr_sha256(packet->bytes,packet->length,input->sha256);
    CHECK(!memcmp(original_packet,input->sha256,32));
    fprintf(stderr,"Cold TaskResult Catalog: twelve independently signed metadata mutations reject and roll back\n");
}
static const char task_catalog_library_source[]=
    "export enum ColdError {Bad{value:i64}}\n"
    "fn scalar()->i64{return 42}\n"
    "fn fail() throws(ColdError)->i64{throw ColdError.Bad{value:9}}\n"
    "fn panicWork()->i64{const ignored=try! fail();return 0}\n"
    "fn spin()->i64{\nwhile(true){Coro.yield()}\nreturn 7\n}\n"
    "export fn coldSuccess()->TaskResult<i64,Never>{var task=go scalar();return task.awaitResult()}\n"
    "export fn coldError()->TaskResult<i64,ColdError>{var task=go fail();return task.awaitResult()}\n"
    "export fn coldPanic()->TaskResult<i64,Never>{var task=go panicWork();return task.awaitResult()}\n"
    "export fn coldCancelled()->TaskResult<i64,Never>{var task=go spin();task.cancel();return task.awaitResult()}\n";
static const char task_catalog_consumer_source[]=
    "import {ColdError,coldSuccess,coldError,coldPanic,coldCancelled} from \"./range-lib\"\n"
    "export fn success()->TaskResult<i64,Never>{return coldSuccess()}\n"
    "export fn failure()->TaskResult<i64,ColdError>{return coldError()}\n"
    "export fn panicked()->TaskResult<i64,Never>{return coldPanic()}\n"
    "export fn cancelled()->TaskResult<i64,Never>{return coldCancelled()}\n";
#endif // XIR_TASK_RESULT_CATALOG_CASES_H
