/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_defaults_invoke_golden.h - Default owner and error projections across input decoding
 */
#ifndef XIR_DEFAULTS_INVOKE_GOLDEN_H
#define XIR_DEFAULTS_INVOKE_GOLDEN_H
#include "xir_defaults_owned_fixture.h"
static void defaults_invoke_golden_cases(void) {
    size_t live=runtime_live,physical=runtime_bytes;
    XrXirArtifact *producer=defaults_owned_fixture(true),*owned=NULL;
    const XrXirModule *module=xr_xir_compile_artifact_module(producer);
    CHECK(module->linkage_kind==XR_XIR_PROGRAM&&module->function_count==4);
    CHECK(module->declarations->entry_function==3&&module->declarations->modules[0].initializer==1);
    CHECK(module->functions[3].operand_count==0&&module->functions[3].instructions[0].args[0]==2);
    CHECK(xr_xir_compile_recheck_v2(library_context,module,xr_xir_compile_artifact_construction(producer),&owned,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(producer);producer=NULL;
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(owned);
    CHECK(xr_xir_compile_checked_read(library_context,packet.bytes,packet.length,&producer,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(producer)->functions[3].instructions[0].op==XR_XIR_INVOKE_DEFAULT);
    xr_xir_compile_artifact_free(producer);producer=NULL;
    uint8_t record[40]={0};string_fixture_u32(record,XR_XIR_INVOKE_DEFAULT);
    string_fixture_u32(record+4,XR_XIR_I64);string_fixture_u32(record+8,2);
    string_fixture_u32(record+16,1);string_fixture_u32(record+20,2);
    size_t start=string_fixture_find(&packet,record,sizeof(record));
    const struct {size_t offset;uint32_t value;} attacks[]={
        {8,4},{8,1},{12,1},{24,1},{20,1},
        {40,XR_XIR_CONST_INT},{48,1},{40,XR_XIR_INVOKE_ERROR},{44,XR_XIR_ERROR},
        {120,XR_XIR_CONST_INT},{128,1},{120,XR_XIR_INVOKE_RESULT},{124,XR_XIR_I64},{8,0}};
    uint8_t *bad=xr_malloc(packet.length);CHECK(bad);
    for(size_t i=0;i<XR_COUNTOF(attacks);++i){
        memcpy(bad,packet.bytes,packet.length);string_fixture_u32(bad+start+attacks[i].offset,attacks[i].value);
        string_catalog_rehash(bad,packet.length);
        XrXirStatus expected=i==9?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=xr_xir_compile_checked_read(library_context,bad,packet.length,&producer,NULL);
        if(status!=expected)fprintf(stderr,"invoke mutation%zu status%u expected%u\n",i,status,expected);
        CHECK(status==expected&&!producer&&runtime_live==live&&runtime_bytes==physical);
    }
    memcpy(bad,packet.bytes,packet.length);
    string_fixture_u32(bad+start+16,2);string_fixture_u32(bad+start+20,1);
    string_catalog_rehash(bad,packet.length);
    CHECK(xr_xir_compile_checked_read(library_context,bad,packet.length,&producer,NULL)==XR_XIR_BAD_STRUCTURE&&!producer);
    xr_free(bad);xr_xir_compile_checked_packet_free(&packet);
    CHECK(runtime_live==live&&runtime_bytes==physical);
    puts("Current default invoke owner/ordinal, SSA/result/error projections and successor attacks PASS");
}
#endif // XIR_DEFAULTS_INVOKE_GOLDEN_H
