/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_types.c - Borrowed descriptor boundaries and owned replay
 *
 * KEY CONCEPT:
 *   A valid signature does not authorize an executable borrowed value.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_view_types_internal.h"
#include "xir/xxir_type_inference.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_interface.h"
#include "xir/xxir_type_arena.h"
#include "tuple_owner_observer.h"
#include "xir_view_types85_golden.h"
#include "xir_view_ops85_golden.h"
#include "xir_checked_scalar84_golden.h"

static XrXirType view_type(uint32_t index) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+index);}
typedef struct ViewTypes {
    XrXirTypeNode nodes[12];XrXirCallableParameter fields[2],parameters[2];XrXirTypes types;
} ViewTypes;
static void view_types_init(ViewTypes *f) {
    *f=(ViewTypes){0};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=view_type(0)};
    f->fields[0]=(XrXirCallableParameter){view_type(2),0};
    f->fields[1]=(XrXirCallableParameter){view_type(1),0};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=2};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=view_type(4)};
    f->parameters[0]=(XrXirCallableParameter){view_type(1),XR_PARAM_READ};
    f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=f->parameters,
        .parameter_count=1,.result=view_type(3),.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->types=(XrXirTypes){f->nodes,7,NULL,NULL};
}
static void view_types_rules(const XrXirCompileContext *c) {
    ViewTypes f;view_types_init(&f);uint32_t output=77;
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    XrXirTypeArena *arena=NULL;uint64_t live_before=stats(c).live_bytes;
    CHECK(xr_xir_compile_type_arena_new(c,&f.types,&arena)==XR_XIR_VALUE_OK && arena);
    CHECK(xr_xir_compile_type_arena_has_borrowed(arena));
    CHECK(xr_xir_compile_type_arena_borrowed(arena,view_type(3))==3);
    CHECK(!xr_xir_compile_type_arena_borrowed(arena,view_type(6)));
    xr_xir_compile_type_arena_drop(arena);
    CHECK(stats(c).live_bytes==live_before);
    CHECK(xr_xir_compile_view_type_flags(c,&f.types,view_type(3),&output)==XR_XIR_OK && output==3);
    CHECK(xr_xir_compile_view_type_flags(c,&f.types,view_type(6),&output)==XR_XIR_OK && !output);
    CHECK(xr_xir_compile_view_return_candidate(c,&f.types,view_type(6),&output)==XR_XIR_OK && !output);
    f.parameters[0]=(XrXirCallableParameter){view_type(5),XR_PARAM_REF};
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    f.parameters[0]=(XrXirCallableParameter){view_type(0),XR_PARAM_READ};
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_TYPE);
    f.nodes[6].result=view_type(2);
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    f.parameters[0].type=view_type(2);
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    f.parameters[0].type=view_type(3);
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_TYPE);
    f.parameters[0].type=view_type(0);
    f.nodes[6].parameter_count=2;f.parameters[1]=f.parameters[0];
    output=77;
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_view_return_candidate(c,&f.types,view_type(6),&output)==XR_XIR_BAD_TYPE && output==77);
    f.nodes[6].parameter_count=0;f.nodes[6].parameters=NULL;
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_TYPE);
    f.nodes[6].result=XR_XIR_I64;
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_view_return_candidate(c,&f.types,view_type(6),&output)==XR_XIR_OK && output==UINT32_MAX);
    const uint32_t forbidden[]={XR_XIR_TYPE_ARRAY,XR_XIR_TYPE_CELL,XR_XIR_TYPE_SET,
        XR_XIR_TYPE_MAP,XR_XIR_TYPE_TASK,XR_XIR_TYPE_ATOMIC,XR_XIR_TYPE_SLICE,XR_XIR_TYPE_MUT_SLICE};
    for (uint32_t i=0;i<sizeof(forbidden)/sizeof(*forbidden);++i) {
        view_types_init(&f);f.types.count=8;
        f.nodes[7]=(XrXirTypeNode){.kind=forbidden[i],.element=view_type(2)};
        if (forbidden[i]==XR_XIR_TYPE_MAP) f.nodes[7].result=XR_XIR_I64;
        CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_TYPE);
    }
    view_types_init(&f);f.types.count=8;
    f.nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=view_type(6)};
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    f.nodes[0].flags=1;
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_STRUCTURE);
    view_types_init(&f);f.nodes[0].element=view_type(0);
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_TYPE);
    view_types_init(&f);f.nodes[1]=f.nodes[0];
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_BAD_STRUCTURE);
}
static void view_types_generic(const XrXirCompileContext *c) {
    ViewTypes f;view_types_init(&f);f.types.count=8;
    f.nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,
        .element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1};
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_expression_shape(c,&f.types,view_type(7),1)==XR_XIR_OK);
    const XrXirType argument=XR_XIR_I64;
    CHECK(xr_xir_compile_type_substitution_matches(c,&f.types,&argument,1,view_type(7),view_type(0))==XR_XIR_OK);
    XrXirInferenceRequest request={.types=&f.types,.own_count=1};
    XrXirInferenceState *state=NULL;XrXirType solved=XR_XIR_UNIT;
    CHECK(xr_xir_compile_inference_begin(c,&request,&state)==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_observe(state,&f.types,(XrXirInferencePair){view_type(7),view_type(0)})==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_finalize(state,&f.types,&solved,1)==XR_XIR_OK && solved==XR_XIR_I64);
    xr_xir_compile_inference_dispose(state);state=NULL;
    CHECK(xr_xir_compile_inference_begin(c,&request,&state)==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_observe(state,&f.types,
        (XrXirInferencePair){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,view_type(3)})==XR_XIR_BAD_TYPE);
    xr_xir_compile_inference_dispose(state);
    const XrXirConstraint empty={0};XrXirType application_argument=view_type(3);
    const XrXirInterfaceDeclaration declaration={.module={"m",1},.name={"Inspect",7},
        .parameter_count=1,.constraints=&empty};
    const XrXirInterfaceTable interfaces={&declaration,1};f.types.interfaces=&interfaces;
    const XrXirInterfaceApplication application={0,&application_argument,1};
    const XrXirConstraint requirement={.interfaces=&application,.interface_count=1};
    CHECK(xr_xir_compile_constraint_structure(c,&f.types,requirement,1)==XR_XIR_BAD_TYPE);
    application_argument=XR_XIR_I64;
    CHECK(xr_xir_compile_constraint_structure(c,&f.types,requirement,1)==XR_XIR_OK);
    const XrXirFunction function={0};const XrXirModule module={.functions=&function,.function_count=1,.types=&f.types};
    CHECK(xr_xir_compile_type_constraints(c,&module,0,view_type(3),empty)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_constraints(c,&module,0,XR_XIR_I64,empty)==XR_XIR_OK);
}
static void view_types_pipeline(const XrXirCompileContext *c) {
    ViewTypes f;view_types_init(&f);
    XrXirInstruction instruction={.op=XR_XIR_RETURN};XrXirBlock block={.count=1};
    XrXirFunction body={.name="viewTypes",.name_length=9,.blocks=&block,.block_count=1,
        .instructions=&instruction,.instruction_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&body,.function_count=1,.types=&f.types};
    XrXirArtifact *checked=NULL,*decoded=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    CHECK(xir_fixture_check(c,&module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.bytes[12]==85 && packet.bytes[8]==28);
    CHECK(packet.length==sizeof(view_types85_golden) && !memcmp(packet.bytes,view_types85_golden,packet.length));
    CHECK(xr_xir_compile_checked_read(c,view_types85_golden,sizeof(view_types85_golden),&decoded,NULL)==XR_XIR_OK);
    memset(&f,0xcc,sizeof(f));
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(decoded,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_checked_read(c,checked_scalar84_golden,sizeof(checked_scalar84_golden),&checked,NULL)==XR_XIR_BAD_STRUCTURE && !checked);
    view_types_init(&f);body.parameters=(XrXirType[]){view_type(0)};body.parameter_count=1;
    CHECK(xir_fixture_check(c,&module,&checked,NULL)==XR_XIR_BAD_TYPE && !checked);
}
static void view_ops_pipeline(const XrXirCompileContext *c) {
    const struct {const uint8_t *bytes;size_t length;XrXirStatus expected;} cases[]={
#define VIEW_PACKET(name,status) {view_ops85_##name,sizeof(view_ops85_##name),status}
        VIEW_PACKET(empty_cleanup,XR_XIR_OK),
        VIEW_PACKET(scalar_176,XR_XIR_BAD_TYPE),VIEW_PACKET(scalar_177,XR_XIR_BAD_TYPE),
        VIEW_PACKET(scalar_178,XR_XIR_BAD_TYPE),VIEW_PACKET(scalar_179,XR_XIR_BAD_TYPE),
        VIEW_PACKET(scalar_180,XR_XIR_BAD_TYPE),VIEW_PACKET(scalar_181,XR_XIR_BAD_TYPE),
        VIEW_PACKET(scalar_182,XR_XIR_BAD_TYPE),VIEW_PACKET(scalar_183,XR_XIR_BAD_TYPE),
        VIEW_PACKET(invalid_new_flag,XR_XIR_BAD_STRUCTURE)
#undef VIEW_PACKET
    };
    for(uint32_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        uint64_t baseline=stats(c).live_bytes;XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
        XrXirStatus status=xr_xir_compile_checked_read(c,cases[i].bytes,cases[i].length,&artifact,NULL);
        printf("view wire85 case=%u status=%u expected=%u\n",i,status,cases[i].expected);
        CHECK(status==cases[i].expected && (artifact!=NULL)==(status==XR_XIR_OK));
        if(artifact) {
            CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
            CHECK(packet.length==cases[i].length && !memcmp(packet.bytes,cases[i].bytes,packet.length));
            xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);
        }
        CHECK(stats(c).live_bytes==baseline);
    }
}
static uint64_t view_types_dag(const XrXirCompileContext *c) {
    XrXirTypeNode nodes[49]={0};XrXirCallableParameter fields[48][2]={0};
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    for (uint32_t i=1;i<49;++i) {
        fields[i-1][0]=(XrXirCallableParameter){view_type(i-1),0};fields[i-1][1]=fields[i-1][0];
        nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=fields[i-1],.parameter_count=2};
    }
    const XrXirTypes types={nodes,49,NULL,NULL};uint32_t flags=99;
    CHECK(xr_xir_compile_types_structure_verify(c,&types)==XR_XIR_OK);
    uint64_t before=stats(c).work;
    CHECK(xr_xir_compile_view_type_flags(c,&types,view_type(48),&flags)==XR_XIR_OK && flags==XR_XIR_VIEW_SHARED);
    uint64_t work=stats(c).work-before;CHECK(work<512);return work;
}
static void view_types_bounds(void) {
    ViewTypes f;view_types_init(&f);
    XrXirCompileContext c=owner_new(caps());uint32_t output=77;
    uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_view_type_flags(&c,&f.types,view_type(3),&output)==XR_XIR_OK && output==3);
    XrCompileResourceStats measured=stats(&c);owner_free(&c,baseline);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;
        else limits.work=measured.work-minus;
        c=owner_new(limits);output=77;
        baseline=stats(&c).live_bytes;
        XrXirStatus status=xr_xir_compile_view_type_flags(&c,&f.types,view_type(3),&output);
        CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK));CHECK(output==(minus?77u:3u));
        CHECK(stats(&c).work<=limits.work);owner_free(&c,baseline);
    }
    c=owner_new(caps());baseline=stats(&c).live_bytes;output=77;
    fail_at=attempts;injected=false;
    CHECK(xr_xir_compile_view_type_flags(&c,&f.types,view_type(3),&output)==XR_XIR_OUT_OF_MEMORY);
    CHECK(injected && output==77);fail_at=SIZE_MAX;owner_free(&c,baseline);
}
int main(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    view_types_rules(&c);view_types_generic(&c);view_types_pipeline(&c);view_ops_pipeline(&c);
    uint64_t work=view_types_dag(&c);owner_free(&c,baseline);view_types_bounds();
    printf("view descriptor rules, candidate origin, generic exclusion, owned replay and DAG work=%llu PASS\n",
        (unsigned long long)work);return 0;
}
