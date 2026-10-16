/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_timed_wire.c - Independent timed packets and exact operand refusals
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_construction_fixture.h"
#include "shared/xnative_declaration.h"
#include "xir_channel_timed76_golden.h"
#include "xir_channel_blocking75_golden.h"
typedef struct TimedWireFixture {
    XrXirConstraint constraint;XrXirNominalField field;
    XrXirNominalVariant recv[4],send[4];XrXirNominalDeclaration declarations[2];
    XrXirNominalTable table;XrXirType element,parameters[3];XrXirTypeNode nodes[3];XrXirTypes types;
    XrXirInstruction instructions[2];uint32_t operands[4];XrXirBlock block;
    XrXirFunction functions[2];XrXirInstruction initialize;XrXirBlock init_block;
    XrXirSourceModule source;XrXirFunctionIdentity identities[2];XrXirDeclarations source_declarations;XrXirModule module;
} TimedWireFixture;
static void timed_wire_init(TimedWireFixture *f,bool send,bool unit) {
    *f=(TimedWireFixture){0};const char *module="stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def";
    const char *recv[]={"Value","Empty","Timeout","Closed"},*sent[]={"Sent","Full","Timeout","Closed"};
    for (unsigned i=0;i<4;++i) {
        f->recv[i]=(XrXirNominalVariant){{recv[i],(uint32_t)strlen(recv[i])},i ? 1u : 0u,i ? 0u : 1u};
        f->send[i]=(XrXirNominalVariant){{sent[i],(uint32_t)strlen(sent[i])},0,0};
    }
    f->field=(XrXirNominalField){{"value",5},(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,0};
    f->declarations[0]=(XrXirNominalDeclaration){.module={module,(uint32_t)strlen(module)},.name={"Recv",4},.exported=1,
        .constraints=&f->constraint,.parameter_count=1,.fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_ENUM,
        .variants=f->recv,.variant_count=4,.native={.native_id=XR_NATIVE_DECLARATION_RECV}};
    f->declarations[1]=(XrXirNominalDeclaration){.module={module,(uint32_t)strlen(module)},.name={"SendResult",10},.exported=1,
        .kind=XR_XIR_NOMINAL_ENUM,.variants=f->send,.variant_count=4,.native={.native_id=XR_NATIVE_DECLARATION_SEND_RESULT}};
    for (unsigned i=0;i<2;++i) memcpy(f->declarations[i].native.source_fingerprint,
        xr_native_declaration_by_id(f->declarations[i].native.native_id)->source_fingerprint.bytes,32);
    f->table=(XrXirNominalTable){f->declarations,2,NULL};f->element=unit ? XR_XIR_UNIT : XR_XIR_I64;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CHANNEL,.element=f->element};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,&f->element,1,NULL,0}};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1}};
    f->types=(XrXirTypes){f->nodes,3,&f->table,NULL};
    uint32_t count=send && !unit ? 3u : 2u;f->parameters[0]=(XrXirType)256;
    for (uint32_t i=1;i<count;++i) f->parameters[i]=XR_XIR_I64;
    for (uint32_t i=0;i<count;++i) f->operands[i]=i;
    XrXirType result=(XrXirType)(send ? 258 : 257);
    f->instructions[0]=(XrXirInstruction){.op=send ? XR_XIR_CHANNEL_SEND_TIMEOUT : XR_XIR_CHANNEL_RECV_TIMEOUT,.type=result,.args={0,count}};
    f->instructions[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={count,0}};
    f->block=(XrXirBlock){.count=2};f->functions[1]=(XrXirFunction){.name="timedWire",.name_length=9,.parameters=f->parameters,
        .parameter_count=count,.result=result,.blocks=&f->block,.block_count=1,.instructions=f->instructions,.instruction_count=2,
        .operands=f->operands,.operand_count=count};
    f->initialize=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};f->init_block=(XrXirBlock){.count=1};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&f->init_block,.block_count=1,
        .instructions=&f->initialize,.instruction_count=1};
    f->source=(XrXirSourceModule){module,(uint32_t)strlen(module),NULL,0,0};f->identities[1].exported=1;
    f->source_declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=2,.types=&f->types,
        .declarations=&f->source_declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static void timed_wire_positive(LibraryCompileOwner *owner,unsigned variant) {
    const uint8_t *expected[]={timed_send_i6476_golden,timed_send_unit76_golden,timed_recv_i6476_golden,timed_recv_unit76_golden};
    const size_t lengths[]={sizeof(timed_send_i6476_golden),sizeof(timed_send_unit76_golden),sizeof(timed_recv_i6476_golden),sizeof(timed_recv_unit76_golden)};
    TimedWireFixture f;timed_wire_init(&f,variant<2,(variant&1)!=0);
    XrXirArtifact *checked=NULL,*read=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&owner->context,&f.module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Timed wire positive variant=%u status=%u f=%u i=%u\n",variant,status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if (packet.length!=lengths[variant] || memcmp(packet.bytes,expected[variant],packet.length<lengths[variant] ? packet.length : lengths[variant]))
        fprintf(stderr,"Timed wire independent packet variant=%u actual=%llu expected=%llu\n",variant,(unsigned long long)packet.length,(unsigned long long)lengths[variant]);
    CHECK(packet.length==lengths[variant] && !memcmp(packet.bytes,expected[variant],packet.length));
    CHECK(xr_xir_compile_checked_read(&owner->context,expected[variant],lengths[variant],&read,NULL)==XR_XIR_OK);
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
    const XrXirFunctionEffects *effect=xr_xir_effects_function(effects,1);const XrXirRootEffects *root=xr_xir_effects_root(effects,1);
    CHECK(effect && effect->suspend==XR_XIR_EFFECT_MAY && effect->throws==XR_XIR_EFFECT_NONE && root && !root->requires_root && !root->unresolved);
    xr_xir_compile_effects_free(effects);
    memset(&f,0xcc,sizeof(f));memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK && xr_xir_compile_artifact_verify(read,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(read);xr_xir_compile_artifact_free(checked);CHECK(!runtime_live && !runtime_bytes);
}
static void timed_wire_negative(LibraryCompileOwner *owner,unsigned variant,unsigned axis) {
    TimedWireFixture f;bool send=variant<2,unit=(variant&1)!=0;timed_wire_init(&f,send,unit);
    XrXirStatus expected=XR_XIR_BAD_TYPE;uint32_t count=f.functions[1].parameter_count;
    switch(axis) {
    case 0:f.parameters[count-1]=XR_XIR_BOOL;break;
    case 1:f.instructions[0].args[1]-=1;expected=XR_XIR_BAD_STRUCTURE;break;
    case 2:f.operands[0]=count-1;break;
    case 3:f.instructions[0].type=(XrXirType)(send ? 257 : 258);f.functions[1].result=f.instructions[0].type;break;
    case 4:f.declarations[send ? 1 : 0].native=(XrXirNominalNativeRecord){0};break;
    case 5:f.instructions[0].immediate=1;expected=XR_XIR_BAD_STRUCTURE;break;
    case 6:f.instructions[0].args[1]+=1;f.operands[count]=0;f.functions[1].operand_count=count+1;expected=XR_XIR_BAD_STRUCTURE;break;
    default:CHECK(send && !unit);f.parameters[1]=XR_XIR_STRING;break;
    }
    XrXirArtifact *checked=NULL;XrXirStatus status=xir_fixture_check(&owner->context,&f.module,&checked,NULL);
    if (status!=expected) fprintf(stderr,"Timed wire negative variant=%u axis=%u actual=%u expected=%u\n",variant,axis,status,expected);
    CHECK(status==expected && !checked && !runtime_live && !runtime_bytes);
}
static void timed_wire_previous(LibraryCompileOwner *owner) {
    const uint8_t *previous[]={blocking_send_i6475_golden,blocking_send_unit75_golden,blocking_recv_or_i6475_golden,blocking_recv_or_unit75_golden};
    const size_t lengths[]={sizeof(blocking_send_i6475_golden),sizeof(blocking_send_unit75_golden),sizeof(blocking_recv_or_i6475_golden),sizeof(blocking_recv_or_unit75_golden)};
    const uint8_t *current[]={retained_send_i6476_golden,retained_send_unit76_golden,retained_recv_or_i6476_golden,retained_recv_or_unit76_golden};
    const size_t current_lengths[]={sizeof(retained_send_i6476_golden),sizeof(retained_send_unit76_golden),sizeof(retained_recv_or_i6476_golden),sizeof(retained_recv_or_unit76_golden)};
    for (unsigned i=0;i<4;++i) {
        XrXirArtifact *checked=NULL;CHECK(xr_xir_compile_checked_read(&owner->context,current[i],current_lengths[i],&checked,NULL)==XR_XIR_OK && checked);
        xr_xir_compile_artifact_free(checked);
    }
    for (unsigned i=0;i<4;++i) for (unsigned occupied=0;occupied<2;++occupied) {
        XrXirArtifact *sentinel=occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL,*output=sentinel;
        size_t attempts=source_program_compile_attempts;
        CHECK(xr_xir_compile_checked_read(&owner->context,previous[i],lengths[i],&output,NULL)==XR_XIR_BAD_STRUCTURE && output==sentinel);
        CHECK(source_program_compile_attempts==attempts && !runtime_live && !runtime_bytes);
    }
}
int main(void) {
    _Static_assert(XR_XIR_CHANNEL_SEND_TIMEOUT==162 && XR_XIR_CHANNEL_RECV_TIMEOUT==163 && XR_XIR_CHECKED_CONTRACT==76,"independent timed wire ordinals");
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    timed_wire_previous(&owner);
    for (unsigned variant=0;variant<4;++variant) {
        timed_wire_positive(&owner,variant);
        for (unsigned axis=0;axis<(variant==0 ? 8u : 7u);++axis) timed_wire_negative(&owner,variant,axis);
    }
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts("Four independent timed76 full packets; old75 whole packets rejected before allocation;29 exact operand/type/authority controls physical0 PASS");return 0;
}
