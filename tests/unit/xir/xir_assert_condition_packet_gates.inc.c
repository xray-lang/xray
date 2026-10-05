/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_packet_gates.inc.c - Full framed packet and semantic rejection
 */
#include "base/xsha256.h"
#include "xir_assert_condition_golden.h"
static void assert_packet_hash(XrXirCheckedPacket *packet) {
    XrSHA256Context sha;xr_sha256_init(&sha);
    xr_sha256_update(&sha,packet->bytes,32);xr_sha256_update(&sha,packet->bytes+64,packet->length-64);
    xr_sha256_final(&sha,packet->bytes+32);
}
static void assert_word(uint8_t *bytes,uint32_t word) {
    for (uint32_t i=0;i<4;++i) bytes[i]=(uint8_t)(word>>(8*i));
}
static size_t assert_pattern(const XrXirCheckedPacket *packet,const uint8_t *bytes,size_t size) {
    size_t position=SIZE_MAX,count=0;
    for (size_t i=64;i+size<=packet->length;++i) if (!memcmp(packet->bytes+i,bytes,size)) {position=i;++count;}
    CHECK(count==1);return position;
}
static void assert_packet_oom(const XrXirArtifact *checked) {
    assert_compile_stage_cases(checked,ASSERT_COMPILE_WRITER,"Assertion framed writer/replay");
    assert_compile_stage_cases(checked,ASSERT_COMPILE_READER,"Assertion framed reader");
}

static void assert_core_packet(const XrXirSourceResult *source) {
    const XrXirModule *parent=xr_xir_compile_artifact_module(source->checked);
    uint32_t begin=parent->declarations->modules[1].initializer;
    uint32_t helper_function=UINT32_MAX;
    for (uint32_t i=0;i<parent->defaults->count;++i) {
        const XrXirDefaultBinding *record=&parent->defaults->records[i];
        if (record->owner==begin+1 && record->owner_kind==XR_XIR_DEFAULT_PARAMETER && record->ordinal==1) {
            CHECK(helper_function==UINT32_MAX);helper_function=record->function;
        }
    }
    CHECK(helper_function<parent->function_count);
    XrXirFunctionIdentity identities[3]={{0},{0,1,0,0,0,0,0, 0, 0},{0}};
    XrXirSourceModule module=parent->declarations->modules[1];module.initializer=0;
    const XrXirLiteral literal={NULL,0};
    XrXirDeclarations declarations={&module,1,identities,NULL,0,&literal,1,UINT32_MAX,UINT32_MAX,NULL};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,1,1,2};
    XrXirDefaultTable defaults={&binding,1};
    XrXirFunction functions[]={parent->functions[begin],parent->functions[begin+1],parent->functions[helper_function]};
    XrXirInstruction helper[]={functions[2].instructions[0],functions[2].instructions[1]};
    helper[0].immediate=0;functions[2].instructions=helper;
    XrXirModule built={XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_LIBRARY,&defaults};
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_check(assert_compile_context,&built,&checked,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(assert_condition_golden) && !memcmp(packet.bytes,assert_condition_golden,packet.length));
    assert_packet_oom(checked);
    uint8_t instruction[40]={0};assert_word(instruction,115);assert_word(instruction+12,1);
    size_t op=assert_pattern(&packet,instruction,sizeof(instruction));
    uint8_t relation[16]={0};assert_word(relation+4,1);assert_word(relation+8,1);assert_word(relation+12,2);
    size_t default_binding=assert_pattern(&packet,relation,sizeof(relation));
    const struct {size_t offset;uint32_t value;XrXirStatus status;} attacks[]={
        {op+4,XR_XIR_I64,XR_XIR_BAD_TYPE},{op+8,1,XR_XIR_BAD_TYPE},
        {op+12,0,XR_XIR_BAD_TYPE},{op+12,UINT32_MAX,XR_XIR_BAD_VALUE},
        {op+16,1,XR_XIR_BAD_STRUCTURE},{op+20,1,XR_XIR_BAD_STRUCTURE},
        {op+24,1,XR_XIR_BAD_STRUCTURE},{op+32,1,XR_XIR_BAD_STRUCTURE},{op+36,1,XR_XIR_BAD_STRUCTURE},
        {default_binding,1,XR_XIR_BAD_STRUCTURE},{default_binding+4,0,XR_XIR_BAD_STRUCTURE},
        {default_binding+8,0,XR_XIR_BAD_STRUCTURE},{default_binding+12,0,XR_XIR_BAD_STRUCTURE}
    };
    size_t live=runtime_live,bytes=runtime_bytes;
    for (uint32_t i=0;i<sizeof(attacks)/sizeof(attacks[0]);++i) {
        memcpy(packet.bytes,assert_condition_golden,packet.length);assert_word(packet.bytes+attacks[i].offset,attacks[i].value);
        assert_packet_hash(&packet);
        XrXirArtifact *read=NULL;XrXirStatus status=xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL);
        if (status!=attacks[i].status) fprintf(stderr,"assert rehashed packet attack=%u status=%u expected=%u\n",i,status,attacks[i].status);
        CHECK(status==attacks[i].status && !read && runtime_live==live && runtime_bytes==bytes);
    }
    for (uint32_t old=54;old<=57;++old) {
        memcpy(packet.bytes,assert_condition_golden,packet.length);assert_word(packet.bytes+12,old);
        if (old==57) assert_word(packet.bytes+8,21);
        assert_packet_hash(&packet);size_t early_attempts=source_program_compile_attempts;XrXirArtifact *read=NULL;
        CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_BAD_STRUCTURE && !read && source_program_compile_attempts==early_attempts);
    }
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    puts("Full 642-byte independent condition KAT and rehashed typed/default/unused/old54/55/56/wire21 rejection PASS");
}
