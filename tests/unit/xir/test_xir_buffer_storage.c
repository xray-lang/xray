/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_buffer_storage.c - Real private storage through owned Checked code
 *
 * KEY CONCEPT:
 *   Independent payload, permission and retirement checks use actual values.
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_buffer_storage.h"
#include "xir/xxir_nominal.h"
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"buffer %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_construction_fixture.h"
#include "xir_buffer_class_cases.h"
#include "xir_buffer_domain_cases.h"
static void payload_cases(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirDomainStats frame=xr_xir_domain_stats(domain);
    const int64_t sizes[]={0,1,257};
    for (unsigned mode=0;mode<3;++mode) for(unsigned n=0;n<3;++n) {
        XrXirValue storage={0},copy={0};
        CHECK(xr_xir_buffer_storage_new(domain,sizes[n],mode==2?64:0,mode==1,&storage)==XR_XIR_VALUE_OK);
        CHECK(storage.type==XR_XIR_BUFFER_STORAGE && xr_xir_value_valid(&storage));
        int64_t length=-1;CHECK(xr_xir_buffer_storage_length(&storage,&length)==XR_XIR_VALUE_OK && length==sizes[n]);
        const void *bytes=NULL;size_t observed=SIZE_MAX,alignment=SIZE_MAX;
        CHECK(xr_xir_buffer_storage_observe(&storage,&bytes,&observed,&alignment) && observed==(size_t)sizes[n]);
        CHECK(alignment==(mode==2?64u:0u) && (!bytes || mode!=2 || !((uintptr_t)bytes%64)));
        if(mode==1)for(size_t i=0;i<observed;++i)CHECK(!((const unsigned char *)bytes)[i]);
        CHECK(xr_xir_value_copy(&storage,&copy)==XR_XIR_VALUE_OK);xr_xir_value_drop(&storage);
        CHECK(xr_xir_buffer_storage_length(&copy,&length)==XR_XIR_VALUE_OK && length==sizes[n]);xr_xir_value_drop(&copy);
        CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    }
    XrXirValue output={0};const int64_t invalid[][2]={{-1,0},{1,-1},{1,1},{1,7},{1,24},{INT64_MAX,0}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        XrXirValueStatus status=xr_xir_buffer_storage_new(domain,invalid[i][0],invalid[i][1],false,&output);
        CHECK(status==(i==5?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_BAD_ARGUMENT) && !output.type && !output.payload);
        CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    }
    int64_t sentinel=91;XrXirValue number={XR_XIR_I64,0,4};
    CHECK(xr_xir_buffer_storage_length(&number,&sentinel)==XR_XIR_VALUE_BAD_ARGUMENT && sentinel==91);
    XrXirValue text={0};CHECK(xr_xir_string_new(domain,"not storage",11,&text)==XR_XIR_VALUE_OK);
    XrXirValue forged=text;forged.type=XR_XIR_BUFFER_STORAGE;
    CHECK(!xr_xir_value_valid(&forged));
    CHECK(xr_xir_buffer_storage_length(&forged,&sentinel)==XR_XIR_VALUE_BAD_ARGUMENT && sentinel==91);
    xr_xir_value_drop(&text);
    CHECK(xr_xir_buffer_storage_new(domain,13,0,true,&output)==XR_XIR_VALUE_OK);
    xr_xir_domain_drop(domain);
    CHECK(xr_xir_buffer_storage_length(&output,&sentinel)==XR_XIR_VALUE_OK && sentinel==13);xr_xir_value_drop(&output);
}
static XrXirArtifact *fixture(const XrXirCompileContext *context,bool attacks) {
    const XrXirInstruction done={.op=XR_XIR_RETURN};const XrXirBlock init={0,1,0,0};
    XrXirInstruction code[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=8},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={0,1},.immediate=2},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={1,1},.immediate=3},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=64},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={2,2},.immediate=4},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={4,1},.immediate=5},
        {.op=XR_XIR_RETURN,.args={5,0}}};
    const uint32_t operands[]={0,0,0,3,4};const XrXirBlock main={0,7,0,0};
    XrXirInstruction make[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=13},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={0,1},.immediate=3},
        {.op=XR_XIR_RETURN,.args={1,0}}};const uint32_t one_operand=0;const XrXirBlock maker={0,3,0,0};
    XrXirInstruction bad_zero[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=0},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=0},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={0,2},.immediate=4},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction bad_nonzero[4];memcpy(bad_nonzero,bad_zero,sizeof(bad_zero));bad_nonzero[0].immediate=8;
    const uint32_t invalid_operands[]={0,1};const XrXirBlock invalid_block={0,4,0,0};
    const XrXirType box_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirInstruction box_code[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=13},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={0,1},.immediate=3},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=17},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={1,1},.immediate=2},
        {.op=XR_XIR_CLASS_NEW,.type=box_type,.args={2,2}},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=21},
        {.op=XR_XIR_CALL,.type=XR_XIR_BUFFER_STORAGE,.args={4,1},.immediate=2},
        {.op=XR_XIR_CLASS_SET,.args={4,6},.immediate=0},
        {.op=XR_XIR_RETURN,.args={4,0}}};
    const uint32_t box_operands[]={0,2,1,3,5};const XrXirBlock box_block={0,9,0,0};
    const XrXirInstruction box_length[]={
        {.op=XR_XIR_CALL,.type=box_type,.immediate=9},
        {.op=XR_XIR_CLASS_GET,.type=XR_XIR_BUFFER_STORAGE,.args={0,0}},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=5},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    const uint32_t length_operand=1;
    const XrXirType integer=XR_XIR_I64,aligned[]={XR_XIR_I64,XR_XIR_I64},storage=XR_XIR_BUFFER_STORAGE;
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&init,1,&done,1,NULL,0,XR_XIR_NEVER},
        {"main",4,NULL,0,XR_XIR_I64,&main,1,code,7,operands,5,XR_XIR_NEVER},
        {"allocate",8,&integer,1,XR_XIR_BUFFER_STORAGE,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER},
        {"zeroed",6,&integer,1,XR_XIR_BUFFER_STORAGE,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER},
        {"aligned",7,aligned,2,XR_XIR_BUFFER_STORAGE,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER},
        {"length",6,&storage,1,XR_XIR_I64,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER},
        {"make",4,NULL,0,XR_XIR_BUFFER_STORAGE,&maker,1,make,3,&one_operand,1,XR_XIR_NEVER},
        {"bad0",4,NULL,0,XR_XIR_BUFFER_STORAGE,&invalid_block,1,bad_zero,4,invalid_operands,2,XR_XIR_NEVER},
        {"bad8",4,NULL,0,XR_XIR_BUFFER_STORAGE,&invalid_block,1,bad_nonzero,4,invalid_operands,2,XR_XIR_NEVER},
        {"box",3,NULL,0,box_type,&box_block,1,box_code,9,box_operands,5,XR_XIR_NEVER},
        {"boxLength",9,NULL,0,XR_XIR_I64,&invalid_block,1,box_length,4,&length_operand,1,XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[11]={{0}};
    for(uint32_t f=2;f<6;++f){identities[f].native_callable=f+2;identities[f].promises=XR_XIR_FUNCTION_NO_SUSPEND;}
    identities[6].exported=1;identities[7].exported=1;identities[8].exported=1;
    identities[9].exported=1;identities[10].exported=1;
    const char identity[]="stdlib-module-v1:module=3:mem:path=10:mem/mem.xr";
    XrXirSourceModule source={.name=identity,.name_length=sizeof(identity)-1,.initializer=0};
    XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirNominalField fields[]={{{"current",7},XR_XIR_BUFFER_STORAGE,XR_XIR_FIELD_MUTABLE},
        {{"keep",4},XR_XIR_BUFFER_STORAGE,0}};
    XrXirNominalDeclaration nominal={.module={identity,sizeof(identity)-1},.name={"BufferBox",9},
        .fields=fields,.field_count=2,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable nominals={&nominal,1,NULL};
    const XrXirType field_types[]={XR_XIR_BUFFER_STORAGE,XR_XIR_BUFFER_STORAGE};
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,field_types,2}};
    XrXirTypes types={&node,1,&nominals,NULL};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=11,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(context,&module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"actual check status=%u f=%u b=%u i=%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    if(attacks) {
        source.name="mem";source.name_length=3;nominal.module=(XrXirLiteral){"mem",3};
        CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_TYPE);
        source.name=identity;source.name_length=sizeof(identity)-1;nominal.module=(XrXirLiteral){identity,sizeof(identity)-1};
        identities[2].exported=1;CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_STRUCTURE);identities[2].exported=0;
        identities[2].native_callable=XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY;
        CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_TYPE);identities[2].native_callable=XR_NATIVE_CALLABLE_BUFFER_ALLOCATE;
        XrXirOp original=code[1].op;code[1].op=XR_XIR_CLASS_NEW;CHECK(xir_fixture_verify(context,&module,NULL)!=XR_XIR_OK);code[1].op=original;
        functions[2].result=XR_XIR_I64;CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_TYPE);functions[2].result=XR_XIR_BUFFER_STORAGE;
        fields[0].flags=0;nominal.flags|=XR_XIR_NOMINAL_SENDABLE;
        CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_BAD_TYPE);
        nominal.flags=XR_XIR_NOMINAL_FINAL;fields[0].flags=XR_XIR_FIELD_MUTABLE;
        XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
        CHECK(xr_xir_compile_type_markers_prove(context,&proof,XR_XIR_BUFFER_STORAGE,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_BAD_TYPE);
        nominal.kind=XR_XIR_NOMINAL_STRUCT;nominal.flags=0;fields[0].flags=0;
        CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
        const XrXirNominalVariant variant={{"Pair",4},0,2};
        nominal.kind=XR_XIR_NOMINAL_ENUM;nominal.variants=&variant;nominal.variant_count=1;
        CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
        nominal.kind=XR_XIR_NOMINAL_CLASS;nominal.flags=XR_XIR_NOMINAL_FINAL;nominal.variants=NULL;nominal.variant_count=0;fields[0].flags=XR_XIR_FIELD_MUTABLE;
        CHECK(xir_fixture_verify(context,&module,NULL)==XR_XIR_OK);
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    XrXirArtifact *received=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    XrXirArtifact *closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_specialize(received,&closed,NULL,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(received);xr_xir_compile_artifact_free(closed);return lowered;
}
int main(int argc,char **argv) {
    payload_cases();XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    if(argc==3 && (!strcmp(argv[1],"--previous") || !strcmp(argv[1],"--previous109"))) {
        FILE *file=fopen(argv[2],"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);
        CHECK(size>64 && !fseek(file,0,SEEK_SET));unsigned char *bytes=malloc((size_t)size);CHECK(bytes);
        CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
        uint32_t previous=!strcmp(argv[1],"--previous109")?109u:108u;
        CHECK(bytes[8]==39 && bytes[12]==previous && !bytes[13] && !bytes[14] && !bytes[15]);
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *output=NULL;CHECK(xr_xir_compile_checked_read(&context,bytes,(size_t)size,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(xr_xir_compile_checked_read(&context,bytes,64,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
        free(bytes);xr_compile_resources_release(context.resources);printf("Genuine previous common%u full Source packet and prefix rejected before publication PASS\n",previous);return 0;
    }
    XrXirArtifact *lowered=fixture(&context,true);
    if(argc==3 && !strcmp(argv[1],"--emit")) {
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"buffer_storage",1048576,&source)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
        xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(lowered);
    } else {
        CHECK(argc==1);buffer_domain_cases(&context);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED && result.payload==8);xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_start(instance,6,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED && result.type==XR_XIR_BUFFER_STORAGE);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        for(uint32_t f=7;f<9;++f) {
            instance=NULL;
            CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_start(instance,f,NULL,0)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_BAD_STATE);
            XrXirValue rejected={0};
            CHECK(xr_xir_instance_take_result(instance,&rejected)==XR_XIR_CALL_BAD_STATE && !rejected.type && !rejected.reserved && !rejected.payload);
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_STATE);
        }
        buffer_class_cases(program);
        int64_t size=0;CHECK(xr_xir_buffer_storage_length(&result,&size)==XR_XIR_VALUE_OK && size==13);xr_xir_value_drop(&result);
    }
    xr_compile_resources_release(context.resources);puts("Private buffer storage: real owned bytes and checked transport PASS");return 0;
}
