#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_instruction_reject_cases.h - Independent method, ordering and generic oracles
 */
#ifndef XIR_ATOMIC_INSTRUCTION_REJECT_CASES_H
#define XIR_ATOMIC_INSTRUCTION_REJECT_CASES_H
static void atomic_instruction_expected(LibraryCompileOwner *owner,AtomicInstructionFixture *f,XrXirStatus expected) {
    XrCompileResourceStats before=library_compile_stats(&owner->context);XrXirArtifact *out=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(&owner->context, &f->module, &out, &diagnostic);
    if(status!=expected)fprintf(stderr,"atomic graph expected%u actual%u f%u b%u i%u\n",expected,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==expected);if(expected!=XR_XIR_OK)CHECK(!out);else CHECK(out);
    xr_xir_compile_artifact_free(out);CHECK(library_compile_stats(&owner->context).live_bytes==before.live_bytes);
}
static size_t atomic_test_u32(const uint8_t *bytes,size_t size,size_t offset) {
    CHECK(offset<=size && size-offset>=4);
    return (size_t)bytes[offset]|((size_t)bytes[offset+1]<<8)|((size_t)bytes[offset+2]<<16)|((size_t)bytes[offset+3]<<24);
}
static size_t atomic_method_instruction_start(const uint8_t *bytes,size_t size) {
    CHECK(atomic_test_u32(bytes,size,68)==4 && atomic_test_u32(bytes,size,72)==1);
    size_t p=76;
    for(unsigned function=0;function<4;++function){
        size_t name=atomic_test_u32(bytes,size,p);CHECK(name<=size-p-4);p+=4+name;
        size_t parameters=atomic_test_u32(bytes,size,p);CHECK(parameters<=4);p+=4+4*parameters+4;
        size_t blocks=atomic_test_u32(bytes,size,p);CHECK(blocks==1);p+=4+16*blocks;
        size_t instructions=atomic_test_u32(bytes,size,p);p+=4;
        if(function==3){CHECK(instructions==5 && p<=size && size-p>=40*instructions);return p;}
        CHECK(instructions<=2);p+=40*instructions;
        size_t operands=atomic_test_u32(bytes,size,p);CHECK(operands==0);p+=4;
    }
    CHECK(false);return 0;
}
static void atomic_ordering_rejections(LibraryCompileOwner *owner,char *name) {
    AtomicInstructionFixture f;
    for(unsigned store=0;store<2;++store)for(int ordinal=0;ordinal<5;++ordinal){
        bool invalid=store?(ordinal==1||ordinal==3):(ordinal==2||ordinal==3);
        atomic_instruction_fixture(&f,name,store?XR_XIR_ATOMIC_STORE:XR_XIR_ATOMIC_LOAD,XR_XIR_I64,0,ordinal);
        atomic_instruction_expected(owner,&f,invalid?XR_XIR_BAD_TYPE:XR_XIR_OK);
    }
    for(unsigned store=0;store<2;++store){
        atomic_instruction_fixture(&f,name,store?XR_XIR_ATOMIC_STORE:XR_XIR_ATOMIC_LOAD,XR_XIR_I64,0,-2);
        atomic_instruction_expected(owner,&f,XR_XIR_OK);
    }
    atomic_instruction_fixture(&f,name,XR_XIR_ATOMIC_LOAD,XR_XIR_I64,0,4);
    XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(&owner->context, &f.module, &checked, NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    uint8_t bytes[4096];CHECK(packet.length<=sizeof(bytes));size_t offset=atomic_method_instruction_start(packet.bytes,packet.length);
    CHECK(atomic_test_u32(packet.bytes,packet.length,offset)==XR_XIR_ENUM_NEW);
    for(unsigned ordinal=2;ordinal<=3;++ordinal){memcpy(bytes,packet.bytes,packet.length);bytes[offset+24]=(uint8_t)ordinal;
        ordering_packet_digest(bytes,packet.length);XrXirArtifact *out=NULL,*occupied=checked;
        XrCompileResourceStats before=library_compile_stats(&owner->context);
        CHECK(xr_xir_compile_checked_read(&owner->context,bytes,packet.length,&out,NULL)==XR_XIR_BAD_TYPE&&!out);
        CHECK(xr_xir_compile_checked_read(&owner->context,bytes,packet.length,&occupied,NULL)==XR_XIR_BAD_STRUCTURE&&occupied==checked);
        CHECK(library_compile_stats(&owner->context).live_bytes==before.live_bytes);
    }
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
}
static void atomic_instruction_rejections(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    char *name=NULL;CHECK(ordering_factory(&owner.context,&name)==XR_XIR_OK);AtomicInstructionFixture f;
    atomic_ordering_rejections(&owner,name);
    const XrXirOp numeric[]={XR_XIR_ATOMIC_ADD,XR_XIR_ATOMIC_SUB,XR_XIR_ATOMIC_FETCH_ADD,XR_XIR_ATOMIC_FETCH_SUB};
    for(unsigned i=0;i<4;++i){atomic_instruction_fixture(&f,name,numeric[i],XR_XIR_BOOL,0,-3);
        atomic_instruction_expected(&owner,&f,XR_XIR_BAD_TYPE);}
    atomic_instruction_fixture(&f,name,XR_XIR_ATOMIC_TOGGLE,XR_XIR_F64,0,-3);
    atomic_instruction_expected(&owner,&f,XR_XIR_BAD_TYPE);
    atomic_instruction_fixture(&f,name,XR_XIR_ATOMIC_ADD,(XrXirType)65536,XR_XIR_CONSTRAINT_ATOMIC_VALUE,-3);
    atomic_instruction_expected(&owner,&f,XR_XIR_BAD_TYPE);
    f.constraint.markers=XR_XIR_CONSTRAINT_ATOMIC_NUMBER;atomic_instruction_expected(&owner,&f,XR_XIR_OK);
    atomic_instruction_fixture(&f,name,XR_XIR_ATOMIC_TOGGLE,(XrXirType)65536,XR_XIR_CONSTRAINT_ATOMIC_VALUE,-3);
    atomic_instruction_expected(&owner,&f,XR_XIR_BAD_TYPE);
    f.constraint.markers=XR_XIR_CONSTRAINT_ATOMIC_BOOLEAN;atomic_instruction_expected(&owner,&f,XR_XIR_OK);
    for(unsigned mutation=0;mutation<9;++mutation){
        atomic_instruction_fixture(&f,name,XR_XIR_ATOMIC_COMPARE_EXCHANGE,XR_XIR_I64,0,-1);
        switch(mutation){case 0:f.fields[1].type=XR_XIR_I64;break;
        case 1:f.method_ops[0].args[1]=2;f.functions[3].operand_count=2;break;
        case 2:f.method_ops[0].targets[0]=1;break;case 3:f.method_ops[0].immediate=1;break;
        case 4:f.method_ops[0].type_arguments[0]=1;break;
        case 5:f.parameters[3]=(XrXirType)256;break;
        case 6:f.nominal.native=(XrXirNominalNativeRecord){0};break;
        case 7:f.modules[0].dependency_count=0;f.modules[0].dependencies=NULL;break;
        default:f.method_ops[0].type=XR_XIR_I64;f.functions[3].result=XR_XIR_I64;break;}
        atomic_instruction_expected(&owner,&f,mutation>=1&&mutation<=4?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE);
    }
    atomic_instruction_fixture(&f,name,XR_XIR_ATOMIC_TO_STRING,XR_XIR_I64,0,-1);
    atomic_instruction_expected(&owner,&f,XR_XIR_BAD_STRUCTURE);
    xr_compile_resources_free(name);library_compile_owner_drop(&owner);
}
static void atomic_instruction_cases(void) {
    atomic_instruction_rejections();
    const XrXirType leaves[]={XR_XIR_I64,XR_XIR_F64,XR_XIR_BOOL};
    for(unsigned leaf=0;leaf<3;++leaf)for(XrXirOp op=XR_XIR_ATOMIC_NEW;op<=XR_XIR_ATOMIC_TO_STRING;op=(XrXirOp)(op+1)){
        bool numeric=op>=XR_XIR_ATOMIC_ADD&&op<=XR_XIR_ATOMIC_FETCH_SUB;
        if((numeric&&leaves[leaf]==XR_XIR_BOOL)||(op==XR_XIR_ATOMIC_TOGGLE&&leaves[leaf]!=XR_XIR_BOOL))continue;
        AtomicInstructionRun run={op,leaves[leaf],op!=XR_XIR_ATOMIC_NEW&&op!=XR_XIR_ATOMIC_TO_STRING,-1};
        library_compile_operation_cases(xr_xir_op_name(op),atomic_instruction_pipeline,&run);
    }
    for(unsigned store=0;store<2;++store)for(unsigned state=0;state<3;++state){
        int known=state==0?-2:state==1?4:store?2:1;
        AtomicInstructionRun run={store?XR_XIR_ATOMIC_STORE:XR_XIR_ATOMIC_LOAD,XR_XIR_I64,true,known};
        const char *label=store?state==0?"STORE None":state==1?"STORE Some SeqCst":"STORE Some Release":
            state==0?"LOAD None":state==1?"LOAD Some SeqCst":"LOAD Some Acquire";
        library_compile_operation_cases(label,atomic_instruction_pipeline,&run);
    }
}
#endif // XIR_ATOMIC_INSTRUCTION_REJECT_CASES_H
