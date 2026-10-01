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
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleResolverConfig config={0};XrModuleResolver *resolver=xr_module_resolver_new(&config);CHECK(resolver);
    SourceContext core={0};core.budget=xr_xir_default_budget();core.linkage_kind=XR_XIR_LIBRARY;core.core_factory=true;
    core.graph=xr_module_graph_new(session,resolver);CHECK(core.graph);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"xray-core-assertions-v1",NULL};char *error=NULL;
    CHECK(!xr_module_graph_build_source(core.graph,&authority,xir_core_declaration_source,&error));
    CHECK(!xr_module_graph_topological_sort(core.graph) && source_manifests_load(&core));
    XrXirSourceResult result={0};XrXirBudget checking=core.budget;source_construct(&core,&checking,&result);
    CHECK(core.diagnostic.status==XR_XIR_OK && result.checked);
    source_core_dispose(&core);xr_module_resolver_free(resolver);xr_compiler_session_delete(session);xr_free(error);
    return result.checked;
}
static void panics_packet_oom(const XrXirArtifact *checked) {
    size_t live=runtime_live,bytes=runtime_bytes;XrXirCheckedPacket kept={0};
    CHECK(xr_xir_checked_write(checked,NULL,&kept,NULL)==XR_XIR_OK);
    size_t kept_live=runtime_live,kept_bytes=runtime_bytes;
    for (uint32_t group=0;group<2;++group) {
        size_t sites=0;
        for (size_t point=0;point<=sites;++point) {
            runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
            XrXirArtifact *read=NULL;XrXirCheckedPacket packet={0};
            XrXirStatus status=group ? xr_xir_checked_read(kept.bytes,kept.length,NULL,&read,NULL) :
                xr_xir_checked_write(checked,NULL,&packet,NULL);
            if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;}
            else CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_OUT_OF_MEMORY && !read && !packet.bytes);
            runtime_fail_at=SIZE_MAX;xr_xir_artifact_free(read);xr_xir_checked_packet_free(&packet);
            CHECK(runtime_live==kept_live && runtime_bytes==kept_bytes);
        }
        printf("Panics framed %s OOM=%zu complete refund\n",group ? "reader" : "writer",sites);
    }
    xr_xir_checked_packet_free(&kept);CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void equal_packet_gates(void) {
    XrXirArtifact *checked=panics_core_checked();XrXirCheckedPacket packet={0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL)==XR_XIR_OK);
    if (packet.length!=sizeof(assert_equal_golden) || memcmp(packet.bytes,assert_equal_golden,packet.length)) {
        size_t at=0;
        while (at<packet.length && at<sizeof(assert_equal_golden) && packet.bytes[at]==assert_equal_golden[at]) ++at;
        fprintf(stderr,"independent Equal Core KAT length=%zu expected=%zu first=%zu\n",packet.length,sizeof(assert_equal_golden),at);
    }
    CHECK(packet.length==sizeof(assert_equal_golden) && !memcmp(packet.bytes,assert_equal_golden,packet.length));
    panics_packet_oom(checked);
    size_t op=panics_instruction_offset(&packet,3),live=runtime_live,bytes=runtime_bytes;
    CHECK(op==XR_EQUAL_VECTOR_OP3);
    const struct {size_t offset;uint32_t value;XrXirStatus expected;} attacks[]={
        {XR_EQUAL_VECTOR_GENERIC3+8,8,XR_XIR_BAD_TYPE},
        {XR_EQUAL_VECTOR_GENERIC3+8,0,XR_XIR_BAD_TYPE},
        {XR_EQUAL_VECTOR_GENERIC3+8,XR_XIR_CONSTRAINT_SENDABLE,XR_XIR_BAD_TYPE},
        {XR_EQUAL_VECTOR_GENERIC6+8,0,XR_XIR_BAD_TYPE},
        {op+24,2,XR_XIR_BAD_TYPE},
        {op+4,XR_XIR_I64,XR_XIR_BAD_TYPE},
        {op+12,2,XR_XIR_BAD_TYPE},
        {op+16,1,XR_XIR_BAD_STRUCTURE},
        {XR_EQUAL_VECTOR_DEFAULTS+40,2,XR_XIR_BAD_STRUCTURE},
        {XR_EQUAL_VECTOR_DEFAULTS+48,5,XR_XIR_BAD_STRUCTURE},
        {XR_EQUAL_VECTOR_IDENTITY6+4,1,XR_XIR_BAD_STRUCTURE}
    };
    for (uint32_t i=0;i<sizeof(attacks)/sizeof(*attacks);++i) {
        memcpy(packet.bytes,assert_equal_golden,packet.length);
        panics_word(packet.bytes+attacks[i].offset,attacks[i].value);panics_hash(&packet);
        XrXirArtifact *read=NULL;XrXirStatus status=xr_xir_checked_read(packet.bytes,packet.length,NULL,&read,NULL);
        if (status!=attacks[i].expected) fprintf(stderr,"Equal attack %u status=%u expected=%u\n",i,status,attacks[i].expected);
        CHECK(status==attacks[i].expected && !read && runtime_live==live && runtime_bytes==bytes);
    }
    for (uint32_t group=0;group<2;++group) {
        memcpy(packet.bytes,assert_equal_golden,packet.length);
        panics_word(packet.bytes+(group ? 12 : 8),group ? 56 : 21);panics_hash(&packet);
        runtime_attempts=0;XrXirArtifact *read=NULL;
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&read,NULL)==XR_XIR_BAD_STRUCTURE && !read && !runtime_attempts);
    }
    xr_xir_checked_packet_free(&packet);xr_xir_artifact_free(checked);
    CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    puts("Independent Core1919 KAT, valid-rehash Equal/proof/default attacks and old56 early refusal PASS");
}
