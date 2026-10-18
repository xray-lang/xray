/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_try_ir_cases.h - Independent native result and operand authority controls
 */
#ifndef XIR_CHANNEL_TRY_IR_CASES_H
#define XIR_CHANNEL_TRY_IR_CASES_H
#include "xir_construction_fixture.h"
#include "shared/xnative_declaration.h"
typedef struct ChannelTryIr {
    XrXirConstraint constraint;XrXirNominalField field;
    XrXirNominalVariant recv[4],send[4];XrXirNominalDeclaration declarations[2];
    XrXirNominalTable table;XrXirType arguments[2];XrXirTypeNode nodes[4];XrXirTypes types;
    XrXirInstruction instructions[6],initialize;uint32_t operands[3];XrXirBlock block,init_block;
    XrXirFunction functions[2];XrXirSourceModule source;XrXirFunctionIdentity identities[2];
    XrXirDeclarations source_declarations;XrXirModule module;
} ChannelTryIr;
static void channel_try_ir_init(ChannelTryIr *f,bool unit) {
    *f=(ChannelTryIr){0};const char *module="stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def";
    const char *recv[]={"Value","Empty","Timeout","Closed"},*send[]={"Sent","Full","Timeout","Closed"};
    for (unsigned i=0;i<4;++i) {
        f->recv[i]=(XrXirNominalVariant){{recv[i],(uint32_t)strlen(recv[i])},i ? 1u : 0u,i ? 0u : 1u};
        f->send[i]=(XrXirNominalVariant){{send[i],(uint32_t)strlen(send[i])},0,0};
    }
    f->field=(XrXirNominalField){{"value",5},(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,0};
    f->declarations[0]=(XrXirNominalDeclaration){.module={module,(uint32_t)strlen(module)},.name={"Recv",4},.exported=1,
        .constraints=&f->constraint,.parameter_count=1,.fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_ENUM,
        .variants=f->recv,.variant_count=4,.native={.native_id=XR_NATIVE_DECLARATION_RECV}};
    f->declarations[1]=(XrXirNominalDeclaration){.module={module,(uint32_t)strlen(module)},.name={"SendResult",10},.exported=1,
        .kind=XR_XIR_NOMINAL_ENUM,.variants=f->send,.variant_count=4,.native={.native_id=XR_NATIVE_DECLARATION_SEND_RESULT}};
    for (unsigned i=0;i<2;++i) memcpy(f->declarations[i].native.source_fingerprint,
        xr_native_declaration_by_id(f->declarations[i].native.native_id)->source_fingerprint.bytes,32);
    f->table=(XrXirNominalTable){f->declarations,2,NULL};f->arguments[0]=unit ? XR_XIR_UNIT : XR_XIR_I64;f->arguments[1]=XR_XIR_STRING;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CHANNEL,.element=f->arguments[0]};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,&f->arguments[0],1,NULL,0}};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1}};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,&f->arguments[1],1,NULL,0}};
    f->types=(XrXirTypes){f->nodes,4,&f->table,NULL};f->operands[0]=1;
    f->instructions[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->instructions[1]=(XrXirInstruction){.op=XR_XIR_CHANNEL_NEW,.type=(XrXirType)256,.args={0,0}};
    f->instructions[2]=(XrXirInstruction){.op=XR_XIR_CHANNEL_TRY_SEND,.type=(XrXirType)258,.args={0,unit ? 1u : 2u}};
    f->instructions[3]=(XrXirInstruction){.op=XR_XIR_CHANNEL_TRY_RECV,.type=(XrXirType)257,.args={1,0}};
    f->instructions[4]=(XrXirInstruction){.op=XR_XIR_ENUM_GET,.type=f->arguments[0],.args={3,0}};
    f->instructions[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={unit ? 0u : 4u,0}};
    f->initialize=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};f->init_block=(XrXirBlock){.count=1};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->init_block,.block_count=1,.instructions=&f->initialize,.instruction_count=1};
    f->block=(XrXirBlock){.count=6};f->functions[1]=(XrXirFunction){.name="tryProof",.name_length=8,.result=XR_XIR_I64,
        .blocks=&f->block,.block_count=1,.instructions=f->instructions,.instruction_count=6,.operands=f->operands,.operand_count=unit ? 1u : 2u};
    f->source=(XrXirSourceModule){module,(uint32_t)strlen(module),NULL,0,0};f->identities[1].exported=1;
    f->source_declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=2,.types=&f->types,
        .declarations=&f->source_declarations};
}
static void channel_try_ir_expected(LibraryCompileOwner *owner,ChannelTryIr *fixture,unsigned axis,XrXirStatus expected) {
    XrCompileResourceStats before=library_compile_stats(&owner->context);XrXirArtifact *artifact=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&owner->context,&fixture->module,&artifact,&diagnostic);
    if (status!=expected) fprintf(stderr,"Channel try IR axis=%u expected=%u actual=%u instruction=%u\n",axis,expected,status,diagnostic.instruction);
    CHECK(status==expected && (status==XR_XIR_OK ? artifact!=NULL : artifact==NULL));xr_xir_compile_artifact_free(artifact);
    CHECK(library_compile_stats(&owner->context).live_bytes==before.live_bytes);
}
static void channel_try_ir_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);ChannelTryIr fixture;
    for (unsigned unit=0;unit<2;++unit) { channel_try_ir_init(&fixture,unit!=0);channel_try_ir_expected(&owner,&fixture,UINT32_MAX,XR_XIR_OK); }
    for (unsigned axis=0;axis<18;++axis) {
        channel_try_ir_init(&fixture,axis==13 || axis==14 || axis==17);XrXirStatus expected=XR_XIR_BAD_TYPE;
        switch (axis) {
        case 0: fixture.instructions[2].args[1]=0;expected=XR_XIR_BAD_STRUCTURE;break;
        case 1: fixture.instructions[2].args[1]=1;expected=XR_XIR_BAD_STRUCTURE;break;
        case 2: fixture.instructions[2].args[1]=3;expected=XR_XIR_BAD_STRUCTURE;break;
        case 3: fixture.operands[0]=0;break;
        case 4: fixture.operands[1]=1;break;
        case 5: fixture.instructions[2].type=(XrXirType)257;break;
        case 6: fixture.instructions[3].type=(XrXirType)258;break;
        case 7: fixture.instructions[3].type=(XrXirType)259;break;
        case 8: fixture.instructions[3].args[0]=0;break;
        case 9: fixture.instructions[4].type=XR_XIR_UNIT;break;
        case 10: fixture.declarations[0].native.source_fingerprint[0]^=1;expected=XR_XIR_BAD_STRUCTURE;break;
        case 11: fixture.declarations[1].native=(XrXirNominalNativeRecord){0};break;
        case 12: fixture.declarations[0].native=(XrXirNominalNativeRecord){0};break;
        case 13: fixture.instructions[2].args[1]=2;expected=XR_XIR_BAD_STRUCTURE;break;
        case 14: fixture.instructions[4].args[1]=1;expected=XR_XIR_BAD_STRUCTURE;break;
        case 15: fixture.instructions[3].args[0]=UINT32_MAX;expected=XR_XIR_BAD_VALUE;break;
        case 16: fixture.instructions[2].type=XR_XIR_UNIT;break;
        case 17: fixture.instructions[4].type=XR_XIR_I64;break;
        }
        fixture.functions[1].operand_count=fixture.instructions[2].args[1];
        fixture.functions[1].operands=fixture.functions[1].operand_count ? fixture.operands : NULL;
        channel_try_ir_expected(&owner,&fixture,axis,expected);
    }
    library_compile_owner_drop(&owner);puts("Channel try XIR: two positive payload types and eighteen exact rejection axes physical0");
}
static void channel_unit_construct_ir_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);ChannelTryIr fixture;
    for (unsigned unit=0;unit<2;++unit) {
        channel_try_ir_init(&fixture,unit!=0);
        fixture.instructions[2]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)257,.args={0,unit ? 0u : 1u}};
        fixture.instructions[4].args[0]=2;fixture.operands[0]=0;
        fixture.functions[1].operand_count=unit ? 0u : 1u;fixture.functions[1].operands=unit ? NULL : fixture.operands;
        channel_try_ir_expected(&owner,&fixture,UINT32_MAX,XR_XIR_OK);
    }
    for (unsigned axis=0;axis<4;++axis) {
        bool unit=axis!=1;channel_try_ir_init(&fixture,unit);
        fixture.instructions[2]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)257};
        fixture.instructions[4].args[0]=2;fixture.functions[1].operand_count=0;fixture.functions[1].operands=NULL;
        XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        if (axis==0) {
            fixture.instructions[2].args[1]=1;fixture.functions[1].operand_count=1;
            fixture.functions[1].operands=fixture.operands;fixture.operands[0]=0;
        } else if (axis==2) { fixture.declarations[0].native=(XrXirNominalNativeRecord){0};expected=XR_XIR_BAD_TYPE; }
        else if (axis==3) { fixture.instructions[2].immediate=1;fixture.instructions[4].immediate=1; }
        channel_try_ir_expected(&owner,&fixture,axis,expected);
    }
    library_compile_owner_drop(&owner);puts("Channel native Recv construction: i64/Unit positives and four exact rejection axes physical0");
}
#endif // XIR_CHANNEL_TRY_IR_CASES_H
