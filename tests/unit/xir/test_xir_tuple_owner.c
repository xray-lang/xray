/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_owner.c - Owned ordered type metadata and Checked admission
 */
#include "xir_construction_fixture.h"
#include "xir/xxir.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_interface_members.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_type_inference.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_type_scratch_internal.h"
#include "base/xsha256.h"
#include "tuple_owner_observer.h"
#include "tuple_old_23_61.inc.c"
#include "tuple_24_62.inc.c"
#include "tuple_24_63.inc.c"
#include "tuple_25_65.inc.c"
#include "xir_tuple72_golden.h"
#include "xir/xxir_effect_terms.inc.c"
static XrXirType constructed(uint32_t n) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);}
static XrXirType parameter(uint32_t n) {return (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+n);}
static const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
typedef struct TupleFixture {
    XrXirCallableParameter fields[3];XrXirTypeNode node;XrXirTypes types;
    uint32_t operands[4];XrXirType param;
    XrXirInstruction ops[7];XrXirBlock block;XrXirFunction function;XrXirModule module;
} TupleFixture;
static void fixture(TupleFixture *f) {
    memset(f,0,sizeof(*f));
    f->fields[0]=(XrXirCallableParameter){XR_XIR_UNIT,0};
    f->fields[1]=(XrXirCallableParameter){XR_XIR_I64,0};
    f->fields[2]=(XrXirCallableParameter){XR_XIR_STRING,0};
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=3};
    f->types=(XrXirTypes){&f->node,1,NULL,NULL};f->param=XR_XIR_STRING;
    f->operands[0]=1;f->operands[1]=0;
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=73};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=constructed(0),.args={0,2}};
    f->ops[2]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_UNIT,.args={2},.immediate=0};
    f->ops[3]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_STRING,.args={2},.immediate=2};
    f->ops[4]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_I64,.args={2},.immediate=1};
    f->ops[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={5}};
    f->block=(XrXirBlock){.first=0,.count=6};
    f->function=(XrXirFunction){.name="tuple",.name_length=5,.parameters=&f->param,.parameter_count=1,
        .result=XR_XIR_I64,.blocks=&f->block,.block_count=1,.instructions=f->ops,.instruction_count=6,
        .operands=f->operands,.operand_count=2};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=&f->function,.function_count=1,.types=&f->types};
}
static XrXirStatus pipeline(const XrXirCompileContext *c) {
    TupleFixture f;fixture(&f);XrXirArtifact *checked=NULL,*decoded=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};XrXirTypes *clone=NULL;
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(checked,&packet,NULL);
    if(status==XR_XIR_OK)CHECK(packet.length==sizeof(tuple72_golden) && !memcmp(packet.bytes,tuple72_golden,packet.length));
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(c,packet.bytes,packet.length,&decoded,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_types_clone(c,xr_xir_compile_artifact_module(decoded)->types,&clone);
    if(status==XR_XIR_OK) {
        CHECK(xr_xir_compile_artifact_module(checked)->types->nodes[0].parameters!=f.fields);
        CHECK(clone->nodes[0].parameters!=f.fields && clone->nodes[0].parameter_count==3);
        CHECK(clone->nodes[0].parameters[0].type==XR_XIR_UNIT && clone->nodes[0].parameters[1].type==XR_XIR_I64 &&
            clone->nodes[0].parameters[2].type==XR_XIR_STRING && !clone->nodes[0].parameters[2].mode);
        memset(f.fields,0xcc,sizeof(f.fields));
        status=xr_xir_compile_artifact_verify(checked,NULL);
        if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(decoded,NULL);
    }
    if(status==XR_XIR_OK) {
        status=xr_xir_compile_lower(decoded,&target,&lowered,NULL);
        if(status==XR_XIR_OK) {
            CHECK(xr_xir_compile_artifact_module(lowered)->stage==XR_XIR_LOWERED);
            status=xr_xir_compile_artifact_verify(lowered,NULL);
        }
    }
    xr_xir_compile_types_free(clone);xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked);return status;
}
static void rejection_matrix(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;TupleFixture f;
    fixture(&f);CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    for(unsigned i=0;i<18;++i) {
        fixture(&f);
        switch(i) {
        case 0:f.node.parameter_count=0;break;
        case 1:f.node.parameters=NULL;break;
        case 2:f.node.element=XR_XIR_I64;break;
        case 3:f.node.result=XR_XIR_STRING;break;
        case 4:f.node.flags=1;break;
        case 5:f.node.nominal.declaration=1;break;
        case 6:f.node.nominal.arguments=&f.param;break;
        case 7:f.node.nominal.argument_count=1;break;
        case 8:f.node.nominal.fields=&f.param;break;
        case 9:f.node.nominal.field_count=1;break;
        case 10:f.fields[1].mode=1;break;
        case 11:f.fields[1].type=constructed(0);break;
        case 12:f.fields[1].type=constructed(1);break;
        case 13:f.fields[1].type=(XrXirType)255;break;
        case 14:f.node.parameter_span=1;break;
        case 15:f.fields[1].type=parameter(0);break;
        case 16:f.node.kind=XR_XIR_TYPE_CALLABLE;break;
        case 17:f.node.parameter_count=65537;break;
        }
        CHECK(xir_fixture_verify(&c, &f.module, NULL)!=XR_XIR_OK);CHECK(stats(&c).live_bytes==baseline);
    }
    for(unsigned i=0;i<15;++i) {
        fixture(&f);
        switch(i) {
        case 0:f.ops[1].args[1]=1;break;
        case 1:f.ops[1].args[1]=3;f.function.operand_count=3;f.operands[2]=1;break;
        case 2:f.ops[1].args[1]=0;break;
        case 3:f.operands[0]=UINT32_MAX;break;
        case 4:f.operands[0]=3;break;
        case 5:f.operands[0]=2;break;
        case 6:f.operands[0]=0;break;
        case 7:f.ops[2].args[0]=UINT32_MAX;break;
        case 8:f.ops[2].args[0]=0;break;
        case 9:f.ops[2].immediate=-1;break;
        case 10:f.ops[2].immediate=3;break;
        case 11:f.ops[2].type=XR_XIR_I64;break;
        case 12:f.ops[1].immediate=1;break;
        case 13:f.ops[1].type=XR_XIR_STRING;break;
        case 14:f.module.stage=(XrXirStage)99;break;
        }
        CHECK(xir_fixture_verify(&c, &f.module, NULL)!=XR_XIR_OK);CHECK(stats(&c).live_bytes==baseline);
    }
    fixture(&f);f.node.parameter_count=1;f.ops[1].args[1]=0;
    f.ops[1].args[0]=0;f.ops[2]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_UNIT,.args={2}};
    f.ops[3]=(XrXirInstruction){.op=XR_XIR_RETURN};f.block.count=f.function.instruction_count=4;
    f.function.result=XR_XIR_UNIT;f.function.operand_count=0;f.function.operands=NULL;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    fixture(&f);f.fields[1].type=parameter(0);f.node.parameter_span=1;
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_expression_shape(&c,&f.types,constructed(0),0)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_expression_shape(&c,&f.types,constructed(0),1)==XR_XIR_OK);
    XrXirTypeNode duplicate[2]={f.node,f.node};XrXirTypes duplicates={duplicate,2,NULL,NULL};
    CHECK(xr_xir_compile_types_structure_verify(&c,&duplicates)==XR_XIR_BAD_STRUCTURE);
    fixture(&f);XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&f.types,&arena)==XR_XIR_VALUE_OK && arena);
    const XrXirTypes *owned=xr_xir_compile_type_arena_types(arena);
    CHECK(owned && owned->nodes!=&f.node && owned->nodes[0].parameters!=f.fields);
    memset(f.fields,0xcc,sizeof(f.fields));
    CHECK(xr_xir_compile_types_structure_verify(&c,owned)==XR_XIR_OK);
    CHECK(owned->nodes[0].parameters[0].type==XR_XIR_UNIT && owned->nodes[0].parameters[1].type==XR_XIR_I64 &&
        owned->nodes[0].parameters[2].type==XR_XIR_STRING);
    xr_xir_compile_type_arena_drop(arena);fixture(&f);
    XrXirLayout layout={0};
    for(unsigned i=XR_XIR_LAYOUT_STORAGE;i<=XR_XIR_LAYOUT_FRAME;++i) {
        CHECK(xr_xir_compile_layout(&c,&f.types,constructed(0),&target,(XrXirLayoutContext)i,&layout)==XR_XIR_OK);
        CHECK(layout.alignment==8 && layout.size==((i==XR_XIR_LAYOUT_PARAMETER||i==XR_XIR_LAYOUT_RESULT||i==XR_XIR_LAYOUT_BOXED)?16u:8u));
    }
    owner_free(&c,baseline);puts("Tuple 18 shape + 15 operation rejects, concrete Unit, span, duplicate, arena and layout PASS");
}
static void old_rejection(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    uint64_t allocations=stats(&c).allocation_count;
    XrXirArtifact *artifact=NULL;
    CHECK(xr_xir_compile_checked_read(&c,old_23_61,sizeof(old_23_61),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    CHECK(xr_xir_compile_checked_read(&c,tuple_24_62,sizeof(tuple_24_62),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    CHECK(xr_xir_compile_checked_read(&c,tuple_24_63,sizeof(tuple_24_63),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    CHECK(xr_xir_compile_checked_read(&c,tuple_25_64,sizeof(tuple_25_64),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    CHECK(xr_xir_compile_checked_read(&c,tuple_25_65,sizeof(tuple_25_65),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    artifact=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&c,tuple_25_65,sizeof(tuple_25_65),&artifact,NULL)==XR_XIR_BAD_STRUCTURE && artifact==(XrXirArtifact *)(uintptr_t)1);
    CHECK(stats(&c).allocation_count==allocations);owner_free(&c,baseline);
    puts("complete 23/61, Tuple24/62,24/63 and25/64,25/65 packets rejected before allocation PASS");
}
static void markers(void) {
    TupleFixture f;fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    XrXirProofContext proof={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    CHECK(xr_xir_compile_type_markers_prove(&c,&proof,constructed(0),XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&c,&proof,constructed(0),XR_XIR_CONSTRAINT_EQUAL)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_storage_prove(&c,&proof,XR_XIR_UNIT)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_storage_prove(&c,&proof,constructed(0))==XR_XIR_OK);
    f.fields[2].type=XR_XIR_ERROR;
    CHECK(xr_xir_compile_type_markers_prove(&c,&proof,constructed(0),XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_BAD_TYPE);
    fixture(&f);f.fields[1].type=parameter(0);f.node.parameter_span=1;
    XrXirConstraint bound={.markers=XR_XIR_CONSTRAINT_SENDABLE};XrXirGeneric generic={.constraints=&bound,.parameter_count=1};
    f.module.generics=&generic;
    CHECK(xr_xir_compile_type_markers_prove(&c,&proof,constructed(0),XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    bound.markers=0;
    CHECK(xr_xir_compile_type_markers_prove(&c,&proof,constructed(0),XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_BAD_TYPE);
    owner_free(&c,baseline);puts("Tuple Sendable field conjunction and authentic bounds; Equal and generic Unit stay closed PASS");
}
static void wire_rejection(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    uint8_t bytes[sizeof(tuple72_golden)+4];
    for(unsigned mode=0;mode<9;++mode) {
        memcpy(bytes,tuple72_golden,sizeof(tuple72_golden));size_t length=sizeof(tuple72_golden);
        size_t at=length-28;uint32_t value=0;
        switch(mode) {
        case 0:at=length-36;value=7;break;
        case 1:at=length-32;value=1;break;
        case 2:value=0;break;
        case 3:value=65537;break;
        case 4:at=length-20;value=256;break;
        case 5:at=length-20;value=257;break;
        case 6:at=length-20;value=255;break;
        case 7:at=length-20;value=3;break;
        case 8:at=length-28;value=4;break;
        }
        for(unsigned b=0;b<4;++b)bytes[at+b]=(uint8_t)(value>>(8*b));
        uint8_t image[sizeof(tuple72_golden)];memcpy(image,bytes,32);memcpy(image+32,bytes+64,length-64);
        xr_sha256(image,length-32,bytes+32);XrXirArtifact *out=NULL;
        CHECK(xr_xir_compile_checked_read(&c,bytes,length,&out,NULL)!=XR_XIR_OK && !out);
        CHECK(stats(&c).live_bytes==baseline);
    }
    for(size_t length=64;length<sizeof(tuple72_golden);++length) {
        XrXirArtifact *out=NULL;CHECK(xr_xir_compile_checked_read(&c,tuple72_golden,length,&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
    }
    owner_free(&c,baseline);puts("Tuple 9 correctly hashed hostile payloads and every packet truncation rejected PASS");
}
static void hidden_tuple_ownership(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    for(unsigned signature=0;signature<2;++signature) {
        TupleFixture f;fixture(&f);f.param=constructed(0);
        if(!signature){f.function.parameter_count=0;f.function.parameters=NULL;}
        f.ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
        f.ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={signature}};
        f.block.count=f.function.instruction_count=2;f.function.operand_count=0;f.function.operands=NULL;
        XrXirArtifact *checked=NULL,*lowered=NULL;
        CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_lower(checked,&target,&lowered,NULL)==XR_XIR_OK && lowered);
        CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
        XrXirTypeArena *arena=NULL;
        CHECK(xr_xir_compile_type_arena_new(&c,xr_xir_compile_artifact_module(lowered)->types,&arena)==XR_XIR_VALUE_OK && arena);
        const XrXirTypes *owned=xr_xir_compile_type_arena_types(arena);
        CHECK(owned->nodes[0].parameters!=f.fields);
        xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(checked);
        memset(f.fields,0xcc,sizeof(f.fields));
        CHECK(xr_xir_compile_types_structure_verify(&c,owned)==XR_XIR_OK);
        CHECK(owned->nodes[0].parameters[2].type==XR_XIR_STRING);
        xr_xir_compile_type_arena_drop(arena);fixture(&f);
        f.module.stage=XR_XIR_LOWERED;
        CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
        checked=NULL;
        CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_BAD_STAGE && !checked);
    }
    TupleFixture f;fixture(&f);
    CHECK(xr_xir_compile_class_field_verify(&c,&f.types,constructed(0))==XR_XIR_OK);
    f.fields[0].mode=1;
    CHECK(xr_xir_compile_class_field_verify(&c,&f.types,constructed(0))==XR_XIR_BAD_TYPE);
    owner_free(&c,baseline);puts("unused and signature-only Tuple Lowered and independently owned arena; forged check stage and concrete Unit mode rejected PASS");
}
static XrXirStatus sendable_dag(const XrXirCompileContext *c) {
    TupleFixture f;fixture(&f);
    XrXirCallableParameter repeated[3]={{constructed(0),0},{constructed(0),0},{XR_XIR_UNIT,0}};
    XrXirTypeNode nodes[2]={f.node,{.kind=XR_XIR_TYPE_TUPLE,.parameters=repeated,.parameter_count=3}};
    f.types.nodes=nodes;f.types.count=2;
    XrXirProofContext proof={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    XrXirStatus status=xr_xir_compile_types_structure_verify(c,&f.types);
    if(status==XR_XIR_OK)status=xr_xir_compile_type_markers_prove(c,&proof,constructed(1),XR_XIR_CONSTRAINT_SENDABLE);
    return status;
}
static XrXirStatus visibility(const XrXirCompileContext *c) {
    XrXirNominalDeclaration nominal={.module={"home",4},.name={"Hidden",6},.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable nominals={&nominal,1,NULL};
    XrXirCallableParameter fields[2]={{XR_XIR_UNIT,0},{constructed(0),0}};
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2}};
    XrXirTypes types={nodes,2,&nominals,NULL};
    XrXirSourceModule modules[2]={{"home",4,NULL,0,0},{"away",4,NULL,0,1}};
    XrXirFunctionIdentity identities[2]={{.module=0},{.module=1}};
    XrXirDeclarations declarations={.modules=modules,.module_count=2,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirInstruction op={.op=XR_XIR_RETURN};XrXirBlock block={.count=1};
    XrXirFunction functions[2]={{.name="home_init",.name_length=9,.result=XR_XIR_UNIT,.instructions=&op,
        .instruction_count=1,.blocks=&block,.block_count=1},
        {.name="away_init",.name_length=9,.result=XR_XIR_UNIT,.instructions=&op,.instruction_count=1,.blocks=&block,.block_count=1}};
    XrXirModule module={.stage=XR_XIR_CHECKED,.functions=functions,.function_count=2,.types=&types,
        .declarations=&declarations,.linkage_kind=XR_XIR_LIBRARY};
    XirTypeScratch scratch={c->resources,NULL,0};uint32_t imported=0;
    XrXirStatus status=xr_xir_compile_types_structure_verify(c,&types);
    if(status==XR_XIR_OK)status=xr_xir_compile_type_access_scratch(c,&module,0,constructed(1),&scratch);
    if(status==XR_XIR_OK) {
        XrXirStatus denied=xr_xir_compile_type_access_scratch(c,&module,1,constructed(1),&scratch);
        if(denied==XR_XIR_BUDGET)status=denied;else CHECK(denied==XR_XIR_BAD_TYPE);
    }
    nominal.exported=1;
    if(status==XR_XIR_OK) {
        XrXirStatus denied=xr_xir_compile_type_access_scratch(c,&module,1,constructed(1),&scratch);
        if(denied==XR_XIR_BUDGET)status=denied;else CHECK(denied==XR_XIR_BAD_TYPE);
    }
    modules[1].dependencies=&imported;modules[1].dependency_count=1;
    if(status==XR_XIR_OK)status=xr_xir_compile_type_access_scratch(c,&module,1,constructed(1),&scratch);
    nominal.exported=0;
    if(status==XR_XIR_OK) {
        XrXirStatus denied=xr_xir_compile_type_access_scratch(c,&module,1,constructed(1),&scratch);
        if(denied==XR_XIR_BUDGET)status=denied;else CHECK(denied==XR_XIR_BAD_TYPE);
    }
    xir_type_scratch_free(&scratch);return status;
}
static XrXirStatus all_kinds(const XrXirCompileContext *c) {
    XrXirCallableParameter callable={XR_XIR_I64,0};
    XrXirCallableParameter fields[6]={{XR_XIR_UNIT,0},{XR_XIR_STRING,0},{constructed(0),0},
        {constructed(1),0},{constructed(3),0},{constructed(4),0}};
    XrXirNominalDeclaration nominal={.module={"m",1},.name={"Empty",5},.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable nominals={&nominal,1,NULL};
    XrXirTypeNode nodes[6]={{.kind=XR_XIR_TYPE_CALLABLE,.parameters=&callable,.parameter_count=1,.result=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=6}};
    XrXirTypes types={nodes,6,&nominals,NULL};XrXirSourceModule source={"m",1,NULL,0,0};
    XrXirFunctionIdentity identity={0};XrXirDeclarations declarations={.modules=&source,.module_count=1,
        .functions=&identity,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirInstruction op={.op=XR_XIR_RETURN};XrXirBlock block={.first=0,.count=1};
    XrXirFunction function={.name="init",.name_length=4,.result=XR_XIR_UNIT,.instructions=&op,.instruction_count=1,
        .blocks=&block,.block_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.declarations=&declarations,
        .types=&types,.linkage_kind=XR_XIR_LIBRARY};
    XrXirArtifact *checked=NULL,*decoded=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xir_fixture_check(c, &module, &checked, NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(checked,&packet,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(c,packet.bytes,packet.length,&decoded,NULL);
    if(status==XR_XIR_OK) {
        const XrXirTypes *pool=xr_xir_compile_artifact_module(decoded)->types;
        CHECK(pool->count==6);
        for(unsigned i=0;i<6;++i)CHECK(pool->nodes[i].kind==nodes[i].kind &&
            (!nodes[i].parameter_count || pool->nodes[i].parameters!=nodes[i].parameters));
        CHECK(pool->nodes[5].parameter_count==6 && pool->nodes[5].parameters[0].type==XR_XIR_UNIT &&
            pool->nodes[5].parameters[5].type==constructed(4));
        status=xr_xir_compile_artifact_verify(decoded,NULL);
    }
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);return status;
}
static XrXirStatus specialization(const XrXirCompileContext *c) {
    XrXirCallableParameter fields[3]={{XR_XIR_UNIT,0},{parameter(0),0},{XR_XIR_STRING,0}};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=3,.parameter_span=1};
    XrXirTypes types={&node,1,NULL,NULL};XrXirType generic_param=parameter(0),closed_param=XR_XIR_I64;
    uint32_t body_operands[2]={0,1},call_operand=0;
    XrXirInstruction body[3]={{.op=XR_XIR_TUPLE_NEW,.type=constructed(0),.args={0,2}},
        {.op=XR_XIR_TUPLE_FIELD,.type=parameter(0),.args={2},.immediate=1},
        {.op=XR_XIR_RETURN,.args={3}}};
    XrXirType params[2]={generic_param,XR_XIR_STRING};
    XrXirInstruction caller[1]={{.op=XR_XIR_RETURN}};
    XrXirBlock blocks[2]={{.first=0,.count=1},{.first=0,.count=3}};
    XrXirFunction functions[2]={{.name="root",.name_length=4,.result=XR_XIR_UNIT,.blocks=blocks,.block_count=1,
        .instructions=caller,.instruction_count=1},{.name="make",.name_length=4,.parameters=params,.parameter_count=2,
        .result=generic_param,.blocks=blocks+1,.block_count=1,.instructions=body,.instruction_count=3,
        .operands=body_operands,.operand_count=2}};
    XrXirConstraint bound={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    XrXirGeneric generics[2]={{0},{.constraints=&bound,.parameter_count=1}};
    /* A separate closed entry references an otherwise generic body twice. */
    XrXirType root_params[2]={closed_param,XR_XIR_STRING};uint32_t root_operands[4]={0,1,0,1};
    XrXirType arguments[2]={XR_XIR_I64,XR_XIR_I64};
    XrXirInstruction root_ops[3]={{.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,2},.immediate=1,.type_arguments={0,1}},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={2,2},.immediate=1,.type_arguments={1,1}},
        {.op=XR_XIR_RETURN,.args={3}}};
    functions[0].parameters=root_params;functions[0].parameter_count=2;functions[0].result=XR_XIR_I64;
    functions[0].instructions=root_ops;functions[0].instruction_count=3;functions[0].operands=root_operands;functions[0].operand_count=4;
    blocks[0].count=3;generics[0].arguments=arguments;generics[0].argument_count=2;
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,.generics=generics,.types=&types};
    XrXirArtifact *checked=NULL,*closed=NULL;XrXirStatus status=xir_fixture_check(c, &module, &checked, NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&closed,NULL);
    if(status==XR_XIR_OK) {
        const XrXirModule *m=xr_xir_compile_artifact_module(closed);
        CHECK(m->stage==XR_XIR_CHECKED && !m->generics && m->function_count==2 && m->types->count==1);
        CHECK(m->functions[0].instructions[0].immediate==m->functions[0].instructions[1].immediate);
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(m->types,m->functions[1].instructions[0].type);
        CHECK(tuple && tuple->parameter_count==3 && !tuple->parameter_span && tuple->parameters[0].type==XR_XIR_UNIT &&
            tuple->parameters[1].type==XR_XIR_I64 && tuple->parameters[2].type==XR_XIR_STRING);
        status=xr_xir_compile_artifact_verify(closed,NULL);
    }
    (void)call_operand;
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);return status;
}
static XrXirStatus closure(const XrXirCompileContext *c) {
    XrXirCallableParameter fields[3]={{parameter(0),0},{XR_XIR_UNIT,0},{parameter(1),0}};
    XrXirCallableParameter call_params[1]={{constructed(0),0}};
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=3,.parameter_span=2},
        {.kind=XR_XIR_TYPE_CALLABLE,.parameters=call_params,.parameter_count=1,.result=constructed(0),.parameter_span=2}};
    XrXirConstraint bounds[2]={{0},{0}};
    XrXirInterfaceMethod method={.name={"inspect",7},.signature=constructed(1)};
    XrXirInterfaceDeclaration declaration={.module={"m",1},.name={"Inspect",7},.parameter_count=2,.constraints=bounds,
        .methods=&method,.method_count=1};XrXirInterfaceTable table={&declaration,1};
    XrXirTypes types={nodes,2,NULL,&table};XrXirType formals[2]={parameter(0),parameter(1)};
    XrXirType actuals[2]={XR_XIR_I64,XR_XIR_STRING};XrXirInterfaceApplication app={0,formals,2};
    XrXirInterfaceClosureRequest request={&types,&types,&app,1,actuals,2,0};XrXirInterfaceClosure *out=NULL;
    XrXirTypes *owned=NULL;
    XrXirStatus status=xr_xir_compile_types_structure_verify(c,&types);
    if(status==XR_XIR_OK)status=xr_xir_compile_interface_closure_substitute(c,&request,&out);
    if(status==XR_XIR_OK) {
        const XrXirInterfaceRequirement *requirement=xr_xir_interface_closure_requirement(out,0);
        const XrXirTypes *pool=xr_xir_interface_closure_types(out);
        const XrXirTypeNode *signature=xr_xir_callable_signature(pool,requirement->signature);
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(pool,signature->result);
        XrXirType result=signature->result;
        CHECK(tuple && tuple->parameter_count==3 && !tuple->parameter_span && tuple->parameters[0].type==XR_XIR_I64 &&
            tuple->parameters[1].type==XR_XIR_UNIT && tuple->parameters[2].type==XR_XIR_STRING);
        CHECK(tuple->parameters!=fields);
        status=xr_xir_compile_type_expression_shape(c,pool,signature->result,0);
        if(status==XR_XIR_OK)status=xr_xir_compile_type_substitution_matches_between(c,&types,pool,actuals,2,constructed(0),signature->result);
        /* A closure borrows the caller's base pool; the receiving owner clones its complete view. */
        if(status==XR_XIR_OK)status=xr_xir_compile_types_clone(c,pool,&owned);
        xr_xir_compile_interface_closure_free(out);out=NULL;
        memset(fields,0xcc,sizeof(fields));memset(call_params,0xcc,sizeof(call_params));
        if(status==XR_XIR_OK)status=xr_xir_compile_types_structure_verify(c,owned);
        if(status==XR_XIR_OK)status=xr_xir_compile_type_expression_shape(c,owned,result,0);
    }
    xr_xir_compile_types_free(owned);xr_xir_compile_interface_closure_free(out);return status;
}
typedef XrXirStatus (*Operation)(const XrXirCompileContext *);
static XrXirStatus effects(const XrXirCompileContext *c) {
    XrXirCallableParameter fields[3]={{constructed(0),0},{XR_XIR_UNIT,0},{parameter(1),0}};
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_ARRAY,.element=parameter(0),.parameter_span=1},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=3,.parameter_span=2}};
    XrXirTypes types={nodes,2,NULL,NULL};XrXirType arguments[2]={XR_XIR_STRING,XR_XIR_I64};
    XrXirGeneric substitution={.arguments=arguments,.argument_count=2};XrXirCompileContext state=*c;
    EffectTerms pool={.types=types,.remaining=&state,.capacity=2};XrXirType result=XR_XIR_UNIT;
    XrXirStatus status=effect_terms_substitute(&pool,constructed(1),&substitution,&result);
    if(status==XR_XIR_OK) {
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(&pool.types,result);
        CHECK(tuple && tuple->parameter_count==3 && !tuple->parameter_span && tuple->parameters[1].type==XR_XIR_UNIT &&
            tuple->parameters[2].type==XR_XIR_I64 && xr_xir_array_element(&pool.types,tuple->parameters[0].type)==XR_XIR_STRING);
        status=xr_xir_compile_type_substitution_matches(c,&pool.types,arguments,2,constructed(1),result);
        if(status==XR_XIR_OK)status=xr_xir_compile_types_structure_verify(c,&pool.types);
    }
    effect_terms_free(&pool);return status;
}
static XrXirStatus inference(const XrXirCompileContext *c) {
    XrXirCallableParameter formal[3]={{parameter(0),0},{XR_XIR_UNIT,0},{parameter(1),0}};
    XrXirCallableParameter actual[3]={{XR_XIR_STRING,0},{XR_XIR_UNIT,0},{XR_XIR_I64,0}};
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_TUPLE,.parameters=formal,.parameter_count=3,.parameter_span=2},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=actual,.parameter_count=3}};
    XrXirTypes types={nodes,2,NULL,NULL};XrXirInferenceRequest request={.types=&types,.own_count=2};
    XrXirInferenceState *state=NULL;XrXirType result[2]={0};XrXirInferenceKnown known={0};
    XrXirStatus status=xr_xir_compile_inference_begin(c,&request,&state);
    if(status==XR_XIR_OK)status=xr_xir_compile_inference_expected_known(state,&types,constructed(0),&known);
    if(status==XR_XIR_OK) {CHECK(!known.known);status=xr_xir_compile_inference_observe(state,&types,(XrXirInferencePair){constructed(0),constructed(1)});}
    if(status==XR_XIR_OK)status=xr_xir_compile_inference_expected_known(state,&types,constructed(0),&known);
    if(status==XR_XIR_OK) {CHECK(known.known);status=xr_xir_compile_inference_finalize(state,&types,result,2);}
    if(status==XR_XIR_OK)CHECK(result[0]==XR_XIR_STRING && result[1]==XR_XIR_I64);
    xr_xir_compile_inference_dispose(state);return status;
}
static XrCompileResourceStats measure(Operation op,XrCompileResourceLimits limits,XrXirStatus expected) {
    XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    XrXirStatus status=op(&c);if(status!=expected)fprintf(stderr,"status=%u expected=%u\n",status,expected);
    CHECK(status==expected);XrCompileResourceStats s=stats(&c);owner_free(&c,baseline);return s;
}
static void boundaries(const char *name,Operation op) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;XrXirStatus status=op(&c);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){if(status!=XR_XIR_OK)fprintf(stderr,"%s status=%u\n",name,status);CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites&&sites<4096);}
        else {if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"%s OOM site=%zu status=%u\n",name,pass-1,status);CHECK(injected && status==XR_XIR_OUT_OF_MEMORY);}
        owner_free(&c,baseline);
    }
    XrCompileResourceStats base=measure(op,caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={base.allocated_bytes,base.peak_bytes,base.work};
    XrCompileResourceStats actual=measure(op,exact,XR_XIR_OK);
    CHECK(base.allocated_bytes==actual.allocated_bytes && base.peak_bytes==actual.peak_bytes && base.work==actual.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)measure(op,less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)measure(op,less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)measure(op,less,XR_XIR_BUDGET);
    printf("%s OOM sites=%zu finite exact and 3 axes -1 physical=0/0 PASS\n",name,sites);
}
int main(void) {
    old_rejection();rejection_matrix();markers();wire_rejection();hidden_tuple_ownership();boundaries("Tuple check/codec/clone/Lowered",pipeline);
    boundaries("all original five kinds + Tuple codec",all_kinds);
    boundaries("Tuple Checked specialization",specialization);boundaries("Tuple interface substitution",closure);
    boundaries("Tuple effect substitution",effects);boundaries("Tuple inference",inference);
    boundaries("Tuple Sendable shared DAG",sendable_dag);boundaries("Tuple nested visibility",visibility);
    puts("Tuple Checked ownership qualification PASS");return 0;
}
