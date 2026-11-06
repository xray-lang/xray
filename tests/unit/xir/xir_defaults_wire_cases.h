/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_defaults_wire_cases.h - Owned defaults and malformed external input
 */
#ifndef XIR_DEFAULTS_WIRE_CASES_H
#define XIR_DEFAULTS_WIRE_CASES_H
#include "xir_defaults_invoke_golden.h"
static void defaults_wire_ownership(void) {
    size_t live=runtime_live,physical=runtime_bytes;
    XrXirArtifact *producer=defaults_owned_fixture(false),*copy=NULL;
    const XrXirModule *original=xr_xir_compile_artifact_module(producer);
    CHECK(original->defaults&&original->defaults->count==1);
    CHECK(original->defaults->records[0].function==0&&original->defaults->records[0].owner==2);
    CHECK(xr_xir_compile_recheck_v2(library_context,original,xr_xir_compile_artifact_construction(producer),&copy,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(copy)->defaults!=original->defaults);
    CHECK(xr_xir_compile_artifact_module(copy)->defaults->records!=original->defaults->records);
    xr_xir_compile_artifact_free(producer);CHECK(xr_xir_compile_artifact_verify(copy,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(copy)->functions[0].instructions[0].immediate==41);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(copy,&packet,NULL)==XR_XIR_OK);
    LibraryPacketFixture fixture={packet.bytes,packet.length,NULL};
    library_compile_operation_cases("defaults owned reader/writer",library_packet_operation,&fixture);
    fixture=(LibraryPacketFixture){NULL,0,copy};
    library_compile_operation_cases("defaults owned clone",library_packet_operation,&fixture);
    XrXirModule bad=*xr_xir_compile_artifact_module(copy);XrXirDefaultTable empty={0};bad.defaults=&empty;
    XrXirArtifact *rejected=NULL;
    CHECK(xr_xir_compile_recheck_v2(library_context,&bad,xr_xir_compile_artifact_construction(copy),&rejected,NULL,NULL)==XR_XIR_BAD_STRUCTURE&&!rejected);
    xr_xir_compile_artifact_free(copy);
    XrXirArtifact *read=NULL;CHECK(xr_xir_compile_checked_read(library_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    const XrXirDefaultBinding *binding=&xr_xir_compile_artifact_module(read)->defaults->records[0];
    CHECK(binding->owner_kind==XR_XIR_DEFAULT_PARAMETER&&binding->owner==2&&!binding->ordinal&&!binding->function);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&packet);
    CHECK(runtime_live==live&&runtime_bytes==physical);
}
static void defaults_wire_failure_prefix(void) {
    size_t live=runtime_live,physical=runtime_bytes,blocks=source_live,bytes=source_bytes;
    XrXirArtifact *source=defaults_owned_fixture(false),*artifact=NULL;
    CHECK(!xr_xir_compile_artifact_module(source)->provenance);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);
    uint8_t binding[20]={0};string_fixture_u32(binding,1);
    string_fixture_u32(binding+4,XR_XIR_DEFAULT_PARAMETER);string_fixture_u32(binding+8,2);
    /* With no provenance, the default table is the final declaration record,
     * followed only by the absent-evidence tag. Other fields can contain the
     * same integers, so a byte-pattern search alone cannot locate this table. */
    CHECK(packet.length>=64+sizeof(binding)+4);
    size_t start=packet.length-sizeof(binding)-4;
    CHECK(!memcmp(packet.bytes+start,binding,sizeof(binding)));
    CHECK(!memcmp(packet.bytes+packet.length-4,(uint8_t[4]){0},4));
    uint8_t *bad=xr_malloc(packet.length);CHECK(bad);
    for(unsigned field=0;field<5;++field){
        memcpy(bad,packet.bytes,packet.length);memset(bad+start+field*4,255,4);string_catalog_rehash(bad,packet.length);
        CHECK(xr_xir_compile_checked_read(library_context,bad,packet.length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE&&!artifact);
        artifact=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(library_context,bad,packet.length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(artifact==(XrXirArtifact *)(uintptr_t)1);artifact=NULL;
    }
    for(size_t length=start;length<packet.length;++length){
        memcpy(bad,packet.bytes,packet.length);
        uint64_t payload=length-64;for(unsigned b=0;b<8;++b)bad[24+b]=(uint8_t)(payload>>(8*b));
        string_catalog_rehash(bad,length);
        CHECK(xr_xir_compile_checked_read(library_context,bad,length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE&&!artifact);
        CHECK(runtime_live==live&&runtime_bytes==physical);
    }
    xr_free(bad);xr_xir_compile_checked_packet_free(&packet);
    CHECK(source_live==blocks&&source_bytes==bytes&&runtime_live==live&&runtime_bytes==physical);
    puts("Current defaults count/owner/ordinal/helper attacks, truncations and unchanged outputs PASS");
}
static void defaults_wire_cases(void) {
    defaults_invoke_golden_cases();defaults_wire_ownership();defaults_wire_failure_prefix();
}
#endif // XIR_DEFAULTS_WIRE_CASES_H
