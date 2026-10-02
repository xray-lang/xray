/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nullable_values.c - Independent sum layout, lifetime and physical gates
 */
#include "xir/xxir_nullable.h"
#include "xir/xxir_equal.h"
#include "xir/xxir_array.h"
#include "xir/xxir_type_arena.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_nullable_golden.h"

static void nullable_word(uint8_t *bytes,uint32_t word) {
    for (uint32_t i=0;i<4;++i) bytes[i]=(uint8_t)(word>>(8*i));
}
static void nullable_packet_hash(XrXirCheckedPacket *packet) {
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,packet->bytes,32);
    xr_sha256_update(&hash,packet->bytes+64,packet->length-64);xr_sha256_final(&hash,packet->bytes+32);
}
static void nullable_packets(void) {
    for (uint32_t some=0;some<2;++some) {
        const uint8_t *golden=some ? nullable_some_golden : nullable_none_golden;
        size_t length=some ? sizeof(nullable_some_golden) : sizeof(nullable_none_golden);
        XrXirArtifact *checked=NULL;XrXirCheckedPacket packet={0};
        CHECK(xr_xir_checked_read(golden,length,NULL,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL)==XR_XIR_OK);
        CHECK(packet.length==length && !memcmp(packet.bytes,golden,length));xr_xir_artifact_free(checked);
        size_t live=runtime_live,bytes=runtime_bytes;size_t op=some ? 153 : 113;
        const struct {size_t offset;uint32_t value;} attacks[]={
            {length-20,6},{length-16,1},{length-12,XR_XIR_UNIT},{length-12,256},{length-12,257},
            {op+4,XR_XIR_I64},{op+8,1},{op+12,1},{op+16,1},{op+20,1},{op+24,1},{op+28,1},
            {op+32,1},{op+36,1}
        };
        for (uint32_t i=0;i<sizeof(attacks)/sizeof(attacks[0]);++i) {
            memcpy(packet.bytes,golden,length);nullable_word(packet.bytes+attacks[i].offset,attacks[i].value);
            nullable_packet_hash(&packet);checked=(XrXirArtifact *)(uintptr_t)1;
            XrXirStatus status=xr_xir_checked_read(packet.bytes,length,NULL,&checked,NULL);
            if (status==XR_XIR_OK) fprintf(stderr,"Nullable packet some=%u attack=%u accepted\n",some,i);
            CHECK(status!=XR_XIR_OK && !checked && runtime_live==live && runtime_bytes==bytes);
        }
        memcpy(packet.bytes,golden,length);nullable_word(packet.bytes+12,57);nullable_packet_hash(&packet);
        runtime_attempts=0;checked=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_checked_read(packet.bytes,length,NULL,&checked,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(!checked && !runtime_attempts && runtime_live==live && runtime_bytes==bytes);
        runtime_attempts=0;CHECK(xr_xir_checked_read(golden,length,NULL,&checked,NULL)==XR_XIR_OK);
        size_t sites=runtime_attempts;xr_xir_artifact_free(checked);
        for (size_t at=0;at<sites;++at) {
            runtime_attempts=0;runtime_fail_at=at;checked=(XrXirArtifact *)(uintptr_t)1;
            XrXirStatus status=xr_xir_checked_read(golden,length,NULL,&checked,NULL);runtime_fail_at=SIZE_MAX;
            CHECK(status==XR_XIR_OUT_OF_MEMORY && !checked && runtime_live==live && runtime_bytes==bytes);
        }
        xr_xir_checked_packet_free(&packet);CHECK(!runtime_live && !runtime_bytes);
        printf("Nullable independent packet some=%u attacks=%zu OOM=%zu old57 early refusal PASS\n",
            some,sizeof(attacks)/sizeof(attacks[0]),sites);
    }
}

static XrXirValueAdmission nullable_admission(XrXirDomain *domain,XrXirTypeArena *arena) {
    return (XrXirValueAdmission){arena,domain,NULL,NULL,1000000,1048576};
}
static XrXirTypeArena *nullable_arena(XrXirDomain *domain) {
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_F64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)257},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)258},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)260}
    };
    XrXirTypes types={nodes,6,NULL,NULL};XrXirBudget budget=xr_xir_default_budget();
    XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_type_arena_new(domain,&types,&budget,&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static void nullable_layouts(void) {
    const XrXirType elements[]={XR_XIR_BOOL,XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,
        XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,XR_XIR_U64,XR_XIR_F32,XR_XIR_F64,XR_XIR_STRING};
    const uint32_t sizes[]={2,2,4,8,16,2,4,8,16,8,16,16};
    const uint32_t alignments[]={1,1,2,4,8,1,2,4,8,4,8,8};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    for (uint32_t i=0;i<sizeof(elements)/sizeof(elements[0]);++i) {
        XrXirTypeNode node={.kind=XR_XIR_TYPE_NULLABLE,.element=elements[i]};
        XrXirTypes types={&node,1,NULL,NULL};XrXirLayout layout={0};
        CHECK(xr_xir_layout(&types,(XrXirType)256,&target,XR_XIR_LAYOUT_STORAGE,&layout)==XR_XIR_OK);
        CHECK(layout.size==sizes[i] && layout.alignment==alignments[i]);
        CHECK(xr_xir_layout(&types,(XrXirType)256,&target,XR_XIR_LAYOUT_SSA,&layout)==XR_XIR_OK);
        CHECK(layout.size==8 && layout.alignment==8);
        CHECK(xr_xir_layout(&types,(XrXirType)256,&target,XR_XIR_LAYOUT_PARAMETER,&layout)==XR_XIR_OK);
        CHECK(layout.size==16 && layout.alignment==8);
    }
}
static void nullable_variants(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=nullable_arena(domain),*foreign=nullable_arena(domain);
    XrXirValueAdmission admission=nullable_admission(domain,arena);
    XrXirValue zero={XR_XIR_I64,0,0},some={0},none={0},held={0};
    CHECK(xr_xir_nullable_new((XrXirType)256,&zero,&admission,&some)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)256,NULL,&admission,&none)==XR_XIR_VALUE_OK);
    bool active=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(&some,&active,&payload) && active && payload->type==XR_XIR_I64 && !payload->payload);
    CHECK(xr_xir_nullable_view(&none,&active,&payload) && !active && !payload);
    bool equal=false;CHECK(xr_xir_value_equal(&none,&none,(XrXirType)256,&admission,&equal)==XR_XIR_VALUE_OK && equal);
    CHECK(xr_xir_value_equal(&none,&some,(XrXirType)256,&admission,&equal)==XR_XIR_VALUE_OK && !equal);
    CHECK(xr_xir_value_copy(&none,&held)==XR_XIR_VALUE_OK);
    admission.arena=foreign;equal=true;
    CHECK(xr_xir_value_equal(&none,&none,(XrXirType)256,&admission,&equal)==XR_XIR_VALUE_BAD_ARGUMENT && equal);
    admission.arena=arena;XrXirValue output={0};size_t live=runtime_live,bytes=runtime_bytes;
    CHECK(xr_xir_nullable_new((XrXirType)256,&some,&admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!output.type && !output.payload && live==runtime_live && bytes==runtime_bytes);
    XirNominalValue *raw=(XirNominalValue *)object_pointer(&some);raw->variant=2;
    CHECK(!xr_xir_value_valid(&some));raw->variant=1;
    xr_xir_value_drop(&some);xr_xir_value_drop(&none);
    xr_xir_type_arena_drop(arena);xr_xir_type_arena_drop(foreign);
    CHECK(xr_xir_nullable_view(&held,&active,&payload) && !active && !payload);
    xr_xir_value_drop(&held);xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
static void nullable_ieee_and_cow(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=nullable_arena(domain);XrXirValueAdmission admission=nullable_admission(domain,arena);
    XrXirValue nan={XR_XIR_F64,0,INT64_C(0x7ff8000000000000)},zero={XR_XIR_F64,0,0},negative={XR_XIR_F64,0,INT64_MIN};
    XrXirValue values[4]={{0}},array={0},alias={0},read={0};
    CHECK(xr_xir_nullable_new((XrXirType)257,&nan,&admission,&values[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,&zero,&admission,&values[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,&negative,&admission,&values[2])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,NULL,&admission,&values[3])==XR_XIR_VALUE_OK);
    bool equal=true;
    CHECK(xr_xir_value_equal(&values[0],&values[0],(XrXirType)257,&admission,&equal)==XR_XIR_VALUE_OK && !equal);
    CHECK(xr_xir_value_equal(&values[1],&values[2],(XrXirType)257,&admission,&equal)==XR_XIR_VALUE_OK && equal);
    CHECK(xr_xir_array_new((XrXirType)259,values,4,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array,&alias)==XR_XIR_VALUE_OK && alias.payload==array.payload);
    CHECK(xr_xir_value_equal(&array,&alias,(XrXirType)259,&admission,&equal)==XR_XIR_VALUE_OK && !equal);
    XrXirValuePlace place={(XrXirType)array.type,&array.payload};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_set(&place,0,&values[3],&admission,&fault)==XR_XIR_VALUE_OK && array.payload!=alias.payload);
    CHECK(xr_xir_value_equal(&array,&array,(XrXirType)259,&admission,&equal)==XR_XIR_VALUE_OK && equal);
    CHECK(xr_xir_array_get(&alias,0,&admission,&read,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_equal(&read,&read,(XrXirType)257,&admission,&equal)==XR_XIR_VALUE_OK && !equal);
    xr_xir_value_drop(&read);xr_xir_value_drop(&array);xr_xir_value_drop(&alias);
    for (uint32_t i=0;i<4;++i) xr_xir_value_drop(&values[i]);
    xr_xir_type_arena_drop(arena);xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
static void nullable_actual_oom(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirTypeArena *arena=nullable_arena(domain);XrXirValueAdmission admission=nullable_admission(domain,arena);
    XrXirValue text={0},values[2]={{0}},array={0},held={0};
    CHECK(xr_xir_string_new(domain,"a\0b",3,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)258,NULL,&admission,&values[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)258,&text,&admission,&values[1])==XR_XIR_VALUE_OK);
    for (uint32_t operation=0;operation<4;++operation) {
        XrXirValue output={0};XrXirFaultDetail fault={0};runtime_attempts=0;
        admission=nullable_admission(domain,arena);
        XrXirValueStatus status=operation<2 ? xr_xir_nullable_new((XrXirType)258,operation ? &text : NULL,&admission,&output) :
            operation==2 ? xr_xir_array_new((XrXirType)260,values,2,&admission,&output) :
            xr_xir_array_get(&array,1,&admission,&output,&fault);
        CHECK(status==XR_XIR_VALUE_OK);size_t sites=runtime_attempts;CHECK(sites>0);
        if (operation==2) {CHECK(xr_xir_value_copy(&output,&array)==XR_XIR_VALUE_OK);}
        xr_xir_value_drop(&output);size_t live=runtime_live,bytes=runtime_bytes;
        for (size_t at=0;at<sites;++at) {
            admission=nullable_admission(domain,arena);runtime_attempts=0;runtime_fail_at=at;
            status=operation<2 ? xr_xir_nullable_new((XrXirType)258,operation ? &text : NULL,&admission,&output) :
                operation==2 ? xr_xir_array_new((XrXirType)260,values,2,&admission,&output) :
                xr_xir_array_get(&array,1,&admission,&output,&fault);
            runtime_fail_at=SIZE_MAX;
            CHECK(status==XR_XIR_VALUE_OOM && !output.type && !output.payload && !output.reserved);
            CHECK(runtime_live==live && runtime_bytes==bytes && admission.scratch_bytes==1048576);
        }
        printf("Nullable operation %u actual OOM sites=%zu physical refund PASS\n",operation,sites);
    }
    XrXirFaultDetail fault={0};admission=nullable_admission(domain,arena);
    CHECK(xr_xir_array_get(&array,1,&admission,&held,&fault)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);xr_xir_value_drop(&values[0]);xr_xir_value_drop(&values[1]);xr_xir_value_drop(&text);
    xr_xir_type_arena_drop(arena);bool some=false;const XrXirValue *payload=NULL;const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_nullable_view(&held,&some,&payload) && some && xr_xir_string_view(payload,&bytes,&length));
    CHECK(length==3 && !memcmp(bytes,"a\0b",3));
    xr_xir_value_drop(&held);xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
int main(void) {
    nullable_packets();nullable_layouts();nullable_variants();nullable_ieee_and_cow();nullable_actual_oom();
    puts("Nullable independent layout/variants/arena/IEEE/COW/OOM/lifetime PASS");return 0;
}
