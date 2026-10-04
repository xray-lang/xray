/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cgen_bytes_owner.c - Actual C byte emission, work and physical failures
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(90); } } while (0)
#include "../xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_emit_c.c"

static void bytes_case(uint32_t count) {
    char *input=malloc(count ? count : 1);CHECK(input);
    for(uint32_t i=0;i<count;++i)input[i]=(char)(i%256);
    XrCompileResourceLimits limits={UINT64_C(1)<<28,UINT64_C(1)<<27,UINT64_C(1)<<30};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    size_t capacity=128+(size_t)count*7;
    char *storage=NULL;CHECK(xr_compile_resources_alloc(resources,capacity,(void **)&storage)==XR_COMPILE_RESOURCE_OK);
    CBuffer buffer={.text=storage,.capacity=capacity,.limit=capacity,.status=XR_XIR_OK,.context=&context};
    XrCompileResourceStats before,after;
    xr_compile_resources_stats(resources,&before);
    emit_bytes(&buffer,input,count);
    CHECK(emit_finalize(&buffer));
    xr_compile_resources_stats(resources,&after);
    /* Each plain byte has one format read and one output write. Format NUL
     * reads remain; each hex digit keeps its conversion and stack read. The
     * complete byte expression receives one actual final NUL write. */
    uint64_t expected=2*38+1+2*7+1+11*((count+15)/16)+1;
    for(uint32_t i=0;i<count;++i)expected+=((unsigned char)input[i]<16 ? 16 : 18);
    CHECK(buffer.status==XR_XIR_OK && after.work-before.work==expected);
    CHECK(buffer.length==45+5*(size_t)count+5*((count+15)/16) && buffer.text[buffer.length]=='\0');
    CHECK(strncmp(buffer.text,"(const char *)(const unsigned char[]){",38)==0);
    const char *cursor=buffer.text+38;
    for(uint32_t i=0;i<count;++i) {
        if(i%16==0) {CHECK(strncmp(cursor,"\n    ",5)==0);cursor+=5;}
        char expected_byte[6];int length=snprintf(expected_byte,sizeof(expected_byte),"0x%02x,",(unsigned char)input[i]);
        CHECK(length==5 && memcmp(cursor,expected_byte,5)==0);cursor+=5;
    }
    CHECK(strcmp(cursor,"\n    0}")==0);
    xr_compile_resources_free(buffer.text);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
    for(unsigned minus=0;minus<2;++minus) {
        resources=NULL;limits.work=expected+2-minus;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        context.resources=resources;storage=NULL;
        CHECK(xr_compile_resources_alloc(resources,capacity,(void **)&storage)==XR_COMPILE_RESOURCE_OK);
        buffer=(CBuffer){.text=storage,.capacity=capacity,.limit=capacity,.status=XR_XIR_OK,.context=&context};
        emit_bytes(&buffer,input,count);
        CHECK(buffer.status==XR_XIR_OK);
        buffer.text[buffer.length]='q';
        CHECK(emit_finalize(&buffer)==!minus);
        CHECK(buffer.status==(minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(buffer.text[buffer.length]==(minus ? 'q' : '\0'));
        xr_compile_resources_free(buffer.text);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes);
    }
    free(input);
}
static void public_output(void) {
    char payload[5014];memset(payload,'x',sizeof(payload));payload[1]=0;
    const XrXirInstruction init[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction main_ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock blocks[]={{0,1,0,0},{0,2,0,0}};
    const XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,blocks,1,init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks+1,1,main_ops,2,NULL,0}};
    const XrXirSourceModule source={"bytes",5,NULL,0,0};
    const XrXirFunctionIdentity identities[2]={{0},{0}};
    const XrXirLiteral literals[]={{payload,sizeof(payload)},{"",0}};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,literals,2,0,1,NULL};
    const XrXirModule built={XR_XIR_BUILT,functions,2,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_check(&context,&built,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);
    xr_compile_resources_release(resources);
    size_t baseline=runtime_bytes;
    XrXirCSource output={0};runtime_attempts=0;
    CHECK(xr_xir_compile_emit_c(lowered,"bytes",1<<20,&output)==XR_XIR_OK);
    size_t required=output.length+1,sites=runtime_attempts;
    CHECK(output.text[output.length]=='\0');
    CHECK(strstr(output.text,"0x78,0x00,0x78,") && strstr(output.text,"(const char *)(const unsigned char[]){"));
    xr_xir_compile_c_source_free(&output);CHECK(runtime_bytes==baseline);
    CHECK(xr_xir_compile_emit_c(lowered,"bytes",required,&output)==XR_XIR_OK);
    CHECK(output.length+1==required && output.text[output.length]=='\0');xr_xir_compile_c_source_free(&output);
    CHECK(xr_xir_compile_emit_c(lowered,"bytes",required-1,&output)==XR_XIR_BUDGET);
    CHECK(!output.text && !output.length && runtime_bytes==baseline);
    char sentinel='q';output=(XrXirCSource){&sentinel,1};
    CHECK(xr_xir_compile_emit_c(lowered,"bytes",required,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output.text==&sentinel && output.length==1 && sentinel=='q');output=(XrXirCSource){0};
    for(size_t fault=0;fault<sites;++fault) {
        runtime_attempts=0;runtime_fail_at=fault;
        CHECK(xr_xir_compile_emit_c(lowered,"bytes",1<<20,&output)==XR_XIR_OUT_OF_MEMORY);
        runtime_fail_at=SIZE_MAX;
        CHECK(!output.text && !output.length && runtime_bytes==baseline);
    }
    xr_xir_compile_artifact_free(lowered);CHECK(!runtime_live && !runtime_bytes);
    printf("Public emit: exact/minus-one byte limit, preserved output, %zu OOM points PASS\n",sites);
}
static void allocation_failures(void) {
    char bytes[5014];memset(bytes,0xab,sizeof(bytes));
    size_t sites=0;
    for(size_t fault=SIZE_MAX;;) {
        XrCompileResourceLimits limits={UINT64_C(1)<<28,UINT64_C(1)<<27,UINT64_C(1)<<30};
        XrCompileResources *resources=NULL;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
        CBuffer buffer={0};buffer.context=&context;buffer.limit=1<<20;
        runtime_attempts=0;runtime_fail_at=fault;
        emit_bytes(&buffer,bytes,sizeof(bytes));(void)emit_finalize(&buffer);runtime_fail_at=SIZE_MAX;
        if(fault==SIZE_MAX) {CHECK(buffer.status==XR_XIR_OK);sites=runtime_attempts;}
        else CHECK(buffer.status==XR_XIR_OUT_OF_MEMORY);
        xr_compile_resources_free(buffer.text);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes);
        if(fault==SIZE_MAX)fault=0;else if(++fault==sites)break;
    }
    CHECK(sites>1);printf("C byte emission: %zu actual allocation failures, physical zero PASS\n",sites);
}
int main(void) {
    bytes_case(0);bytes_case(1);bytes_case(256);bytes_case(4095);bytes_case(4096);bytes_case(5014);
    allocation_failures();
    public_output();
    puts("C byte emission: fixed work formula, exact/minus-one, empty/NUL/all byte values PASS");
    return 0;
}
