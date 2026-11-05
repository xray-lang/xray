/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_marker_read.c - Callback-free marker attempts on actual Input
 */
#include "xir_read_workspace_probe.h"

typedef struct MarkerReadFixture {
    XrXirTypeNode nodes[2];
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirTypes types;
    XrXirInstruction initialize[6],read[4];
    XrXirBlock blocks[2];
    XrXirFunction functions[2];
    XrXirFunctionIdentity identities[2];
    XrXirSourceModule source;
    XrXirSlot slots[2];
    XrXirDeclarations declarations;
    XrXirModule module;
} MarkerReadFixture;
enum {MR_ARRAY,MR_EMPTY_STRUCT,MR_CLASS,MR_MUTABLE_ARRAY};
static void mr_fixture(MarkerReadFixture *f,unsigned shape) {
    bool nominal=shape==MR_EMPTY_STRUCT || shape==MR_CLASS;
    memset(f,0,sizeof(*f));XrXirType type=XR_XIR_CONSTRUCTED_TYPE_BASE;
    f->nominal=(XrXirNominalDeclaration){.module={"marker",6},.name={"Token",5},.kind=shape==MR_CLASS?XR_XIR_NOMINAL_CLASS:XR_XIR_NOMINAL_STRUCT};
    f->nominals=(XrXirNominalTable){.declarations=&f->nominal,.count=1};
    f->nodes[0]=nominal?(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL}:
        (XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=type};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=shape==MR_MUTABLE_ARRAY?2u:1u,.nominals=nominal?&f->nominals:NULL};uint32_t at=0;
    f->initialize[at++]=(XrXirInstruction){.op=shape==MR_CLASS?XR_XIR_CLASS_NEW:shape==MR_EMPTY_STRUCT?XR_XIR_STRUCT_NEW:XR_XIR_ARRAY_NEW,.type=type};
    f->initialize[at++]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={0,0},.immediate=0};
    if(shape==MR_MUTABLE_ARRAY) {
        f->initialize[at++]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=type};
        f->initialize[at++]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=type+1,.args={2,0}};
        f->initialize[at++]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={3,0},.immediate=1};
    }
    f->initialize[at++]=(XrXirInstruction){.op=XR_XIR_RETURN};
    uint32_t read=0;
    f->read[read++]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=type,.immediate=0};
    /* The const query exercises the actual prepared marker path. The mutable
     * Array independently requires ROOT without being a non-Sendable type. */
    if(shape==MR_MUTABLE_ARRAY)
        f->read[read++]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=type+1,.immediate=1};
    f->read[read]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};++read;
    f->read[read]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={read-1,0}};++read;
    f->blocks[0]=(XrXirBlock){.count=at};f->blocks[1]=(XrXirBlock){.count=read};
    f->functions[0]=(XrXirFunction){.error_type=XR_XIR_NEVER,.name="init",.name_length=4,.instructions=f->initialize,
        .instruction_count=at,.blocks=&f->blocks[0],.block_count=1};
    f->functions[1]=(XrXirFunction){.error_type=XR_XIR_NEVER,.name="read",.name_length=4,.result=XR_XIR_I64,.instructions=f->read,
        .instruction_count=read,.blocks=&f->blocks[1],.block_count=1};
    f->source=(XrXirSourceModule){.name="marker",.name_length=6};f->slots[0]=(XrXirSlot){.type=type};f->slots[1]=(XrXirSlot){.type=type+1,.mutable=true};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=f->slots,.slot_count=shape==MR_MUTABLE_ARRAY?2u:1u,.root_module=0,.entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=2,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
static EffectBodyMaterial *mr_body(const XrXirCompileContext *work,unsigned shape) {
    MarkerReadFixture f;mr_fixture(&f,shape);XrXirArtifact *checked=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=XR_XIR_OK;
    if(shape==MR_MUTABLE_ARRAY) {
        /* These are two distinct synthetic declarations. The one-slot
         * convenience fixture intentionally supplies only one private name. */
        XrXirConstruction *nominal=NULL,*facts=NULL;
        status=xir_fixture_construction(work,f.module.types,&nominal);
        XrXirSlotFact rows[2]={{0,"const_probe",11,0},{1,"mutable_probe",13,0}};
        XrXirSlotFacts slots={rows,2};
        XrXirConstructionSpec input={nominal?nominal->rows:NULL,nominal?nominal->count:0,NULL,0,NULL};
        if(status==XR_XIR_OK)status=xr_xir_compile_construction_new_v4(work,f.module.types,&input,&slots,&facts,NULL);
        if(status==XR_XIR_OK)status=xr_xir_compile_check_v2(work,&f.module,facts,&checked,&diagnostic);
        xr_xir_compile_construction_free(facts);xr_xir_compile_construction_free(nominal);
    } else status=xir_fixture_check(work,&f.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"marker fixture check shape%u status%u f%u i%u reason%u\n",
        shape,status,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK && checked);EffectBodyMaterial *body=NULL;
    CHECK(effect_body_material_create(work,xr_xir_compile_artifact_module(checked),&body)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&f,0xD7,sizeof(f));return body;
}
static void mr_literal(const EffectInvocationPrimitive *value,unsigned shape) {
    XrXirRootEffectCause cause=shape==MR_CLASS?XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST:
        shape==MR_MUTABLE_ARRAY?XR_XIR_ROOT_CAUSE_MUTABLE_SLOT:XR_XIR_ROOT_CAUSE_NONE;
    CHECK(value->facts.requires_root==(cause!=XR_XIR_ROOT_CAUSE_NONE) && !value->facts.unresolved);
    CHECK(value->witnesses[0].cause==cause);
    if(cause!=XR_XIR_ROOT_CAUSE_NONE) CHECK(value->witnesses[0].instruction==(shape==MR_MUTABLE_ARRAY?1u:0u) &&
        value->witnesses[0].slot==(shape==MR_MUTABLE_ARRAY?1u:0u) && value->witnesses[0].callee==UINT32_MAX &&
        !value->witnesses[0].distance);
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
static void mr_input_cases(unsigned shape) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).held_bytes;
    EffectBodyMaterial *body=mr_body(&context,shape);uint64_t held=pc_stats(&context).held_bytes;
    size_t before=attempts;EffectInvocationPrimitive value={0};
    CHECK(effect_invocation_primitive_read(&context,body,1,&value, NULL)==XR_XIR_OK);mr_literal(&value,shape);
    size_t sites=attempts-before;CHECK(sites && sites<64 && pc_stats(&context).held_bytes==held);
    const XrXirTypeNode *nodes=body->terms.types.nodes;EffectBodyRun *runs=body->functions[1].runs;
    uint32_t functions=body->domain.function_count;mr_attacked=body;
    for(mr_kind=0;mr_kind<4;++mr_kind) {
        mr_callbacks=0;unsigned char untouched[sizeof(value)];memset(&value,0xC9,sizeof(value));
        memcpy(untouched,&value,sizeof(value));
        if(mr_kind<2) pc_allocate_hook=mr_mutate;else pc_free_hook=mr_mutate;
        XrXirStatus status=effect_invocation_primitive_read(&context,body,1,&value, NULL);
        pc_allocate_hook=NULL;pc_free_hook=NULL;
        body->domain.function_count=functions;body->terms.types.nodes=nodes;body->functions[1].runs=runs;
        CHECK(status==XR_XIR_BAD_STRUCTURE && mr_callbacks==1 && !memcmp(untouched,&value,sizeof(value)));
        CHECK(pc_stats(&context).held_bytes==held);
        CHECK(effect_invocation_primitive_read(&context,body,1,&value, NULL)==XR_XIR_OK);mr_literal(&value,shape);
    }
    mr_attacked=NULL;
    for(size_t site=0;site<sites;++site) {
        fail_at=attempts+site;injected=false;memset(&value,0xCA,sizeof(value));
        unsigned char untouched[sizeof(value)];memcpy(untouched,&value,sizeof(value));
        XrXirStatus status=effect_invocation_primitive_read(&context,body,1,&value, NULL);fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && !memcmp(untouched,&value,sizeof(value)));
        CHECK(pc_stats(&context).held_bytes==held);
        CHECK(effect_invocation_primitive_read(&context,body,1,&value, NULL)==XR_XIR_OK);mr_literal(&value,shape);
    }
    effect_body_material_free(body);pc_release(&context,baseline);
    printf("ROOT marker Input shape%u: four real allocation/release attacks, %zu complete allocation failures, untouched output, same-owner retry, producer death, physical0\n",shape,sites);
}
#include "xir_root_marker_generic_cases.h"
typedef struct MarkerParentProbe {
    XirReadOperation *operation;
    XrXirArtifact *input;
    unsigned action;
    size_t polls;
} MarkerParentProbe;
static bool mr_parent_poll(const void *opaque) {
    MarkerParentProbe *probe=(MarkerParentProbe *)opaque;++probe->polls;
    CHECK(!probe->operation->capacity.active && probe->operation->capacity.required);
    if(probe->action==1) return true;
    if(probe->action==2) {
        ((XrXirInstruction *)probe->input->module.functions[0].instructions)[0].op=XR_XIR_INVALID;
        probe->action=0;
    } else if(probe->action==3) {
        fail_at=attempts;probe->action=0;
    }
    return false;
}
static void mr_parent_attempts(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t empty=pc_stats(&context).held_bytes;
    MarkerReadFixture fixture;mr_fixture(&fixture,MR_ARRAY);
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,&diagnostic)==XR_XIR_OK);
    memset(&fixture,0xD7,sizeof(fixture));uint64_t held=pc_stats(&context).held_bytes;
    XrXirInstruction original=checked->module.functions[0].instructions[0];
    for(unsigned action=0;action<5;++action) {
        XirReadOperation operation={.capacity={.resources=context.resources}};
        MarkerParentProbe probe={&operation,checked,action,0};
        XrCompileRequestOptions options={.cancel_poll=mr_parent_poll,.cancel_context=&probe};
        if(action==4) {options.deadline_mode=XR_COMPILE_DEADLINE_AFTER;options.timeout_ns=0;}
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XirReadRequest request={.operation=&operation,.control=&control};XrXirEffects *effects=NULL;
        injected=false;
        XrXirStatus status=xr_xir_compile_effects_infer_verified(&context,&checked->module,&effects,&request);
        fail_at=SIZE_MAX;
        ((XrXirInstruction *)checked->module.functions[0].instructions)[0]=original;
        if(!action) {
            CHECK(status==XR_XIR_OK && effects && probe.polls && operation.capacity.preparations &&
                operation.capacity.blocks && operation.capacity.claims && !operation.capacity.active);
            XrXirEffectFacts result=xr_xir_effects_read(effects,1);
            CHECK(result.status==XR_XIR_OK && !result.root.requires_root && !result.root.unresolved);
            printf("ROOT parent complete retry: polls%zu prepares%zu blocks%zu bytes%zu claims%zu clearBytes%zu frame%llu peakHeld%llu\n",
                probe.polls,operation.capacity.preparations,operation.capacity.blocks,operation.capacity.capacity,
                operation.capacity.claims,operation.capacity.clear_bytes,(unsigned long long)context.limits.frame_bytes,
                (unsigned long long)pc_stats(&context).peak_bytes);
        } else {
            CHECK(!effects);
            if(action==1) CHECK(status==XR_XIR_CANCELLED && probe.polls==1);
            else if(action==2) CHECK(status==XR_XIR_BAD_STRUCTURE && probe.polls);
            else if(action==3) CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && probe.polls);
            else CHECK(status==XR_XIR_DEADLINE);
        }
        xr_xir_compile_effects_free(effects);xir_read_operation_free(&operation);
        CHECK(pc_stats(&context).held_bytes==held);
    }
    /* Every failed operation uses a new request and the original complete
     * program. The earlier initializer was not a reusable semantic prefix. */
    XirReadOperation recovery={.capacity={.resources=context.resources}};
    MarkerParentProbe probe={&recovery,checked,0,0};
    XrCompileRequestOptions options={.cancel_poll=mr_parent_poll,.cancel_context=&probe};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XirReadRequest request={.operation=&recovery,.control=&control};XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_infer_verified(&context,&checked->module,&effects,&request)==XR_XIR_OK && effects);
    xr_xir_compile_effects_free(effects);xir_read_operation_free(&recovery);
    CHECK(pc_stats(&context).held_bytes==held);xr_xir_compile_artifact_free(checked);pc_release(&context,empty);
    puts("ROOT parent: cancel/deadline, earlier-function mutation, real prepare OOM, complete recovery, physical0");
}
#include "xir_read_workspace_terms_cases.h"
int main(int argc,char **argv) {
    CHECK(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);CHECK(mode<8);
    if(mode<2) mr_input_cases(mode);else if(mode<4)mr_generic_cases(mode==3);
    else if(mode<6)mr_input_cases(mode-2);else if(mode==6)mr_parent_attempts();else mr_terms_cases();return 0;
}
