/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_type_wire_cases.h - Independent current Atomic type packet and preserved 12/7 roles
 */
#ifndef XIR_ATOMIC_TYPE_WIRE_CASES_H
#define XIR_ATOMIC_TYPE_WIRE_CASES_H
static const uint8_t legacy_atomic_types_golden[]={
    0x58,0x52,0x43,0x48,0x4b,0x00,0x00,0x00,0x19,0x00,0x00,0x00,
    0x40,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xa9,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x8f,0x51,0xa1,0x74,
    0x17,0xc9,0x43,0x93,0x4a,0x13,0x09,0x58,0x99,0x25,0x97,0x0f,
    0xcc,0x94,0x12,0x1e,0x4a,0xdd,0xfc,0x77,0x9b,0x1f,0x36,0x9e,
    0xe0,0x74,0x9f,0x7a,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x74,0x79,0x70,0x65,
    0x73,0x03,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x01,0x01,0x00,
    0x00,0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x21,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x07,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x07,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x0d,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
};
#include "xir_atomic_types65_golden.h"
typedef struct LegacyTypesFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;XrXirType parameters[3];
    XrXirInstruction instruction;XrXirBlock block;XrXirFunction function;XrXirModule module;
} LegacyTypesFixture;
static void legacy_types_fixture(LegacyTypesFixture *f){
    *f=(LegacyTypesFixture){0};const XrXirType elements[]={XR_XIR_I64,XR_XIR_BOOL,XR_XIR_F64};
    for(unsigned i=0;i<3;++i){f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=elements[i]};f->parameters[i]=(XrXirType)(256+i);}
    f->types=(XrXirTypes){f->nodes,3,NULL,NULL};f->instruction=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    f->block=(XrXirBlock){.count=1};f->function=(XrXirFunction){.name="types",.name_length=5,.parameters=f->parameters,.parameter_count=3,
        .result=XR_XIR_UNIT,.blocks=&f->block,.block_count=1,.instructions=&f->instruction,.instruction_count=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=&f->function,.function_count=1,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
static XrXirStatus legacy_types_pipeline(const XrXirCompileContext *context,void *unused){
    (void)unused;LegacyTypesFixture fixture;legacy_types_fixture(&fixture);XrXirArtifact *checked=NULL,*read=NULL,*specialized=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_check(context,&fixture.module,&checked,NULL);if(status!=XR_XIR_OK)CHECK(!checked);
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_write(checked,&packet,NULL);if(status!=XR_XIR_OK)CHECK(!packet.bytes&&!packet.length);
        else CHECK(packet.length==sizeof(atomic_types65_golden)&&!memcmp(packet.bytes,atomic_types65_golden,packet.length));}
    xr_xir_compile_artifact_free(checked);
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);if(status!=XR_XIR_OK)CHECK(!read);}
    if(status==XR_XIR_OK){status=xr_xir_compile_specialize(read,&specialized,NULL);if(status!=XR_XIR_OK)CHECK(!specialized);}
    xr_xir_compile_artifact_free(read);
    if(status==XR_XIR_OK){status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);if(status!=XR_XIR_OK)CHECK(!lowered);}
    xr_xir_compile_artifact_free(specialized);if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(lowered,NULL);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&packet);return status;
}
static void legacy_types_cases(void){
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    size_t previous_attempts=source_program_compile_attempts;
    XrXirArtifact *previous=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,legacy_atomic_types_golden,sizeof(legacy_atomic_types_golden),&previous,NULL)==XR_XIR_BAD_STRUCTURE && !previous);
    previous=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&owner.context,legacy_atomic_types_golden,sizeof(legacy_atomic_types_golden),&previous,NULL)==XR_XIR_BAD_STRUCTURE && previous==(XrXirArtifact *)(uintptr_t)1);
    CHECK(source_program_compile_attempts==previous_attempts);
    for(unsigned mutation=0;mutation<12;++mutation){
        LegacyTypesFixture f;legacy_types_fixture(&f);XrXirCallableParameter field={XR_XIR_I64,0};
        switch(mutation){case 0:f.nodes[0].element=XR_XIR_UNIT;break;case 1:f.nodes[0].element=XR_XIR_STRING;break;
        case 2:f.nodes[0].element=(XrXirType)91;break;case 3:f.nodes[0].nominal.declaration=1;break;
        case 4:f.nodes[0].flags=1;break;case 5:f.nodes[0].result=XR_XIR_I64;break;case 6:f.nodes[0].parameter_span=1;break;
        case 7:f.nodes[0].nominal.argument_count=1;f.nodes[0].nominal.arguments=&field.type;break;
        case 8:f.nodes[0].parameters=&field;f.nodes[0].parameter_count=1;break;
        case 9:f.nodes[0].nominal.field_count=1;f.nodes[0].nominal.fields=&field.type;break;
        case 10:f.nodes[0].element=(XrXirType)UINT16_MAX;break;default:f.nodes[0].kind=XR_XIR_TYPE_CALLABLE;break;}
        XrXirArtifact *out=NULL;XrCompileResourceStats baseline=library_compile_stats(&owner.context);
        XrXirStatus status=xr_xir_compile_check(&owner.context,&f.module,&out,NULL);
        XrXirStatus expected=mutation<=2||mutation==6||mutation>=10?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
        if(status!=expected)fprintf(stderr,"typeconstructor%u status%u expected%u\n",mutation,status,expected);
        CHECK(status==expected && !out);
        CHECK(library_compile_stats(&owner.context).live_bytes==baseline.live_bytes);
    }
    LegacyTypesFixture f;legacy_types_fixture(&f);XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_check(&owner.context,&f.module,&checked,NULL)==XR_XIR_OK);
    uint8_t bytes[sizeof(atomic_types65_golden)];
    /* Explicit body fields: four-byte kind/span/element nodes start at 189. */
    for(unsigned mutation=0;mutation<7;++mutation){memcpy(bytes,atomic_types65_golden,sizeof(bytes));
        size_t offset=mutation==2?193:mutation==3?189:mutation==4?189:mutation==5?201:197;
        uint32_t value=mutation==0?0:mutation==1?3:mutation==2?1:mutation==3?8:mutation==4?0:mutation==5?8:258;
        for(unsigned b=0;b<4;++b)bytes[offset+b]=(uint8_t)(value>>(8*b));ordering_packet_digest(bytes,sizeof(bytes));
        XrCompileResourceStats baseline=library_compile_stats(&owner.context);XrXirArtifact *out=NULL,*occupied=checked;
        XrXirStatus status=xr_xir_compile_checked_read(&owner.context,bytes,sizeof(bytes),&out,NULL);
        if(status!=XR_XIR_BAD_TYPE&&status!=XR_XIR_BAD_STRUCTURE)fprintf(stderr,"typewire%u status%u\n",mutation,status);
        CHECK(status==XR_XIR_BAD_TYPE&&!out);
        CHECK(xr_xir_compile_checked_read(&owner.context,bytes,sizeof(bytes),&occupied,NULL)==status&&occupied==checked);
        CHECK(library_compile_stats(&owner.context).live_bytes==baseline.live_bytes);
    }
    xr_xir_compile_artifact_free(checked);library_compile_owner_drop(&owner);
    library_compile_operation_cases("legacy Atomic3 type writer/reader",legacy_types_pipeline,NULL);
    fprintf(stderr,"legacy type12/wire7 mapped roles PASS; retired copy/ownership bytes preserved outside current schema\n");
}

#endif
