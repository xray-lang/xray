/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_owner_tuple_cases.h - Full nominal and method default authority
 *
 * KEY CONCEPT:
 *   Phantom equal-layout parameters remain distinct provenance obligations.
 */
#ifndef XIR_DEFAULT_OWNER_TUPLE_CASES_H
#define XIR_DEFAULT_OWNER_TUPLE_CASES_H
#include "xir_construction_fixture.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "base/xsha256.h"
static void default_tuple_u32(uint8_t *bytes,uint32_t n) {
    for(unsigned i=0;i<4;++i) bytes[i]=(uint8_t)(n>>(i*8));
}
static void default_tuple_case(bool constructor,unsigned mode) {
    XrXirType symbolic[]={XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_TYPE_PARAMETER_BASE+1};
    XrXirType actual[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_BOOL,XR_XIR_F64,XR_XIR_I64,XR_XIR_STRING,XR_XIR_BOOL,XR_XIR_F64};
    XrXirConstraint parent[2]={{XR_XIR_CONSTRAINT_SENDABLE,NULL,0},{0}};
    XrXirConstraint owner[4]={{XR_XIR_CONSTRAINT_SENDABLE,NULL,0},{0},{XR_XIR_CONSTRAINT_SENDABLE,NULL,0},{0}};
    XrXirConstraint helper[4];memcpy(helper,owner,sizeof(owner));
    XrXirNominalDeclaration nominal={.module={"root",4},.name={"Box",3},.constraints=parent,.parameter_count=2,.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable nt={&nominal,1,NULL};
    XrXirTypeNode nodes[2]={
        {.kind=XR_XIR_TYPE_NOMINAL,.parameter_span=2,.nominal={0,symbolic,2,NULL,0}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,actual,2,NULL,0}}};
    XrXirTypes types={nodes,2,&nt,NULL};
    XrXirType parameters[]={XR_XIR_CONSTRUCTED_TYPE_BASE,XR_XIR_I64};
    XrXirInstruction ret={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction owner_ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    XrXirInstruction helper_ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction entry[]={
        {XR_XIR_STRUCT_NEW,XR_XIR_CONSTRUCTED_TYPE_BASE+1,{0},{0},0,{0}},
        {XR_XIR_CALL_DEFAULT,XR_XIR_I64,{0},{2,1},0,{0,4}},
        {XR_XIR_CALL,XR_XIR_I64,{0,2},{0},2,{4,4}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    uint32_t operands[]={0,1};
    XrXirBlock one={0,1,0,0},two={0,2,0,0},four={0,4,0,0};
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&four,1,entry,4,operands,2},
        {"owner",5,parameters,2,XR_XIR_I64,&two,1,owner_ops,2,NULL,0},
        {"default",7,NULL,0,XR_XIR_I64,&two,1,helper_ops,2,NULL,0}};
    XrXirFunctionIdentity ids[4]={{0},{0},{.nominal_owner=1,.method_kind=XR_XIR_READ_METHOD},
        {.nominal_owner=1,.method_kind=XR_XIR_MEMBER_HELPER}};
    XrXirSourceModule source={"root",4,NULL,0,0};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=ids,.entry_function=1};
    XrXirGeneric generics[4]={{0},{NULL,0,actual,8, NULL},{owner,4,NULL,0, NULL},{helper,4,NULL,0, NULL}};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,2,1,3};
    XrXirDefaultTable defaults={&binding,1};
    if(constructor){
        functions[2].parameters=&parameters[1];functions[2].parameter_count=1;
        functions[2].result=XR_XIR_CONSTRUCTED_TYPE_BASE;ids[2].method_kind=XR_XIR_CONSTRUCTOR;
        owner_ops[0]=(XrXirInstruction){XR_XIR_STRUCT_NEW,XR_XIR_CONSTRUCTED_TYPE_BASE,{0},{0},0,{0}};
        owner_ops[1].args[0]=1;binding.ordinal=0;entry[1].targets[1]=0;
        entry[2].type=XR_XIR_CONSTRUCTED_TYPE_BASE+1;entry[2].args[0]=0;entry[2].args[1]=1;operands[0]=1;functions[1].operand_count=1;
    }
    XrXirModule module={XR_XIR_BUILT,functions,4,&declarations,generics,&types,NULL,XR_XIR_PROGRAM,&defaults};
    XrXirStatus expected=XR_XIR_BAD_TYPE;
    switch(mode){
    case 0:expected=XR_XIR_OK;break;
    case 1:helper[0].markers=0;break;
    case 2:helper[1].markers=XR_XIR_CONSTRAINT_SENDABLE;break;
    case 3:generics[3].parameter_count=2;break;
    case 4:ids[2].member_access=ids[3].member_access=XR_XIR_MEMBER_PRIVATE;expected=XR_XIR_BAD_STRUCTURE;break;
    case 5:symbolic[0]=XR_XIR_TYPE_PARAMETER_BASE+1;symbolic[1]=XR_XIR_TYPE_PARAMETER_BASE;break;
    case 6:entry[1].type_arguments[0]=2;entry[1].type_arguments[1]=2;expected=XR_XIR_BAD_STRUCTURE;break;
    case 7:{XrXirConstraint old=helper[1];helper[1]=helper[2];helper[2]=old;break;}
    }
    XrXirArtifact *checked=NULL;XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(suite_context, &module, &checked, &d);
    if(status!=expected)fprintf(stderr,"tuple ctor%u mode%u status%u expected%u f%u i%u\n",constructor,mode,status,expected,d.function,d.instruction);
    CHECK(status==expected);CHECK((checked!=NULL)==(expected==XR_XIR_OK));
    if(!checked)return;
    XrXirArtifact *derived=NULL;CHECK(xr_xir_compile_specialize(checked, &derived, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    uint32_t target=UINT32_MAX;
    const XrXirProvenance *p=derived->module.provenance;
    for(uint32_t f=0;f<p->count;++f)if(p->origins[f].function==3)target=f;
    CHECK(target!=UINT32_MAX && p->origins[target].argument_count==4);
    XrXirType *tuple=(XrXirType *)p->origins[target].arguments;
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(derived, &packet, NULL)==XR_XIR_OK);
    /* Schema26: each origin has function/count/type arguments followed by an
       effect-argument count and (parameter,type) pairs. Binding proofs follow
       the origin vector as count plus six u32 words per proof. */
    CHECK(packet.length>=4);
    size_t tail=4,before=0;
    CHECK((size_t)p->binding_count<=(packet.length-tail)/24);
    tail+=24*(size_t)p->binding_count;
    size_t proof_offset=packet.length-tail;
    CHECK(packet32(&packet,proof_offset)==p->binding_count);
    for(uint32_t f=0;f<p->count;++f){
        const XrXirOrigin *origin=&p->origins[f];size_t n=12;
        CHECK(n<=packet.length && (size_t)origin->argument_count<=(packet.length-n)/4);
        n+=4*(size_t)origin->argument_count;
        CHECK((size_t)origin->effect_argument_count<=(packet.length-n)/8);
        n+=8*(size_t)origin->effect_argument_count;
        CHECK(n<=packet.length-tail);tail+=n;if(f<target)before+=n;
    }
    CHECK(tail<=packet.length && packet.length-tail>=4);
    size_t origins_offset=packet.length-tail;
    CHECK(packet32(&packet,origins_offset-4)==p->count);
    size_t tuple_offset=origins_offset+before+8,at=origins_offset;
    for(uint32_t f=0;f<p->count;++f){
        const XrXirOrigin *origin=&p->origins[f];
        CHECK(packet32(&packet,at)==origin->function && packet32(&packet,at+4)==origin->argument_count);at+=8;
        for(uint32_t a=0;a<origin->argument_count;++a,at+=4)
            CHECK(packet32(&packet,at)==(uint32_t)origin->arguments[a]);
        CHECK(packet32(&packet,at)==origin->effect_argument_count);at+=4;
        for(uint32_t a=0;a<origin->effect_argument_count;++a,at+=8){
            CHECK(packet32(&packet,at)==origin->effect_arguments[a].parameter);
            CHECK(packet32(&packet,at+4)==(uint32_t)origin->effect_arguments[a].type);
        }
    }
    CHECK(at==proof_offset);
    for(unsigned a=0;a<4;++a)CHECK(packet32(&packet,tuple_offset+4*a)==(uint32_t)tuple[a]);
    for(unsigned a=0;a<4;++a){
        XrXirType old=tuple[a];tuple[a]=actual[(a+1)%4];
        CHECK(xr_xir_compile_artifact_verify(derived, NULL)==XR_XIR_BAD_TYPE);tuple[a]=old;
        default_tuple_u32(packet.bytes+tuple_offset+4*a,(uint32_t)actual[(a+1)%4]);
        XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,32);
        xr_sha256_update(&sha,packet.bytes+64,packet.length-64);xr_sha256_final(&sha,packet.bytes+32);
        XrXirArtifact *denied=NULL;
        XrXirDiagnostic wire_diagnostic={0};
        XrXirStatus wire_status=xr_xir_compile_checked_read(suite_context,packet.bytes,packet.length,&denied,&wire_diagnostic);
        if(wire_status!=XR_XIR_BAD_TYPE || denied)
            fprintf(stderr,"default_owner_tuple wire ctor=%u mode=%u argument=%u status=%u expected=%u diagnostic=%u f=%u i=%u packet=%zu tupleOffset=%zu origins=%u target=%u effectArgs=%u proofs=%u\n",
                (unsigned)constructor,mode,a,(unsigned)wire_status,(unsigned)XR_XIR_BAD_TYPE,(unsigned)wire_diagnostic.status,
                wire_diagnostic.function,wire_diagnostic.instruction,packet.length,tuple_offset,p->count,target,
                p->origins[target].effect_argument_count,p->binding_count);
        CHECK(wire_status==XR_XIR_BAD_TYPE && !denied);
        denied=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(suite_context,packet.bytes,packet.length,&denied,NULL)==XR_XIR_BAD_STRUCTURE && denied==(XrXirArtifact *)(uintptr_t)1);
        default_tuple_u32(packet.bytes+tuple_offset+4*a,(uint32_t)old);
    }
    XrXirType swap=tuple[0];tuple[0]=tuple[2];tuple[2]=swap;
    CHECK(xr_xir_compile_artifact_verify(derived, NULL)==XR_XIR_BAD_TYPE);
    tuple[2]=tuple[0];tuple[0]=swap;
    CHECK(xr_xir_compile_artifact_verify(derived, NULL)==XR_XIR_OK);
    default_tuple_u32(packet.bytes+tuple_offset,(uint32_t)actual[2]);
    default_tuple_u32(packet.bytes+tuple_offset+8,(uint32_t)actual[0]);
    XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,32);
    xr_sha256_update(&sha,packet.bytes+64,packet.length-64);xr_sha256_final(&sha,packet.bytes+32);
    XrXirArtifact *read=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &read, NULL)==XR_XIR_BAD_TYPE && !read);
    default_tuple_u32(packet.bytes+tuple_offset,(uint32_t)actual[0]);
    default_tuple_u32(packet.bytes+tuple_offset+8,(uint32_t)actual[2]);
    xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,32);
    xr_sha256_update(&sha,packet.bytes+64,packet.length-64);xr_sha256_final(&sha,packet.bytes+32);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &read, NULL)==XR_XIR_OK && read);
    xr_xir_compile_artifact_free(read);read=NULL;
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(derived);derived=NULL;
}
static void default_owner_tuple_cases(void){
    for(unsigned i=0;i<8;++i){default_tuple_case(false,i);default_tuple_case(true,i);}
    puts("default N2 Q2: 16 declaration cases, 10 independent rehashed packet attacks PASS");
}
#endif // XIR_DEFAULT_OWNER_TUPLE_CASES_H
