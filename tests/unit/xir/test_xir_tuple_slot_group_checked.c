/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_slot_group_checked.c - Independent complete packets and exact admission roles
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "slot_group65_golden.h"
#include "xir_checked_scalar64_golden.h"

_Static_assert(XR_XIR_SLOT_GROUP_INIT==145 && XR_XIR_TUPLE_NEW==143 && XR_XIR_RETURN==33,"independent opcode roles");
_Static_assert(XR_XIR_CHECKED_SCHEMA==25 && XR_XIR_CHECKED_CONTRACT==65,"current single reader identity");
static void group_digest(uint8_t *bytes,size_t length) {
    XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,bytes,32);
    xr_sha256_update(&sha,bytes+64,length-64);xr_sha256_final(&sha,bytes+32);
}
static void group_word(uint8_t *bytes,size_t offset,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes[offset+i]=(uint8_t)(value>>(8*i));
}
static XrXirStatus group_packet_pipeline(const uint8_t *bytes,size_t length,XrCompileResourceLimits caps,
    XrCompileResourceStats *measured) {
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    XrCompileResourceStatus created=xr_compile_resources_new(&caps,&context.resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    XrXirCheckedPacket written={0};XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus status=xr_xir_compile_checked_read(&context,bytes,length,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(checked,&written,NULL);
    if(status==XR_XIR_OK)CHECK(written.length==length && !memcmp(written.bytes,bytes,length));
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&closed,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    if(status==XR_XIR_OK) {
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(lowered,0);
        CHECK(layout && layout->outgoing_count==(length==sizeof(slot_group_unit65_golden)?0u:2u));
        status=xr_xir_compile_vm_program_take(&lowered,&program);
    }
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_checked_packet_free(&written);xr_xir_compile_program_drop(program);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes==baseline.live_bytes);if(measured)*measured=stats;
    xr_compile_resources_release(context.resources);
    CHECK(!source_program_compile_live && !source_program_compile_bytes && !runtime_live && !runtime_bytes);return status;
}
static size_t group_packet_owner(const uint8_t *bytes,size_t length) {
    const XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,128000000};
    source_program_compile_attempts=0;CHECK(group_packet_pipeline(bytes,length,caps,NULL)==XR_XIR_OK);
    size_t sites=source_program_compile_attempts;CHECK(sites);
    for(size_t fault=0;fault<sites;++fault) {
        source_program_compile_attempts=0;source_program_compile_injected=false;source_program_compile_fail_at=fault;
        CHECK(group_packet_pipeline(bytes,length,caps,NULL)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_program_compile_injected && source_program_compile_attempts>fault);
        source_program_compile_fail_at=SIZE_MAX;
    }
    XrCompileResourceStats stats={0},got={0};CHECK(group_packet_pipeline(bytes,length,caps,&stats)==XR_XIR_OK);
    XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
    CHECK(group_packet_pipeline(bytes,length,exact,&got)==XR_XIR_OK && got.allocated_bytes==exact.allocated_bytes &&
        got.peak_bytes==exact.live_bytes && got.work==exact.work);
    for(unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits short_caps=exact;
        if(!axis)--short_caps.allocated_bytes;else if(axis==1)--short_caps.live_bytes;else --short_caps.work;
        CHECK(group_packet_pipeline(bytes,length,short_caps,NULL)==XR_XIR_BUDGET);
    }
    printf("Group full packet owner: bytes=%zu actual compiler OOM=%zu, allocated=%llu/peak=%llu/work=%llu, exact+3minusone physical0\n",
        length,sites,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    return sites;
}
static void group_rejected(const uint8_t *bytes,size_t length,XrXirStatus expected,bool early) {
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *output=NULL;
    XrXirStatus status=xr_xir_compile_checked_read(&context,bytes,length,&output,NULL);
    if(status!=expected)fprintf(stderr,"group rejected got=%u expected=%u\n",status,expected);
    CHECK(status==expected && !output);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes);
    if(early)CHECK(after.allocated_bytes==before.allocated_bytes);
    xr_compile_resources_release(context.resources);CHECK(!source_program_compile_live && !source_program_compile_bytes);
}
static void group_wire_rejections(void) {
    /* Complete prior current packet, with its original valid digest and body. */
    group_rejected(checked_scalar64_golden,sizeof(checked_scalar64_golden),XR_XIR_BAD_STRUCTURE,true);
    enum {GROUP_OFFSET=356,PAYLOAD_OFFSET=456};
    /* init instructions begin116: CONST1/2, Tuple, CONST3/4, Tuple, group. */
    CHECK(slot_group_mixed65_golden[GROUP_OFFSET]==145 && slot_group_mixed65_golden[PAYLOAD_OFFSET]==2);
    const struct {size_t offset;uint32_t value;XrXirStatus expected;} cases[]={
        {GROUP_OFFSET+24,0,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+28,1,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+28,UINT32_MAX,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+8,UINT32_MAX,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+12,1,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+12,3,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+16,1,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+20,1,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+32,1,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+36,1,XR_XIR_BAD_STRUCTURE},
        {GROUP_OFFSET+4,XR_XIR_I64,XR_XIR_BAD_TYPE},
        {PAYLOAD_OFFSET,0,XR_XIR_BAD_TYPE}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        uint8_t raw[sizeof(slot_group_mixed65_golden)];memcpy(raw,slot_group_mixed65_golden,sizeof(raw));
        group_word(raw,cases[i].offset,cases[i].value);group_digest(raw,sizeof(raw));
        group_rejected(raw,sizeof(raw),cases[i].expected,false);
    }
    for(unsigned which=0;which<2;++which) {
        uint8_t raw[sizeof(slot_group_unit65_golden)];memcpy(raw,slot_group_unit65_golden,sizeof(raw));
        group_word(raw,116+(which?12:8),1);group_digest(raw,sizeof(raw));
        group_rejected(raw,sizeof(raw),XR_XIR_BAD_STRUCTURE,false);
    }
}
int main(void) {
    size_t faults=group_packet_owner(slot_group_mixed65_golden,sizeof(slot_group_mixed65_golden));
    faults+=group_packet_owner(slot_group_unit65_golden,sizeof(slot_group_unit65_golden));
    group_wire_rejections();
    puts("Independent full wire25/65 group KAT2/current write/read/Lowered m2,m0/14forged+old64 early reject PASS");
    printf("All group packet compiler faults=%zu; unchanged Value21/Call26/Program29, physical0\n",faults);return 0;
}
