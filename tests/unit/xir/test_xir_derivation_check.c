/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derivation_check.c - Exact ordinary derivation authority receiving
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while(0)
#include "xir_source_compile_owner.h"
#include "xir_construction_fixture.h"
#include "xir/xxir_derivation_internal.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir_derivation74_golden.h"
#include "grant74_historical_goldens.h"

typedef struct DeriveFixture {
    XrXirType receiver,field_type;
    XrXirTypeNode nodes[4];
    uint32_t operands[1];
    XrXirNominalField field;
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirInterfaceMethod method;
    XrXirInterfaceDeclaration protocols[2];
    XrXirInterfaceTable interfaces;
    XrXirTypes types;
    XrXirInstruction entry[3],init,body[4],generator[2],factory[3];
    XrXirBlock blocks[6];
    XrXirFunction functions[6];
    XrXirFunctionIdentity identities[6];
    XrXirSourceModule source;
    XrXirImplementationBinding witness;
    XrXirImplementation implementation;
    XrXirImplementationTable implementations;
    XrXirDerivationMember member;
    XrXirDerivationPolicy policy;
    XrXirGeneratedOrigin sites[4];
    XrXirDerivationGrant binding;
    XrXirDerivationTable grants;
    XrXirDeclarations declarations;
    XrXirModule built;
} DeriveFixture;
static uint64_t derive_baseline_live;
static void derive_fixture(DeriveFixture *f) {
    memset(f,0,sizeof(*f));f->receiver=(XrXirType)256;f->field_type=XR_XIR_I64;
    f->field=(XrXirNominalField){{"secret",6},XR_XIR_I64,XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_MUTABLE};
    f->nominal=(XrXirNominalDeclaration){.module={"alpha",5},.name={"Record",6},.exported=1,
        .fields=&f->field,.field_count=1};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.fields=&f->field_type,.field_count=1}};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->method=(XrXirInterfaceMethod){{"measure",7},(XrXirType)257,0,0,NULL};
    f->protocols[0]=(XrXirInterfaceDeclaration){.module={"alpha",5},.name={"Measure",7},
        .exported=1,.methods=&f->method,.method_count=1};
    f->protocols[1]=f->protocols[0];f->protocols[1].name=(XrXirLiteral){"Decode",6};
    f->interfaces=(XrXirInterfaceTable){f->protocols,2};
    f->types=(XrXirTypes){f->nodes,2,&f->nominals,&f->interfaces};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->init=f->entry[1];f->generator[0]=f->entry[0];f->generator[1]=f->entry[1];
    f->body[0]=(XrXirInstruction){.op=XR_XIR_STRUCT_GET,.type=XR_XIR_I64};
    f->body[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    for (uint32_t i=0; i<4; ++i) f->blocks[i]=(XrXirBlock){0,i==1?1:2,0,0};
    f->functions[0]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,&f->blocks[0],1,f->entry,2,NULL,0};
    f->functions[1]=(XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&f->blocks[1],1,&f->init,1,NULL,0};
    f->functions[2]=(XrXirFunction){"measure",7,&f->receiver,1,XR_XIR_I64,&f->blocks[2],1,f->body,2,NULL,0};
    f->functions[3]=(XrXirFunction){"generator",9,NULL,0,XR_XIR_I64,&f->blocks[3],1,f->generator,2,NULL,0};
    f->identities[0].exported=1;f->identities[2]=(XrXirFunctionIdentity){.exported=1,.nominal_owner=1,
        .method_kind=XR_XIR_READ_METHOD,.derivation=1};
    f->identities[3].exported=1;f->source=(XrXirSourceModule){"alpha",5,NULL,0,1};
    f->witness=(XrXirImplementationBinding){{0,NULL,0},0,2};
    f->implementation=(XrXirImplementation){0,{0,NULL,0},&f->witness,1,1};
    f->implementations=(XrXirImplementationTable){&f->implementation,1};
    f->member=(XrXirDerivationMember){0,XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ};
    for (uint32_t i=0; i<4; ++i) f->sites[i]=(XrXirGeneratedOrigin){
        {"alpha",5},{"alpha/record.xr",15},{"Record.secret",13},23+i,7};
    f->policy=(XrXirDerivationPolicy){0,(XrXirType)256,{0,NULL,0},3,0,&f->member,1,f->sites[0]};
    f->binding=(XrXirDerivationGrant){2,0,0,0,(XrXirType)256,{f->sites[0],f->sites,2}};
    f->grants=(XrXirDerivationTable){&f->policy,1,&f->binding,1};
    f->declarations=(XrXirDeclarations){&f->source,1,f->identities,NULL,0,NULL,0,0,0,&f->implementations,&f->grants};
    f->built=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.types=&f->types};
}
static XrXirCompileContext derive_context(void) {
    XrXirCompileContext context={0};XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    derive_baseline_live=stats.live_bytes;
    context.limits=xr_xir_compile_default_limits();return context;
}
static void derive_free_context(XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes==derive_baseline_live);xr_compile_resources_release(context->resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void derive_positive(void) {
    XrXirCompileContext context=derive_context();DeriveFixture f;derive_fixture(&f);
    XrXirArtifact *checked=NULL,*received=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&f.built,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"check=%u function=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);memset(&f,0xa5,sizeof(f));
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    CHECK(xr_xir_compile_nominal_access(&context,module,2,(XrXirType)256,0,XR_XIR_NOMINAL_READ)==XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&context,module,2,(XrXirType)256,0,XR_XIR_NOMINAL_WRITE)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_nominal_access(&context,module,2,(XrXirType)256,0,XR_XIR_NOMINAL_CONSTRUCT)==XR_XIR_BAD_TYPE);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if (packet.length!=sizeof(derivation74_golden) || memcmp(packet.bytes,derivation74_golden,packet.length)) {
        size_t at=0;while(at<packet.length && at<sizeof(derivation74_golden) && packet.bytes[at]==derivation74_golden[at])++at;
        fprintf(stderr,"independent grant fields actual=%zu expected=%zu first=%zu\n",packet.length,sizeof(derivation74_golden),at);
    }
    CHECK(packet.length==sizeof(derivation74_golden) && !memcmp(packet.bytes,derivation74_golden,packet.length));
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    const XrXirDerivationTable *owned=xr_xir_compile_artifact_module(received)->declarations->derivations;
    CHECK(owned && owned->policy_count==1 && owned->count==1);
    CHECK(owned->records[0].origins.instructions[1].line==24);
    CHECK(owned->policies[0].members[0].operations==3);
    xr_xir_compile_artifact_free(received);derive_free_context(&context);
}
static void derive_negative(void) {
    XrXirCompileContext context=derive_context();
    for (unsigned kind=0; kind<12; ++kind) {
        DeriveFixture f;derive_fixture(&f);XrXirArtifact *checked=NULL;
        XrXirSlot slot={0,XR_XIR_I64,0};
        if (kind==0) f.member.operations=XR_XIR_DERIVE_DESCRIBE;
        if (kind==1) f.policy.grantor_module=1;
        if (kind==2) f.implementation.derivation_policy=0;
        if (kind==3) {f.implementation.interface.declaration=1;f.witness.requirement.declaration=1;}
        if (kind==4) f.binding.root=1;
        if (kind==5) {f.field.flags=XR_XIR_FIELD_PRIVATE;f.member.operations=XR_XIR_DERIVE_WRITE;}
        if (kind==6) {
            f.identities[3].exported=0;
            f.body[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=3};
        }
        if (kind==7) f.binding.origins.instruction_count=1;
        if (kind==8) f.binding.source_row=1;
        if (kind==9) {
            f.body[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
            f.body[1]=(XrXirInstruction){.op=XR_XIR_STRUCT_SET,.args={0,1}};
            f.body[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
            f.functions[2].instruction_count=3;f.blocks[2].count=3;
            f.binding.origins.instruction_count=3;
        }
        if (kind==10) {
            f.body[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
            f.body[1]=(XrXirInstruction){.op=XR_XIR_STRUCT_NEW,.type=(XrXirType)256,.args={0,1}};
            f.body[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
            f.functions[2].instruction_count=3;f.blocks[2].count=3;
            f.operands[0]=1;f.functions[2].operands=f.operands;f.functions[2].operand_count=1;
            f.binding.origins.instruction_count=3;
        }
        if (kind==11) {
            f.declarations.slots=&slot;f.declarations.slot_count=1;
            f.factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
            f.factory[1]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={0,0}};
            f.factory[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
            f.functions[1].instructions=f.factory;f.functions[1].instruction_count=3;f.blocks[1].count=3;
            f.body[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64};
        }
        XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(&context,&f.built,&checked,&diagnostic);
        if (status==XR_XIR_OK) fprintf(stderr,"unexpected admission kind=%u\n",kind);
        CHECK(status!=XR_XIR_OK && !checked);
        if (kind==0 || kind==6 || kind==11) CHECK(status==XR_XIR_BAD_TYPE && diagnostic.function==2 && diagnostic.instruction==0);
        if (kind==9 || kind==10) CHECK(status==XR_XIR_BAD_TYPE && diagnostic.function==2 && diagnostic.instruction==1);
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==derive_baseline_live);
        derive_fixture(&f);CHECK(xir_fixture_check(&context,&f.built,&checked,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);
    }
    derive_free_context(&context);
}
static void derive_projection(const char *output_path) {
    XrXirCompileContext context=derive_context();DeriveFixture f;derive_fixture(&f);
    if (output_path) {
        f.nominal.flags=XR_XIR_NOMINAL_MODULE_LITERAL;
        f.factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
        f.factory[1]=(XrXirInstruction){.op=XR_XIR_STRUCT_NEW,.type=(XrXirType)256,.args={0,1}};
        f.factory[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
        f.blocks[4]=(XrXirBlock){0,3,0,0};f.operands[0]=0;
        f.functions[4]=(XrXirFunction){"make",4,NULL,0,(XrXirType)256,&f.blocks[4],1,f.factory,3,f.operands,1};
        f.identities[4]=(XrXirFunctionIdentity){.exported=1,.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR};
        f.built.function_count=5;
        f.entry[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)256,.immediate=4};
        f.entry[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=2};
        f.entry[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
        f.functions[0].instruction_count=3;f.blocks[0].count=3;
        f.functions[0].operands=f.operands;f.functions[0].operand_count=1;
    }
    XrXirArtifact *checked=NULL,*specialized=NULL,*lowered=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(&context,&f.built,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"projection Built=%u function=%u block=%u instruction=%u reason=%u\n",
        (unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK && checked);
    status=xr_xir_compile_specialize(checked,&specialized,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"specialize=%u function=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && specialized);xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(specialized);
    CHECK(module->declarations->derivations && !module->declarations->derivations->policy_count);
    CHECK(module->declarations->derivations->count==1);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(specialized,&target,&lowered,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"lower=%u function=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && lowered);xr_xir_compile_artifact_free(specialized);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    if (output_path) {
        XrXirCSource source={0};status=xr_xir_compile_emit_c(lowered,"derive_authority",8388608,&source);
        if(status!=XR_XIR_OK)fprintf(stderr,"emit=%u\n",(unsigned)status);
        CHECK(status==XR_XIR_OK && source.text && source.length);
        CHECK(!strstr(source.text,"({"));
        FILE *file=fopen(output_path,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
        xr_xir_compile_c_source_free(&source);
    }
    xr_xir_compile_artifact_free(lowered);derive_free_context(&context);
}
static void derive_ordered_instance(void) {
    XrXirCompileContext context=derive_context();DeriveFixture f;derive_fixture(&f);
    XrXirType parameters[]={(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
        (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1)};
    XrXirType forward[]={XR_XIR_I64,XR_XIR_STRING},reverse[]={XR_XIR_STRING,XR_XIR_I64};
    XrXirConstraint constraints[2]={{0},{0}};XrXirGeneric generics[4]={{0}};
    f.nominal.parameter_count=2;f.nominal.constraints=constraints;
    f.field.type=parameters[0];f.field_type=parameters[0];
    f.nodes[0].parameter_span=2;f.nodes[0].nominal.arguments=parameters;f.nodes[0].nominal.argument_count=2;
    f.nodes[1].result=parameters[0];f.nodes[1].parameter_span=1;
    f.protocols[0].parameter_count=1;f.protocols[0].constraints=constraints;
    f.protocols[1].parameter_count=1;f.protocols[1].constraints=constraints;
    f.witness.requirement.arguments=parameters;f.witness.requirement.argument_count=1;
    f.implementation.interface=f.witness.requirement;f.policy.protocol=f.witness.requirement;
    f.functions[2].result=parameters[0];f.body[0].type=parameters[0];
    generics[2].parameter_count=2;generics[2].constraints=constraints;
    XrXirType forward_field=XR_XIR_I64,reverse_field=XR_XIR_STRING,receiver=(XrXirType)258;
    f.nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={0,forward,2,&forward_field,1}};
    f.nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={0,reverse,2,&reverse_field,1}};
    f.types.count=4;f.functions[3].parameters=&receiver;f.functions[3].parameter_count=1;
    f.generator[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},
        .immediate=2,.type_arguments={0,2}};
    f.generator[1].args[0]=1;f.operands[0]=0;f.functions[3].operands=f.operands;f.functions[3].operand_count=1;
    generics[3].arguments=forward;generics[3].argument_count=2;f.built.generics=generics;
    XrXirArtifact *checked=NULL,*closed=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&f.built,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"ordered check=%u function=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    status=xr_xir_compile_specialize(checked,&closed,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"ordered specialize=%u function=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && closed);xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);
    const XrXirDerivationTable *table=module->declarations->derivations;
    CHECK(table && table->count==1 && !table->policy_count);
    const XrXirDerivationGrant *binding=&table->records[0];
    XrXirType wrong=XR_XIR_UNIT;
    for (uint32_t n=0; n<module->types->count; ++n) {
        const XrXirTypeNode *node=&module->types->nodes[n];
        if (node->kind==XR_XIR_TYPE_NOMINAL && node->nominal.argument_count==2 &&
            node->nominal.arguments[0]==XR_XIR_STRING && node->nominal.arguments[1]==XR_XIR_I64)
            wrong=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);
    }
    CHECK(wrong!=XR_XIR_UNIT && wrong!=binding->target);
    CHECK(xr_xir_compile_nominal_access(&context,module,binding->function,binding->target,0,XR_XIR_NOMINAL_READ)==XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_access(&context,module,binding->function,wrong,0,XR_XIR_NOMINAL_READ)==XR_XIR_BAD_TYPE);
    XrXirModule malformed=*module;XrXirDerivationTable forged=*table;
    XrXirDerivationGrant record=*binding;XrXirDeclarations declarations=*module->declarations;
    record.target=wrong;forged.records=&record;declarations.derivations=&forged;malformed.declarations=&declarations;
    CHECK(xir_derivations_verify(&context,&malformed)==XR_XIR_BAD_TYPE);
    xr_xir_compile_artifact_free(closed);derive_free_context(&context);
}
static void derive_previous_schema(void) {
    const uint8_t *const vectors[]={grant74_previous_0,grant74_previous_1,grant74_previous_2,
        grant74_previous_3,grant74_previous_4,grant74_previous_5,grant74_previous_6,
        grant74_previous_7,grant74_previous_8,grant74_previous_9,grant74_previous_10,
        grant74_previous_11,grant74_previous_12};
    const size_t sizes[]={sizeof(grant74_previous_0),sizeof(grant74_previous_1),sizeof(grant74_previous_2),
        sizeof(grant74_previous_3),sizeof(grant74_previous_4),sizeof(grant74_previous_5),
        sizeof(grant74_previous_6),sizeof(grant74_previous_7),sizeof(grant74_previous_8),
        sizeof(grant74_previous_9),sizeof(grant74_previous_10),sizeof(grant74_previous_11),sizeof(grant74_previous_12)};
    XrXirCompileContext context=derive_context();
    for (uint32_t i=0; i<13; ++i) for (unsigned occupied=0; occupied<2; ++occupied) {
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *output=occupied?(XrXirArtifact *)(uintptr_t)1:NULL,*saved=output;
        CHECK(xr_xir_compile_checked_read(&context,vectors[i],sizes[i],&output,NULL)==XR_XIR_BAD_STRUCTURE && output==saved);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes &&
            after.work-before.work==(occupied?0u:32u));
    }
    derive_free_context(&context);
}
static void derive_constructor_policy(void) {
    XrXirCompileContext context=derive_context();
    for (unsigned authorized=0; authorized<2; ++authorized) {
        DeriveFixture f;derive_fixture(&f);f.nominal.flags=XR_XIR_NOMINAL_MODULE_LITERAL;
        f.factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
        f.factory[1]=(XrXirInstruction){.op=XR_XIR_STRUCT_NEW,.type=(XrXirType)256,.args={0,1}};
        f.factory[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
        f.blocks[4]=(XrXirBlock){0,3,0,0};f.operands[0]=0;
        f.functions[4]=(XrXirFunction){"make",4,NULL,0,(XrXirType)256,&f.blocks[4],1,f.factory,3,f.operands,1};
        f.identities[4]=(XrXirFunctionIdentity){.nominal_owner=1,.member_access=XR_XIR_MEMBER_PRIVATE,.method_kind=XR_XIR_CONSTRUCTOR};
        XrXirInstruction helper[]={
            {.op=XR_XIR_CALL,.type=(XrXirType)256,.immediate=4},
            {.op=XR_XIR_STRUCT_GET,.type=XR_XIR_I64},
            {.op=XR_XIR_RETURN,.args={1,0}}};
        f.blocks[5]=(XrXirBlock){0,3,0,0};
        f.functions[5]=(XrXirFunction){"generated_helper",16,NULL,0,XR_XIR_I64,&f.blocks[5],1,helper,3,NULL,0};
        f.identities[5].derivation=2;f.built.function_count=6;
        f.body[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=5};
        XrXirDerivationGrant bindings[]={f.binding,{5,0,1,0,(XrXirType)256,{f.sites[0],f.sites,3}}};
        f.grants.records=bindings;f.grants.count=2;f.policy.constructor=authorized?5:0;
        XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xir_fixture_check(&context,&f.built,&checked,&diagnostic);
        if (!authorized) CHECK(status==XR_XIR_BAD_TYPE && !checked && diagnostic.function==5 && diagnostic.instruction==0);
        else {
            if(status!=XR_XIR_OK)fprintf(stderr,"authorized constructor check=%u function=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
            CHECK(status==XR_XIR_OK && checked);
            CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
            const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
            CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
            CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(lowered);
        }
    }
    derive_free_context(&context);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || (argc==3 && !strcmp(argv[1],"--emit-native")));
    if (!strcmp(argv[1],"positive")) derive_positive();
    else if (!strcmp(argv[1],"negative")) derive_negative();
    else if (!strcmp(argv[1],"projection")) derive_projection(NULL);
    else if (!strcmp(argv[1],"--emit-native")) derive_projection(argv[2]);
    else if (!strcmp(argv[1],"ordered_instance")) derive_ordered_instance();
    else if (!strcmp(argv[1],"previous_schema")) derive_previous_schema();
    else if (!strcmp(argv[1],"constructor_policy")) derive_constructor_policy();
    else CHECK(false);
    return 0;
}
