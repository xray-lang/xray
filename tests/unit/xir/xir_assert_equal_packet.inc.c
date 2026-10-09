/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_equal_packet.inc.c - Independent equality facts and rehashed hostile proofs
 */
#include "base/xsha256.h"
#include "xir_assert_equal_golden.h"
#include "xir_assert_equal64_golden.h"
#include "xir_assert_equal65_golden.h"
#include "xir_assert_panics72_golden.h"
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

static void equal_packet_gates(void) {
    XrXirArtifact *checked=panics_core_checked();XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if (packet.length!=sizeof(assert_panics72_golden) || memcmp(packet.bytes,assert_panics72_golden,packet.length)) {
        size_t at=0;
        while (at<packet.length && at<sizeof(assert_panics72_golden) && packet.bytes[at]==assert_panics72_golden[at]) ++at;
        fprintf(stderr,"independent Equal Core KAT length=%zu expected=%zu first=%zu\n",packet.length,sizeof(assert_panics72_golden),at);
    }
    CHECK(packet.length==sizeof(assert_panics72_golden) && !memcmp(packet.bytes,assert_panics72_golden,packet.length));
    assert_previous_packet_rejected(assert_equal_golden, sizeof(assert_equal_golden));
    assert_previous_packet_rejected(assert_equal64_golden,sizeof(assert_equal64_golden));
    assert_previous_packet_rejected(assert_equal65_golden,sizeof(assert_equal65_golden));
    panics_packet_oom(checked);
    size_t op=panics_instruction_offset(&packet,3),live=runtime_live,bytes=runtime_bytes;
    CHECK(op==XR_PANICS72_VECTOR_OP3);
    const struct {size_t offset;uint32_t value;XrXirStatus expected;} attacks[]={
        {XR_PANICS72_VECTOR_GENERIC3+8,8,XR_XIR_BAD_TYPE},
        {XR_PANICS72_VECTOR_GENERIC3+8,0,XR_XIR_BAD_TYPE},
        {XR_PANICS72_VECTOR_GENERIC3+8,XR_XIR_CONSTRAINT_SENDABLE,XR_XIR_BAD_TYPE},
        {XR_PANICS72_VECTOR_GENERIC6+8,0,XR_XIR_BAD_TYPE},
        {op+24,2,XR_XIR_BAD_TYPE},
        {op+4,XR_XIR_I64,XR_XIR_BAD_TYPE},
        {op+12,2,XR_XIR_BAD_TYPE},
        {op+16,1,XR_XIR_BAD_STRUCTURE},
        {XR_PANICS72_VECTOR_DEFAULTS+40,2,XR_XIR_BAD_STRUCTURE},
        {XR_PANICS72_VECTOR_DEFAULTS+48,5,XR_XIR_BAD_STRUCTURE},
        {XR_PANICS72_VECTOR_IDENTITY6+4,1,XR_XIR_BAD_STRUCTURE}
    };
    for (uint32_t i=0;i<sizeof(attacks)/sizeof(*attacks);++i) {
        memcpy(packet.bytes,assert_panics72_golden,packet.length);
        panics_word(packet.bytes+attacks[i].offset,attacks[i].value);panics_hash(&packet);
        XrXirArtifact *read=NULL;XrXirStatus status=xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL);
        if (status!=attacks[i].expected) fprintf(stderr,"Equal attack %u status=%u expected=%u\n",i,status,attacks[i].expected);
        CHECK(status==attacks[i].expected && !read && runtime_live==live && runtime_bytes==bytes);
    }
    for (uint32_t group=0;group<3;++group) {
        memcpy(packet.bytes,assert_panics72_golden,packet.length);
        panics_word(packet.bytes+(group ? 12 : 8),group ? 55+group : 21);panics_hash(&packet);
        size_t early_attempts=source_program_compile_attempts;XrXirArtifact *read=NULL;
        CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_BAD_STRUCTURE && !read && source_program_compile_attempts==early_attempts);
    }
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes() && !runtime_live && !runtime_bytes);
    puts("Independent Core2187 KAT, valid-rehash Equal/proof/default attacks and old56/57 early refusal PASS");
}
