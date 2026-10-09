/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_wire.c - Literal single-schema parameter formula
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"

/* Manually enumerated fields of bind(callback: Fn8)->i64. The immediate
 * callback execution contributes exactly PARAMETER0, without a declaration. */
static const uint32_t rw_prior_payload[56]={
    0,1,0,4,UINT32_C(0x646e6962),1,256,2,1,0,2,0,0,2,
    58,2,0,0,0,0,0,0,0,0,
    33,0,1,0,0,0,0,0,0,0,
    0,0,1,0,0,1,0,0,2,8,0,
    1,1,1,1,1,0,1,1,0,0,0
};
static const uint32_t rw_payload[57]={
    0,1,0,4,1684957538,1,256,2,1,0,2,0,0,2,58,2,0,0,0,0,0,0,0,0,33,0,1,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,2,8,0,0,1,1,1,1,1,0,1,1,0,0,0
};
static void rw_word(uint8_t *bytes, size_t word, uint32_t value) {
    for (unsigned b=0;b<4;++b) bytes[word*4+b]=(uint8_t)(value>>(8*b));
}
static void rw_hash(uint8_t bytes[292]) {
    XrSHA256Context sha;xr_sha256_init(&sha);
    xr_sha256_update(&sha,bytes,32);xr_sha256_update(&sha,bytes+64,228);
    xr_sha256_final(&sha,bytes+32);
}
static void rw_literal(uint8_t bytes[292]) {
    memset(bytes,0,292);memcpy(bytes,"XRCHK\0\0\0",8);
    rw_word(bytes,2,27);rw_word(bytes,3,72);rw_word(bytes,4,2);rw_word(bytes,6,228);
    for (size_t w=0;w<57;++w) rw_word(bytes+64,w,rw_payload[w]);
    rw_hash(bytes);
}
static void rw_shape(const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->stage==2 && m->function_count==1 && m->functions[0].name_length==4 &&
        !memcmp(m->functions[0].name,"bind",4) && m->provenance && m->provenance->kind==1);
    const XrXirFunctionEffectContract *c=&m->provenance->contracts[0];
    CHECK(c->parameter_count==1 && c->parameters[0].kind==1 && c->parameters[0].uses==1 &&
        c->formula.constant_mask==0 && c->formula.term_count==1 &&
        c->formula.terms[0].kind==1 && c->formula.terms[0].index==0 && !c->value_count && !c->binding_count);
}
static XrXirStatus rw_read(const XrXirCompileContext *context, XrXirArtifact **output) {
    uint8_t bytes[292];rw_literal(bytes);
    return xr_xir_compile_checked_read(context,bytes,sizeof(bytes),output,NULL);
}
static void rw_roundtrip(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t live=rp_stats(&context).live_bytes;
    uint8_t bytes[292];rw_literal(bytes);
    RootParameterFixture f;rp_fixture(&f,&context);f.function.name="bind";f.function.name_length=4;
    f.module.stage=XR_XIR_BUILT;XrXirArtifact *checked=NULL,*read=NULL;XrXirCheckedPacket packet={0};
    CHECK(xir_fixture_check(&context, &f.module, &checked, NULL)==XR_XIR_OK && checked);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK && packet.length==292);
    CHECK(!memcmp(packet.bytes,bytes,292));
    xr_xir_compile_artifact_free(checked);xr_xir_compile_checked_packet_free(&packet);
    CHECK(rw_read(&context,&read)==XR_XIR_OK && read);memset(bytes,0xa9,sizeof(bytes));
    rw_shape(read);xr_xir_compile_artifact_free(read);
    rp_owner_free(&context,live);rp_balanced(physical);
}
static void rw_previous_literal(void) {
    uint8_t bytes[288]={0};memcpy(bytes,"XRCHK\0\0\0",8);
    rw_word(bytes,2,26);rw_word(bytes,3,71);rw_word(bytes,4,2);rw_word(bytes,6,224);
    for(size_t w=0;w<56;++w)rw_word(bytes+64,w,rw_prior_payload[w]);
    XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,bytes,32);
    xr_sha256_update(&hash,bytes+64,224);xr_sha256_final(&hash,bytes+32);
    RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t live=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    XrXirArtifact *out=NULL;
    CHECK(xr_xir_compile_checked_read(&context,bytes,sizeof(bytes),&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
    out=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&context,bytes,sizeof(bytes),&out,NULL)==XR_XIR_BAD_STRUCTURE && out==(XrXirArtifact *)(uintptr_t)1);
    CHECK(rp_attempts==attempts && rp_stats(&context).live_bytes==live);
    rp_owner_free(&context,live);rp_balanced(mark);
}
static void rw_rejections(void) {
    /* Wire ordinals name the independent payload above, never writer output. */
    static const struct { uint32_t word,value;XrXirStatus status; } cases[]={
        {2,25,XR_XIR_BAD_STRUCTURE},{3,70,XR_XIR_BAD_STRUCTURE},
        {2,26,XR_XIR_BAD_STRUCTURE},{3,71,XR_XIR_BAD_STRUCTURE},
        {4,1,XR_XIR_BAD_STAGE},{5,1,XR_XIR_BAD_STRUCTURE},
        {16+43,0,XR_XIR_BAD_TYPE},{16+43,1,XR_XIR_BAD_TYPE},
        {16+48,0,XR_XIR_BAD_STRUCTURE},{16+49,0,XR_XIR_BAD_STRUCTURE},
        {16+50,0,XR_XIR_BAD_TYPE},{16+51,4,XR_XIR_BAD_TYPE},
        {16+53,2,XR_XIR_BAD_STRUCTURE},{16+54,1,XR_XIR_BAD_STRUCTURE},
        {16+47,2,XR_XIR_BAD_STRUCTURE},{16+46,2,XR_XIR_BAD_STRUCTURE},
        {16+46,0,XR_XIR_BAD_STRUCTURE}
    };
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t live=rp_stats(&context).live_bytes;
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        uint8_t bytes[292];rw_literal(bytes);rw_word(bytes,cases[i].word,cases[i].value);rw_hash(bytes);
        XrXirArtifact *read=NULL;XrXirDiagnostic d={0};
        XrXirStatus status=xr_xir_compile_checked_read(&context,bytes,sizeof(bytes),&read,&d);
        if (status!=cases[i].status) fprintf(stderr,"wire case%zu actual%u expected%u\n",i,status,cases[i].status);
        CHECK(status==cases[i].status && !read && d.status==status && rp_stats(&context).live_bytes==live);
    }
    rp_owner_free(&context,live);rp_balanced(physical);
}
static void rw_resources(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext normal=rp_owner(rp_caps());
    uint64_t live=rp_stats(&normal).live_bytes;XrXirArtifact *read=NULL;rp_attempts=0;
    CHECK(rw_read(&normal,&read)==XR_XIR_OK && read);rw_shape(read);
    size_t sites=rp_attempts;XrCompileResourceStats required=rp_stats(&normal);CHECK(sites);
    xr_xir_compile_artifact_free(read);rp_owner_free(&normal,live);rp_balanced(physical);
    for (size_t i=0;i<sites;++i) {
        XrXirCompileContext c=rp_owner(rp_caps());live=rp_stats(&c).live_bytes;
        RootParameterMark mark=rp_mark();rp_attempts=0;rp_fail_at=i;rp_injected=false;read=NULL;
        CHECK(rw_read(&c,&read)==XR_XIR_OUT_OF_MEMORY && rp_injected && !read);rp_balanced(mark);
        XrCompileResourceStats failed=rp_stats(&c);rp_fail_at=SIZE_MAX;
        CHECK(rw_read(&c,&read)==XR_XIR_OK && read);rw_shape(read);
        CHECK(rp_stats(&c).work>=failed.work && rp_stats(&c).allocated_bytes>=failed.allocated_bytes);
        xr_xir_compile_artifact_free(read);rp_owner_free(&c,live);rp_balanced(physical);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=rp_caps();
        if (axis==0) limits.allocated_bytes=required.allocated_bytes-minus;
        if (axis==1) limits.live_bytes=required.peak_bytes-minus;
        if (axis==2) limits.work=required.work-minus;
        XrXirCompileContext c=rp_owner(limits);live=rp_stats(&c).live_bytes;read=NULL;
        CHECK(rw_read(&c,&read)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(minus?!read:read!=NULL);xr_xir_compile_artifact_free(read);
        rp_owner_free(&c,live);rp_balanced(physical);
    }
    printf("literal Checked72 reader actual OOM sites=%zu\n",sites);
}
int main(void) {
    rw_roundtrip();rw_previous_literal();rw_rejections();rw_resources();CHECK(!rp_live && !rp_live_bytes);return 0;
}
