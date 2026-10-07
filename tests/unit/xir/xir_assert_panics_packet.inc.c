/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_packet.inc.c - Independent core framing and hostile result roles
 */
#include "base/xsha256.h"
#include "xir_assert_panics_golden.h"
#include "xir_assert_panics64_golden.h"
#include "xir_assert_panics65_golden.h"
#include "xir_assert_panics69_golden.h"
#include "xir_assert_atomic64_packet_rejection.h"
static void panics_word(uint8_t *bytes,uint32_t value) {
    for (uint32_t i=0;i<4;++i) bytes[i]=(uint8_t)(value>>(i*8));
}
static void panics_hash(XrXirCheckedPacket *packet) {
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,packet->bytes,32);
    xr_sha256_update(&hash,packet->bytes+64,packet->length-64);xr_sha256_final(&hash,packet->bytes+32);
}
static uint32_t panics_word_at(const XrXirCheckedPacket *packet,size_t *position) {
    CHECK(*position<=packet->length && packet->length-*position>=4);uint32_t value=0;
    for (uint32_t i=0;i<4;++i) value|=(uint32_t)packet->bytes[*position+i]<<(i*8);
    *position+=4;return value;
}
static size_t panics_instruction_offset(const XrXirCheckedPacket *packet,uint32_t function) {
    size_t at=64;CHECK(panics_word_at(packet,&at)<=XR_XIR_LIBRARY);
    uint32_t count=panics_word_at(packet,&at);CHECK(function<count);panics_word_at(packet,&at);
    for (uint32_t f=0;f<=function;++f) {
        uint32_t length=panics_word_at(packet,&at);at+=length;
        uint32_t parameters=panics_word_at(packet,&at);at+=(size_t)parameters*4;panics_word_at(packet,&at);
        uint32_t blocks=panics_word_at(packet,&at);at+=(size_t)blocks*16;
        uint32_t instructions=panics_word_at(packet,&at);
        if (f==function) {CHECK(instructions);return at;}
        at+=(size_t)instructions*40;uint32_t operands=panics_word_at(packet,&at);at+=(size_t)operands*4;
    }
    CHECK(false);return 0;
}
static XrXirArtifact *panics_core_checked(void) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(assert_compile_context->resources,&session)==XR_COMPILER_SESSION_OK);CHECK(session);
    XrModuleResolverConfig config={0};XrModuleResolver *resolver=NULL;CHECK(xr_compile_module_resolver_new(assert_compile_context->resources,&config,&resolver)==XR_MODULE_OK);CHECK(resolver);
    SourceContext core={0};core.compile=*assert_compile_context;core.remaining_blocks=core.compile.limits.blocks;core.remaining_instructions=core.compile.limits.instructions;core.linkage_kind=XR_XIR_LIBRARY;core.core_factory=true;
    CHECK(xr_compile_module_graph_new(assert_compile_context->resources,session,resolver,&core.graph)==XR_MODULE_OK);CHECK(core.graph);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"xray-core-assertions-v1",NULL};char *error=NULL;
    CHECK(!xr_compile_module_graph_build_source(core.graph,&authority,xir_core_declaration_source,&error));
    CHECK(!xr_compile_module_graph_topological_sort(core.graph) && source_manifests_load(&core));
    XrXirSourceResult result={0};source_construct(&core,&result);
    CHECK(core.diagnostic.status==XR_XIR_OK && result.checked);
    source_core_dispose(&core);xr_compile_module_resolver_free(resolver);xr_compile_session_free(session);xr_compile_resources_free(error);
    return result.checked;
}
static void panics_packet_oom(const XrXirArtifact *checked) {
    assert_compile_stage_cases(checked,ASSERT_COMPILE_WRITER,"Assertion framed writer/replay");
    assert_compile_stage_cases(checked,ASSERT_COMPILE_READER,"Assertion framed reader");
}

static void panics_previous65_packet_rejected(void) {
    const uint8_t *bytes=assert_panics65_golden;size_t length=sizeof(assert_panics65_golden);
    const uint8_t prefix[]={
        'X','R','C','H','K',0,0,0,25,0,0,0,65,0,0,0,
        2,0,0,0,0,0,0,0,0x77,0x07,0,0,0,0,0,0
    };
    CHECK(length==1975 && !memcmp(bytes,prefix,sizeof(prefix)));
    XrSHA256Context hash;uint8_t digest[32];xr_sha256_init(&hash);
    xr_sha256_update(&hash,bytes,32);xr_sha256_update(&hash,bytes+64,length-64);
    xr_sha256_final(&hash,digest);CHECK(!memcmp(digest,bytes+32,sizeof(digest)));
    size_t attempts=source_program_compile_attempts;
    size_t blocks=source_program_compile_live,allocated=source_program_compile_bytes;
    size_t live=runtime_live,runtime_allocated=runtime_bytes;
    XrXirArtifact *empty=NULL;
    CHECK(xr_xir_compile_checked_read(assert_compile_context,bytes,length,&empty,NULL)==XR_XIR_BAD_STRUCTURE && !empty);
    XrXirArtifact *sentinel=(XrXirArtifact *)(uintptr_t)1,*occupied=sentinel;
    CHECK(xr_xir_compile_checked_read(assert_compile_context,bytes,length,&occupied,NULL)==XR_XIR_BAD_STRUCTURE && occupied==sentinel);
    CHECK(source_program_compile_attempts==attempts && source_program_compile_live==blocks &&
        source_program_compile_bytes==allocated && runtime_live==live && runtime_bytes==runtime_allocated);
    puts("Complete prior25/65 assertion packet rejected before allocation; empty/occupied output preserved PASS");
}

static void panics_packet_gates(void) {
    XrXirArtifact *checked=panics_core_checked();XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if (packet.length!=sizeof(assert_panics69_golden) || memcmp(packet.bytes,assert_panics69_golden,packet.length)) {
        size_t at=0;while (at<packet.length && at<sizeof(assert_panics69_golden) && packet.bytes[at]==assert_panics69_golden[at]) ++at;
        fprintf(stderr,"independent Core KAT mismatch length=%zu expected=%zu first=%zu\n",packet.length,sizeof(assert_panics69_golden),at);
    }
    CHECK(packet.length==sizeof(assert_panics69_golden) && !memcmp(packet.bytes,assert_panics69_golden,packet.length));
    assert_previous_packet_rejected(assert_panics_golden, sizeof(assert_panics_golden));
    assert_previous_packet_rejected(assert_panics64_golden,sizeof(assert_panics64_golden));
    panics_previous65_packet_rejected();
    panics_packet_oom(checked);
    size_t op=panics_instruction_offset(&packet,2),live=runtime_live,bytes=runtime_bytes;
    const struct {size_t offset;uint32_t value;XrXirStatus expected;} attacks[]={
        {XR_PANICS_VECTOR_GENERIC2+4,2,XR_XIR_BAD_STRUCTURE},{XR_PANICS_VECTOR_GENERIC2+8,2,XR_XIR_BAD_STRUCTURE},{XR_PANICS_VECTOR_GENERIC2+8,0,XR_XIR_BAD_STRUCTURE},
        {XR_PANICS_VECTOR_GENERIC2+12,XR_XIR_CONSTRAINT_SENDABLE,XR_XIR_BAD_TYPE},{XR_PANICS_VECTOR_GENERIC5+8,0,XR_XIR_BAD_STRUCTURE},
        {XR_PANICS_VECTOR_DEFAULTS+4,1,XR_XIR_BAD_STRUCTURE},{XR_PANICS_VECTOR_DEFAULTS+28,0,XR_XIR_BAD_STRUCTURE},{XR_PANICS_VECTOR_DEFAULTS+32,3,XR_XIR_BAD_STRUCTURE},
        {op+2*40+24,0,XR_XIR_BAD_TYPE},{op+2*40+4,XR_XIR_I64,XR_XIR_BAD_TYPE},
        {op+4*40+12,0,XR_XIR_BAD_TYPE},{op+8*40+4,XR_XIR_PANIC_INFO,XR_XIR_BAD_TYPE}
    };
    for (uint32_t i=0;i<sizeof(attacks)/sizeof(attacks[0]);++i) {
        memcpy(packet.bytes,assert_panics69_golden,packet.length);panics_word(packet.bytes+attacks[i].offset,attacks[i].value);panics_hash(&packet);
        XrXirArtifact *read=NULL;XrXirStatus status=xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL);
        if (status!=attacks[i].expected) fprintf(stderr,"panics packet attack=%u status=%u expected=%u\n",i,status,attacks[i].expected);
        CHECK(status==attacks[i].expected && !read && runtime_live==live && runtime_bytes==bytes);
    }
    for (uint32_t group=0;group<2;++group) {
        memcpy(packet.bytes,assert_panics69_golden,packet.length);panics_word(packet.bytes+(group ? 12 : 8),group ? 56 : 21);
        panics_hash(&packet);size_t early_attempts=source_program_compile_attempts;XrXirArtifact *read=NULL;
        CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_BAD_STRUCTURE && !read && source_program_compile_attempts==early_attempts);
    }
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes() && !runtime_live && !runtime_bytes);
    puts("Complete independent Core1975 KAT; rehashed role/recipe/helper attacks; old wire/semantic early refusal PASS");
}
