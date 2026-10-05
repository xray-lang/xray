/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_rune.c - Scalar-domain, layout and finite compiler ownership oracles
 *
 * KEY CONCEPT:
 *   Concrete Rune validation is independent of integers. Every failure point
 *   replays the real pipeline with a fresh finite owner and counted releases.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_rune.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "base/xsha256.h"
#include "xir_rune_checked_golden.h"
#include "xir_rune_leaf_fixture.h"
#include "xir_rune_leaf_cases.h"
static XrXirStatus rune_build(const XrXirCompileContext *ctx,int64_t codepoint,bool literal,XrXirArtifact **output) {
    XrXirInstruction ops[]={
        {literal?XR_XIR_CONST_RUNE:XR_XIR_CONST_INT,literal?XR_XIR_RUNE:XR_XIR_I64,{0},{0},codepoint,{0}},
        {literal?XR_XIR_COPY:XR_XIR_INTEGER_TO_RUNE,XR_XIR_RUNE,{0},{0},0,{0}},
        {XR_XIR_RUNE_TO_INTEGER,XR_XIR_I64,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    XrXirBlock block={0,4,0,0};
    XrXirFunction fn={"rune",4,NULL,0,XR_XIR_I64,&block,1,ops,4,NULL,0};
    XrXirModule built={XR_XIR_BUILT,&fn,1,NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL;
    XrXirStatus status=xr_xir_compile_check(ctx,&built,&checked,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(checked,&target,output,NULL);
    xr_xir_compile_artifact_free(checked);return status;
}
static void rune_values(void) {
    static const int64_t valid[]={0,0x41,0x7f,0x80,0x7ff,0x800,0xd7ff,0xe000,0xffff,0x10000,0x1f600,0x10ffff};
    static const int64_t invalid[]={-1,INT64_MIN,0xd800,0xdbff,0xdc00,0xdfff,0x110000,INT64_MAX,INT64_C(0x100000041)};
    for(size_t i=0;i<sizeof(valid)/sizeof(valid[0]);++i) {
        XrXirValue value={XR_XIR_RUNE,0,valid[i]},copy={0};
        CHECK(xr_xir_value_valid(&value) && xr_xir_value_argument(&value,NULL,XR_XIR_RUNE));
        CHECK(!xr_xir_value_argument(&value,NULL,XR_XIR_I64) && !xr_xir_type_is_number(XR_XIR_RUNE));
        CHECK(xr_xir_value_copy(&value,&copy)==XR_XIR_VALUE_OK && copy.type==XR_XIR_RUNE && copy.payload==valid[i]);
        int64_t output=-99;
        CHECK(xr_xir_rune_convert(XR_XIR_RUNE,XR_XIR_I64,valid[i],&output)==XR_XIR_RUN_OK && output==valid[i]);
        CHECK(xr_xir_rune_convert(XR_XIR_RUNE,XR_XIR_U32,valid[i],&output)==XR_XIR_RUN_OK && output==valid[i]);
        CHECK(xr_xir_rune_convert(XR_XIR_I64,XR_XIR_RUNE,valid[i],&output)==XR_XIR_RUN_OK && output==valid[i]);
        xr_xir_value_drop(&copy);
    }
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        XrXirValue value={XR_XIR_RUNE,0,invalid[i]},copy={0};int64_t output=-99;
        CHECK(!xr_xir_value_valid(&value) && !xr_xir_value_argument(&value,NULL,XR_XIR_RUNE));
        CHECK(xr_xir_value_copy(&value,&copy)==XR_XIR_VALUE_BAD_ARGUMENT && copy.type==XR_XIR_UNIT);
        CHECK(xr_xir_rune_convert(XR_XIR_I64,XR_XIR_RUNE,invalid[i],&output)==XR_XIR_RUN_NUMERIC_RANGE && output==-99);
        CHECK(xr_xir_rune_convert(XR_XIR_RUNE,XR_XIR_I64,invalid[i],&output)==XR_XIR_RUN_BAD_ARGUMENT && output==-99);
    }
    int64_t output=7;
    CHECK(xr_xir_rune_convert(XR_XIR_U32,XR_XIR_RUNE,65,&output)==XR_XIR_RUN_BAD_ARGUMENT && output==7);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    for(XrXirLayoutContext c=XR_XIR_LAYOUT_STORAGE;c<=XR_XIR_LAYOUT_FRAME;c++) {
        XrXirLayout layout={0};CHECK(xr_xir_builtin_layout(XR_XIR_RUNE,&target,c,&layout)==XR_XIR_OK);
        CHECK(layout.size==(c==XR_XIR_LAYOUT_STORAGE?4u:c==XR_XIR_LAYOUT_SSA || c==XR_XIR_LAYOUT_FRAME?8u:16u));
        CHECK(layout.alignment==(c==XR_XIR_LAYOUT_STORAGE?4u:8u));
    }
    target.abi_version=18;XrXirLayout untouched={77,88},before=untouched;
    CHECK(xr_xir_builtin_layout(XR_XIR_RUNE,&target,XR_XIR_LAYOUT_STORAGE,&untouched)==XR_XIR_BAD_LAYOUT);
    CHECK(untouched.size==before.size && untouched.alignment==before.alignment);
    CHECK(!runtime_live && !runtime_bytes);
}
static void rune_text(void) {
    static const struct {int64_t code;const char *bytes;size_t length;} vectors[]={
        {0,"\0",1},{0x41,"A",1},{0xd7ff,"\xed\x9f\xbf",3},{0xe000,"\xee\x80\x80",3},
        {0x1f600,"\xf0\x9f\x98\x80",4},{0x10ffff,"\xf4\x8f\xbf\xbf",4}};
    for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i) {
        XrXirValue value={XR_XIR_RUNE,0,vectors[i].code};char buffer[XR_XIR_SCALAR_TEXT_BYTES];
        const char *text=NULL;size_t length=0;
        CHECK(xr_xir_scalar_text(&value,buffer,&text,&length) && length==vectors[i].length);
        CHECK(!memcmp(text,vectors[i].bytes,length));
    }
    static const struct {const char *left,*right;size_t a,b;int expected;} pairs[]={
        {"","",0,0,0},{"","a",0,1,-1},{"a","",1,0,1},{"ab","abc",2,3,-1},
        {"a\0z","a\0a",3,3,1},{"\x7f","\xc2\x80",1,2,-1},
        {"\xc3\xa9","e\xcc\x81",2,3,1},{"-5","abcd",2,4,-1}};
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    for(size_t i=0;i<sizeof(pairs)/sizeof(pairs[0]);++i) {
        XrXirValue left={0},right={0};int order=77;
        CHECK(xr_xir_string_new(domain,pairs[i].left,pairs[i].a,&left)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain,pairs[i].right,pairs[i].b,&right)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_compare(&left,&right,&order) && order==pairs[i].expected);
        left.reserved=1;order=77;CHECK(!xr_xir_string_compare(&left,&right,&order) && order==77);
        left.reserved=0;xr_xir_value_drop(&left);xr_xir_value_drop(&right);
    }
    xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}
static void rune_execution(void) {
    static const int64_t values[]={0,0xd7ff,0xe000,0x10ffff,-1,0xd800,0xdfff,0x110000};
    for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) for(unsigned literal=0;literal<2;++literal) {
        const XrXirCompileContext *ctx=source_program_owner(UINT64_C(64)*1024*1024,128000000);
        XrXirArtifact *artifact=NULL;bool valid=xr_xir_rune_payload_valid(values[i]);
        XrXirStatus status=rune_build(ctx,values[i],literal!=0,&artifact);
        if(literal && !valid)CHECK(status==XR_XIR_BAD_TYPE && !artifact);
        else {
            CHECK(status==XR_XIR_OK && artifact);
            XrXirRunContext run={10000,65536,0,0,0,0};XrXirValue output={0};
            XrXirRunStatus result=xr_xir_compile_vm_run(artifact,0,&run,NULL,0,&output);
            CHECK(result==(valid?XR_XIR_RUN_OK:XR_XIR_RUN_NUMERIC_RANGE));
            if(valid)CHECK(output.type==XR_XIR_I64 && output.payload==values[i]);
            else CHECK(output.type==XR_XIR_UNIT && !output.reserved && !output.payload);
            CHECK(!run.live_bytes && run.allocations==run.frees);xr_xir_value_drop(&output);
            xr_xir_compile_artifact_free(artifact);
        }
        source_program_owners_free();CHECK(!runtime_live && !runtime_bytes);
    }
}
/* Exact three-axis probes create one fresh graph owner; no limits change
 * during Check/Lower and no stage receives a replacement ledger. */
static const XrXirCompileContext *rune_resource_owner(uint64_t allocated,uint64_t live,uint64_t work) {
    CHECK(source_program_owner_count<SOURCE_PROGRAM_OWNER_LIMIT);
    CHECK(allocated<=UINT64_C(64)*1024*1024 && live<=UINT64_C(8)*1024*1024 && work<=128000000);
    SourceProgramOwner *owner=&source_program_owners[source_program_owner_count++];
    XrCompileResourceLimits caps={allocated,live,work};
    CHECK(xr_compile_resources_new(&caps,&owner->context.resources)==XR_COMPILE_RESOURCE_OK);
    owner->context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(owner->context.resources,&owner->baseline)==XR_COMPILE_RESOURCE_OK);
    return &owner->context;
}
static XrXirStatus rune_resource_build(const XrXirCompileContext *ctx,XrXirArtifact **output) {
    XrXirInstruction ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0x1f600,{0}},
        {XR_XIR_INTEGER_TO_RUNE,XR_XIR_RUNE,{0},{0},0,{0}},
        {XR_XIR_RUNE_TO_INTEGER,XR_XIR_I64,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    XrXirBlock block={0,4,0,0};
    XrXirFunction fn={"rune",4,NULL,0,XR_XIR_I64,&block,1,ops,4,NULL,0};
    XrXirModule built={XR_XIR_BUILT,&fn,1,NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL,*decoded=NULL,*closed=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};XrXirCSource source={0};
    XrXirStatus status=xr_xir_compile_check(ctx,&built,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(checked,&packet,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(ctx,packet.bytes,packet.length,&decoded,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(decoded,&closed,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_emit_leaf_c(lowered,"rune_resource",1048576,&source);
    xr_xir_compile_c_source_free(&source);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_artifact_free(checked);
    if(status==XR_XIR_OK)*output=lowered;else xr_xir_compile_artifact_free(lowered);
    return status;
}
static void rune_resources(void) {
    const XrXirCompileContext *ctx=source_program_owner(UINT64_C(64)*1024*1024,128000000);
    source_program_compile_attempts=0;XrXirArtifact *artifact=NULL;
    CHECK(rune_resource_build(ctx,&artifact)==XR_XIR_OK && artifact);
    size_t points=source_program_compile_attempts;XrCompileResourceStats exact={0};
    CHECK(points>0 && xr_compile_resources_stats(ctx->resources,&exact)==XR_COMPILE_RESOURCE_OK);
    xr_xir_compile_artifact_free(artifact);source_program_owners_free();
    for(size_t i=0;i<points;++i) {
        ctx=source_program_owner(UINT64_C(64)*1024*1024,128000000);
        source_program_compile_attempts=0;source_program_compile_injected=false;source_program_compile_fail_at=i;artifact=NULL;
        CHECK(rune_resource_build(ctx,&artifact)==XR_XIR_OUT_OF_MEMORY && !artifact);
        CHECK(source_program_compile_injected);source_program_compile_fail_at=SIZE_MAX;
        source_program_owners_free();CHECK(!runtime_live && !runtime_bytes);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned shortfall=0;shortfall<2;++shortfall) {
        ctx=rune_resource_owner(axis==0?exact.allocated_bytes-shortfall:UINT64_C(64)*1024*1024,
            axis==1?exact.peak_bytes-shortfall:UINT64_C(8)*1024*1024,
            axis==2?exact.work-shortfall:128000000);
        artifact=NULL;XrXirStatus status=rune_resource_build(ctx,&artifact);
        CHECK(status==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(shortfall?!artifact:artifact!=NULL);xr_xir_compile_artifact_free(artifact);source_program_owners_free();
    }
    fprintf(stderr,"Rune whole check/packet-read/specialize/lower/leaf-emit: %zu actual compiler OOM points and three exact/minus1 axes; physical=0/0\n",points);
}
static void rune_wire(void) {
    XrXirInstruction ops[]={
        {XR_XIR_CONST_RUNE,XR_XIR_RUNE,{0},{0},0x1f600,{0}},
        {XR_XIR_RUNE_TO_INTEGER,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirBlock block={0,3,0,0};
    XrXirFunction fn={"rune",4,NULL,0,XR_XIR_I64,&block,1,ops,3,NULL,0};
    XrXirModule built={XR_XIR_BUILT,&fn,1,NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirCompileContext *ctx=source_program_owner(UINT64_C(64)*1024*1024,128000000);
    XrXirArtifact *checked=NULL;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_check(ctx,&built,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(rune_checked_golden) && !memcmp(packet.bytes,rune_checked_golden,packet.length));
    XrXirArtifact *decoded=NULL;
    CHECK(xr_xir_compile_checked_read(ctx,rune_checked_golden,sizeof(rune_checked_golden),&decoded,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(decoded);
    CHECK(module && module->functions[0].instructions[0].op==XR_XIR_CONST_RUNE &&
        module->functions[0].instructions[0].type==XR_XIR_RUNE && module->functions[0].instructions[0].immediate==0x1f600);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    /* Instruction 0 begins at 116; its full 64-bit immediate starts at 140.
     * These attacks retain a valid independent field framing and checksum. */
    static const int64_t rejected[]={-1,0xd800,0x110000,INT64_C(0x100000041)};
    for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        uint8_t bad[sizeof(rune_checked_golden)];memcpy(bad,rune_checked_golden,sizeof(bad));
        uint64_t bits=(uint64_t)rejected[i];
        for(unsigned byte=0;byte<8;++byte)bad[140+byte]=(uint8_t)(bits>>(8*byte));
        XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,bad,32);
        xr_sha256_update(&sha,bad+64,sizeof(bad)-64);xr_sha256_final(&sha,bad+32);
        CHECK(xr_xir_compile_checked_read(ctx,bad,sizeof(bad),&decoded,NULL)==XR_XIR_BAD_TYPE && !decoded);
    }
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked);source_program_owners_free();
}
static XrXirRunStatus rune_vm_leaf(void *owner,uint32_t f,XrXirRunContext *context,const XrXirValue *args,uint32_t count,XrXirValue *result) {
    return xr_xir_compile_vm_run(owner,f,context,args,count,result);
}
static void rune_leaf(int argc,char **argv) {
    const XrXirCompileContext *ctx=source_program_owner(UINT64_C(64)*1024*1024,128000000);
    XrXirArtifact *artifact=NULL;CHECK(rune_leaf_build(ctx,&artifact)==XR_XIR_OK);
    rune_leaf_cases(rune_vm_leaf,artifact);
    XrXirCSource source={0};
    CHECK(xr_xir_compile_emit_leaf_c(artifact,"rune_leaf",1048576,&source)==XR_XIR_OK);
    if(argc==2) {
        FILE *file=fopen(argv[1],"wb");CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length);CHECK(!fclose(file));
    }
    xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(artifact);
    source_program_owners_free();CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    _Static_assert(XR_XIR_RUNE==16 && XR_XIR_CONST_RUNE==128 && XR_XIR_GE_STRING==134,"append-only Rune and text wire IDs");
    _Static_assert(XR_XIR_CHECKED_SCHEMA==24 && XR_XIR_CHECKED_CONTRACT==63 && XR_XIR_VALUE_ABI_VERSION==20 && XR_XIR_CALL_ABI_VERSION==25 && XR_XIR_PROGRAM_ABI_VERSION==28,"current admission versions");
    rune_values();rune_text();rune_wire();rune_leaf(argc,argv);rune_execution();rune_resources();return 0;
}
