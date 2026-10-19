/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_marker_read.c - Callback-free marker attempts on actual Input
 */
#define main root_marker_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main

typedef struct MarkerReadFixture {
    XrXirTypeNode nodes[1];
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirTypes types;
    XrXirInstruction initialize[4],read[3];
    XrXirBlock blocks[2];
    XrXirFunction functions[2];
    XrXirFunctionIdentity identities[2];
    XrXirSourceModule source;
    XrXirSlot slot;
    XrXirDeclarations declarations;
    XrXirModule module;
} MarkerReadFixture;
static void mr_fixture(MarkerReadFixture *f,bool non_sendable) {
    memset(f,0,sizeof(*f));XrXirType type=XR_XIR_CONSTRUCTED_TYPE_BASE;
    f->nominal=(XrXirNominalDeclaration){.module={"marker",6},.name={"Token",5},.kind=XR_XIR_NOMINAL_STRUCT};
    f->nominals=(XrXirNominalTable){.declarations=&f->nominal,.count=1};
    f->nodes[0]=non_sendable?(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL}:
        (XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=1,.nominals=non_sendable?&f->nominals:NULL};uint32_t at=0;
    f->initialize[at++]=(XrXirInstruction){.op=non_sendable?XR_XIR_STRUCT_NEW:XR_XIR_ARRAY_NEW,.type=type};
    f->initialize[at++]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={0,0},.immediate=0};
    f->initialize[at++]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->read[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=type,.immediate=0};
    f->read[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->read[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->blocks[0]=(XrXirBlock){.count=at};f->blocks[1]=(XrXirBlock){.count=3};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.instructions=f->initialize,
        .instruction_count=at,.blocks=&f->blocks[0],.block_count=1};
    f->functions[1]=(XrXirFunction){.name="read",.name_length=4,.result=XR_XIR_I64,.instructions=f->read,
        .instruction_count=3,.blocks=&f->blocks[1],.block_count=1};
    f->source=(XrXirSourceModule){"marker",6,NULL,0,0};f->slot=(XrXirSlot){.type=type};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.root_module=0,.entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=2,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
static EffectBodyMaterial *mr_body(const XrXirCompileContext *work,bool non_sendable) {
    MarkerReadFixture f;mr_fixture(&f,non_sendable);XrXirArtifact *checked=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(work,&f.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"marker fixture check non_sendable%u status%u f%u i%u reason%u\n",
        non_sendable,status,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK && checked);EffectBodyMaterial *body=NULL;
    CHECK(effect_body_material_create(work,xr_xir_compile_artifact_module(checked),&body)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&f,0xD7,sizeof(f));return body;
}
static void mr_literal(const EffectInvocationPrimitive *value,bool non_sendable) {
    CHECK(value->facts.requires_root==non_sendable && !value->facts.unresolved);
    CHECK(value->witnesses[0].cause==(non_sendable?XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST:XR_XIR_ROOT_CAUSE_NONE));
    if(non_sendable) CHECK(!value->witnesses[0].instruction && !value->witnesses[0].slot &&
        value->witnesses[0].callee==UINT32_MAX && !value->witnesses[0].distance);
    CHECK(value->witnesses[1].cause==XR_XIR_ROOT_CAUSE_NONE);
}
static EffectBodyMaterial *mr_attacked;
static unsigned mr_kind,mr_callbacks;
static void mr_mutate(void *memory,size_t bytes) {
    (void)memory;(void)bytes;pc_allocate_hook=NULL;pc_free_hook=NULL;++mr_callbacks;
    if(!mr_kind) --mr_attacked->domain.function_count;
    else if(mr_kind<3) mr_attacked->terms.types.nodes=(const XrXirTypeNode *)(uintptr_t)1;
    else mr_attacked->functions[1].runs=(EffectBodyRun *)(uintptr_t)1;
}
static void mr_input_cases(bool non_sendable) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectBodyMaterial *body=mr_body(&context,non_sendable);uint64_t held=pc_stats(&context).live_bytes;
    size_t before=attempts;EffectInvocationPrimitive value={0};
    CHECK(effect_invocation_primitive_read(&context,body,1,&value)==XR_XIR_OK);mr_literal(&value,non_sendable);
    size_t sites=attempts-before;CHECK(sites && sites<64 && pc_stats(&context).live_bytes==held);
    const XrXirTypeNode *nodes=body->terms.types.nodes;EffectBodyRun *runs=body->functions[1].runs;
    uint32_t functions=body->domain.function_count;mr_attacked=body;
    for(mr_kind=0;mr_kind<4;++mr_kind) {
        mr_callbacks=0;unsigned char untouched[sizeof(value)];memset(&value,0xC9,sizeof(value));
        memcpy(untouched,&value,sizeof(value));
        if(mr_kind<2) pc_allocate_hook=mr_mutate;else pc_free_hook=mr_mutate;
        XrXirStatus status=effect_invocation_primitive_read(&context,body,1,&value);
        pc_allocate_hook=NULL;pc_free_hook=NULL;
        body->domain.function_count=functions;body->terms.types.nodes=nodes;body->functions[1].runs=runs;
        CHECK(status==XR_XIR_BAD_STRUCTURE && mr_callbacks==1 && !memcmp(untouched,&value,sizeof(value)));
        CHECK(pc_stats(&context).live_bytes==held);
        CHECK(effect_invocation_primitive_read(&context,body,1,&value)==XR_XIR_OK);mr_literal(&value,non_sendable);
    }
    mr_attacked=NULL;
    for(size_t site=0;site<sites;++site) {
        fail_at=attempts+site;injected=false;memset(&value,0xCA,sizeof(value));
        unsigned char untouched[sizeof(value)];memcpy(untouched,&value,sizeof(value));
        XrXirStatus status=effect_invocation_primitive_read(&context,body,1,&value);fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && !memcmp(untouched,&value,sizeof(value)));
        CHECK(pc_stats(&context).live_bytes==held);
        CHECK(effect_invocation_primitive_read(&context,body,1,&value)==XR_XIR_OK);mr_literal(&value,non_sendable);
    }
    effect_body_material_free(body);pc_release(&context,baseline);
    printf("ROOT marker Input non_sendable%u: four real allocation/release attacks, %zu complete allocation failures, untouched output, same-owner retry, producer death, physical0\n",non_sendable,sites);
}
#include "xir_root_marker_generic_cases.h"
int main(int argc,char **argv) {
    CHECK(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);CHECK(mode<4);
    if(mode<2) mr_input_cases(mode!=0);else mr_generic_cases(mode==3);return 0;
}
