/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_record_wire.c - Independently framed record graphs and real consumers
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_record.h"
#include "xir/xxir_type_arena.h"
#include "../xir_record80_golden.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_source_fixture_owner.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA==32 && XR_XIR_CHECKED_CONTRACT==80 &&
    XR_XIR_VALUE_ABI_VERSION==26 && XR_XIR_PROGRAM_ABI_VERSION==35,"independent record identity");
_Static_assert(XR_XIR_RECORD_NEW==154 && XR_XIR_RECORD_FIELD==155 && XR_XIR_RECORD_SET==156,
    "independent record operation identities");
static XrXirArtifact *record_wire_lower(SourceFixtureOwner *owner,const uint8_t *bytes,size_t size) {
    XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(&owner->context,bytes,size,&read,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"record wire read=%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && read);
    XrXirCheckedPacket encoded={0};CHECK(xr_xir_compile_checked_write(read,&encoded,NULL)==XR_XIR_OK);
    CHECK(encoded.length==size && !memcmp(encoded.bytes,bytes,size));xr_xir_compile_checked_packet_free(&encoded);
    status=xr_xir_compile_specialize(read,&closed,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"record wire specialize=%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && closed);xr_xir_compile_artifact_free(read);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,&diagnostic)==XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(closed);return lowered;
}
static void record_wire_run(XrXirArtifact *lowered,unsigned family,int64_t expected) {
    XrXirProgram *program=NULL;XrXirInstance *instance=NULL;XrXirInstanceConfig config;XrXirValue result={0};
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,family?2u:1u,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    if (!family) CHECK(result.type==XR_XIR_I64 && result.payload==expected);
    else {
        const XrXirTypeArena *arena=xr_xir_value_arena(&result);
        const XrXirTypeNode *type=xr_xir_record_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)result.type);
        CHECK(type && type->parameter_count==(family==1?0u:3u) && result.payload);
        if (family==2) {
            XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
            XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=1000000,.scratch_bytes=262144};
            XrXirValue value={0};CHECK(xr_xir_record_get(&result,0,&admission,&value)==XR_XIR_VALUE_OK);
            CHECK(value.type==XR_XIR_BOOL && value.payload==1);xr_xir_value_drop(&value);
            CHECK(xr_xir_record_get(&result,1,&admission,&value)==XR_XIR_VALUE_OK && !value.type && !value.payload);
            CHECK(xr_xir_record_get(&result,2,&admission,&value)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==7);
            xr_xir_value_drop(&value);xr_xir_domain_drop(domain);
        }
    }
    xr_xir_value_drop(&result);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==3 && !strcmp(argv[1],"--emit")));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    if (argc==3) {
        XrXirArtifact *lowered=record_wire_lower(&owner,record80_mutate,sizeof(record80_mutate));
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"language_reference",1048576,&source)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
        xr_xir_compile_c_source_free(&source);record_wire_run(lowered,0,9);
    } else {
        const struct {const uint8_t *bytes;size_t size;unsigned family;int64_t expected;} accepted[]={
            {record80_empty,sizeof(record80_empty),1,0},{record80_unit_fields,sizeof(record80_unit_fields),2,0},
            {record80_generic,sizeof(record80_generic),0,7},{record80_mutate,sizeof(record80_mutate),0,9}};
        for (size_t i=0;i<sizeof(accepted)/sizeof(*accepted);++i)
            record_wire_run(record_wire_lower(&owner,accepted[i].bytes,accepted[i].size),accepted[i].family,accepted[i].expected);
        const struct {const uint8_t *bytes;size_t size;} rejected[]={
            {record80_duplicate,sizeof(record80_duplicate)},{record80_unsorted,sizeof(record80_unsorted)},
            {record80_invalid_name,sizeof(record80_invalid_name)}};
        for (size_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
            XrXirArtifact *output=NULL;CHECK(xr_xir_compile_checked_read(&owner.context,rejected[i].bytes,rejected[i].size,&output,NULL)==XR_XIR_BAD_STRUCTURE);
            CHECK(!output);
        }
    }
    source_fixture_owner_free(&owner);puts("Independent record packets, lifetime and real mutation PASS");return 0;
}
