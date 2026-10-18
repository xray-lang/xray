/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_iterator_types.c - Owned Iterator descriptors and independent packet proofs
 *
 * KEY CONCEPT:
 *   Element identity never proves the hidden frame transferable or executable.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_type_inference.h"
#include "xir/xxir_value.h"
#include "xir/xxir_type_arena.h"
#include "tuple_owner_observer.h"
#include "xir_iterator_types_golden.h"
#include "xir_iterator_types_previous73_golden.h"
#include "xir_iterator_types_previous74_golden.h"
#include "xir_iterator_types_previous86_golden.h"
#include "xir_iterator_types_previous87_golden.h"
#include "xir_iterator_types_previous88_golden.h"
#include "xir/xxir_effect_terms.inc.c"

typedef struct IteratorFixture {
    XrXirTypeNode nodes[2]; XrXirTypes types; XrXirType parameters[2];
    XrXirInstruction instruction; XrXirBlock block; XrXirFunction function; XrXirModule module;
} IteratorFixture;
static void iterator_fixture(IteratorFixture *f, bool unit) {
    *f = (IteratorFixture){0};
    f->nodes[0] = (XrXirTypeNode){.kind=XR_XIR_TYPE_ITERATOR, .element=unit ? XR_XIR_UNIT : XR_XIR_I64};
    f->nodes[1] = (XrXirTypeNode){.kind=XR_XIR_TYPE_ITERATOR, .element=XR_XIR_STRING};
    f->types = (XrXirTypes){f->nodes,2,NULL,NULL};
    f->parameters[0]=(XrXirType)256; f->parameters[1]=(XrXirType)257;
    f->instruction=(XrXirInstruction){.op=XR_XIR_RETURN}; f->block=(XrXirBlock){.count=1};
    f->function=(XrXirFunction){.name="iteratorTypes",.name_length=13,.parameters=f->parameters,
        .parameter_count=2,.blocks=&f->block,.block_count=1,.instructions=&f->instruction,.instruction_count=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=&f->function,.function_count=1,.types=&f->types};
}
static XrXirStatus iterator_pipeline(const XrXirCompileContext *c, bool unit) {
    IteratorFixture f; iterator_fixture(&f,unit);
    const uint8_t *golden=unit ? iterator_types_unit : iterator_types_golden;
    XrXirArtifact *checked=NULL,*decoded=NULL,*lowered=NULL; XrXirTypes *clone=NULL;
    XrXirTypeArena *arena=NULL;
    XrXirCheckedPacket packet={0}; XrXirStatus s=xir_fixture_check(c,&f.module,&checked,NULL);
    if (s==XR_XIR_OK) s=xr_xir_compile_checked_write(checked,&packet,NULL);
    if (s==XR_XIR_OK) {
        CHECK(packet.length==sizeof(iterator_types_golden));
        CHECK(!memcmp(packet.bytes,golden,packet.length));
        s=xr_xir_compile_checked_read(c,golden,sizeof(iterator_types_golden),&decoded,NULL);
    }
    if (s==XR_XIR_OK) s=xr_xir_compile_types_clone(c,xr_xir_compile_artifact_module(decoded)->types,&clone);
    if (s==XR_XIR_OK) {
        memset(f.nodes,0xcc,sizeof(f.nodes)); memset(f.parameters,0xcc,sizeof(f.parameters));
        CHECK(clone->nodes[0].kind==XR_XIR_TYPE_ITERATOR && clone->nodes[0].element==(unit ? XR_XIR_UNIT : XR_XIR_I64));
        s=xr_xir_compile_artifact_verify(checked,NULL);
        if (s==XR_XIR_OK) s=xr_xir_compile_artifact_verify(decoded,NULL);
        if (s==XR_XIR_OK) s=xr_xir_compile_lower(decoded,
            &(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);
        if (s==XR_XIR_OK) s=xr_xir_compile_artifact_verify(lowered,NULL);
    }
    if (s==XR_XIR_OK) {
        XrXirValueStatus made=xr_xir_compile_type_arena_new(c,clone,&arena);
        s=made==XR_XIR_VALUE_OK ? XR_XIR_OK : made==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
            made==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
        if (s==XR_XIR_OK) {
            CHECK(xr_xir_compile_type_arena_types(arena)->nodes[0].kind==XR_XIR_TYPE_ITERATOR);
            XrXirValue missing={(uint32_t)256,0,0};
            CHECK(!xr_xir_value_valid(&missing) && !xr_xir_value_argument(&missing,arena,(XrXirType)256));
        }
    }
    xr_xir_compile_type_arena_drop(arena);
    xr_xir_compile_types_free(clone); xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(decoded); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked); return s;
}
static void iterator_rejections(void) {
    XrXirCompileContext c=owner_new(caps()); uint64_t baseline=stats(&c).live_bytes;
    const uint8_t *previous73[]={iterator_types_previous73_golden,iterator_types_previous73_unit,
        iterator_types_previous73_cycle,iterator_types_previous73_unknown,iterator_types_previous73_duplicate};
    for (uint32_t old=0;old<5;++old) {
        XrXirArtifact *output=NULL;size_t before_attempts=attempts;XrCompileResourceStats before=stats(&c);
        CHECK(xr_xir_compile_checked_read(&c,previous73[old],sizeof(iterator_types_previous73_golden),&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(attempts==before_attempts && stats(&c).allocated_bytes==before.allocated_bytes && stats(&c).live_bytes==baseline);
    }
    const uint8_t *previous74[]={iterator_types_previous74_golden,iterator_types_previous74_unit,
        iterator_types_previous74_cycle,iterator_types_previous74_unknown,iterator_types_previous74_duplicate};
    for (uint32_t old=0;old<5;++old) {
        XrXirArtifact *output=NULL;size_t before_attempts=attempts;XrCompileResourceStats before=stats(&c);
        CHECK(xr_xir_compile_checked_read(&c,previous74[old],sizeof(iterator_types_previous74_golden),&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(attempts==before_attempts && stats(&c).allocated_bytes==before.allocated_bytes && stats(&c).live_bytes==baseline);
    }
    const uint8_t *previous86[]={iterator_types_previous86_golden,iterator_types_previous86_unit,
        iterator_types_previous86_cycle,iterator_types_previous86_unknown,iterator_types_previous86_duplicate};
    for (uint32_t old=0;old<5;++old) {
        XrXirArtifact *output=NULL;size_t before_attempts=attempts;XrCompileResourceStats before=stats(&c);
        CHECK(xr_xir_compile_checked_read(&c,previous86[old],sizeof(iterator_types_previous86_golden),&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(attempts==before_attempts && stats(&c).allocated_bytes==before.allocated_bytes && stats(&c).live_bytes==baseline);
    }
    const uint8_t *previous87[]={iterator_types_previous87_golden,iterator_types_previous87_unit,
        iterator_types_previous87_cycle,iterator_types_previous87_unknown,iterator_types_previous87_duplicate};
    for (uint32_t old=0;old<5;++old) {
        XrXirArtifact *output=NULL;size_t before_attempts=attempts;XrCompileResourceStats before=stats(&c);
        CHECK(xr_xir_compile_checked_read(&c,previous87[old],sizeof(iterator_types_previous87_golden),&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(attempts==before_attempts && stats(&c).allocated_bytes==before.allocated_bytes && stats(&c).live_bytes==baseline);
    }
    const uint8_t *previous88[]={iterator_types_previous88_golden,iterator_types_previous88_unit,
        iterator_types_previous88_cycle,iterator_types_previous88_unknown,iterator_types_previous88_duplicate};
    for (uint32_t old=0;old<5;++old) {
        XrXirArtifact *output=NULL;size_t before_attempts=attempts;XrCompileResourceStats before=stats(&c);
        CHECK(xr_xir_compile_checked_read(&c,previous88[old],sizeof(iterator_types_previous88_golden),&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(attempts==before_attempts && stats(&c).allocated_bytes==before.allocated_bytes && stats(&c).live_bytes==baseline);
    }
    for (uint32_t mutation=0;mutation<16;++mutation) {
        IteratorFixture f; iterator_fixture(&f,false); XrXirCallableParameter p={XR_XIR_I64,0};
        XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        switch (mutation) {
        case 0:f.nodes[0].element=(XrXirType)256;expected=XR_XIR_BAD_TYPE;break;
        case 1:f.nodes[0].element=(XrXirType)257;expected=XR_XIR_BAD_TYPE;break;
        case 2:f.nodes[0].element=(XrXirType)255;expected=XR_XIR_BAD_TYPE;break;
        case 3:f.nodes[0].parameter_span=1;expected=XR_XIR_BAD_TYPE;break;
        case 4:f.nodes[0].result=XR_XIR_I64;break;
        case 5:f.nodes[0].flags=1;break;
        case 6:f.nodes[0].parameters=&p;f.nodes[0].parameter_count=1;break;
        case 7:f.nodes[0].nominal.declaration=1;break;
        case 8:f.nodes[0].nominal.arguments=&p.type;f.nodes[0].nominal.argument_count=1;break;
        case 9:f.nodes[0].nominal.fields=&p.type;f.nodes[0].nominal.field_count=1;break;
        case 10:f.nodes[0].kind=UINT32_MAX;expected=XR_XIR_BAD_TYPE;break;
        case 11:f.nodes[1].element=XR_XIR_I64;break;
        case 12:f.nodes[0].kind=XR_XIR_TYPE_CELL;f.nodes[0].element=XR_XIR_I64;
            f.nodes[1].element=(XrXirType)256;expected=XR_XIR_BAD_TYPE;break;
        case 14:f.nodes[0].kind=9;expected=XR_XIR_BAD_TYPE;break;
        case 15:f.nodes[0].kind=10;expected=XR_XIR_BAD_TYPE;break;
        default:f.nodes[1].kind=XR_XIR_TYPE_TASK;f.nodes[1].element=(XrXirType)256;expected=XR_XIR_BAD_TYPE;break;
        }
        XrXirArtifact *out=NULL; CHECK(xir_fixture_check(&c,&f.module,&out,NULL)==expected && !out);
        CHECK(stats(&c).live_bytes==baseline);
    }
    for(uint32_t reserved=153;reserved<=175;++reserved) {
        IteratorFixture f;iterator_fixture(&f,false);f.instruction.op=(XrXirOp)reserved;
        XrXirArtifact *out=NULL;
        CHECK(xir_fixture_check(&c,&f.module,&out,NULL)==XR_XIR_BAD_STAGE && !out);
        CHECK(stats(&c).live_bytes==baseline);
    }
    const uint8_t *bad[]={iterator_types_cycle,iterator_types_unknown,iterator_types_duplicate};
    for (uint32_t i=0;i<3;++i) {
        XrXirArtifact *out=NULL;
        CHECK(xr_xir_compile_checked_read(&c,bad[i],sizeof(iterator_types_golden),&out,NULL)==
            (i==2 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE) && !out);
        out=(XrXirArtifact *)(uintptr_t)1; size_t before=attempts;
        CHECK(xr_xir_compile_checked_read(&c,bad[i],sizeof(iterator_types_golden),&out,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(out==(XrXirArtifact *)(uintptr_t)1 && attempts==before && stats(&c).live_bytes==baseline);
    }
    owner_free(&c,baseline);
}
static XrXirStatus iterator_substitution(const XrXirCompileContext *c) {
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_ITERATOR,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1},
        {.kind=XR_XIR_TYPE_ITERATOR,.element=XR_XIR_I64}};
    XrXirTypes types={nodes,2,NULL,NULL}; XrXirType arg=XR_XIR_I64,result=XR_XIR_UNIT;
    XrXirStatus s=xr_xir_compile_types_structure_verify(c,&types);
    if (s==XR_XIR_OK) s=xr_xir_compile_type_expression_shape(c,&types,(XrXirType)256,1);
    if (s==XR_XIR_OK) s=xr_xir_compile_type_substitution_matches(c,&types,&arg,1,(XrXirType)256,(XrXirType)257);
    XrXirGeneric substitution={.arguments=&arg,.argument_count=1};
    EffectTerms pool={.types=types,.remaining=c,.capacity=2};
    if (s==XR_XIR_OK) s=effect_terms_substitute(&pool,(XrXirType)256,&substitution,&result);
    if (s==XR_XIR_OK) CHECK(xr_xir_iterator_element(&pool.types,result)==XR_XIR_I64 && !xr_xir_type_span(&pool.types,result));
    effect_terms_free(&pool);
    XrXirInferenceState *inference=NULL; XrXirType inferred=XR_XIR_UNIT;
    XrXirInferenceRequest request={.types=&types,.own_count=1};
    if (s==XR_XIR_OK) s=xr_xir_compile_inference_begin(c,&request,&inference);
    if (s==XR_XIR_OK) s=xr_xir_compile_inference_observe(inference,&types,(XrXirInferencePair){(XrXirType)256,(XrXirType)257});
    if (s==XR_XIR_OK) s=xr_xir_compile_inference_finalize(inference,&types,&inferred,1);
    if (s==XR_XIR_OK) CHECK(inferred==XR_XIR_I64);
    xr_xir_compile_inference_dispose(inference); return s;
}
static void iterator_obligations(void) {
    XrXirCompileContext c=owner_new(caps()); uint64_t baseline=stats(&c).live_bytes;
    IteratorFixture f; iterator_fixture(&f,false);
    XrXirProofContext proof={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    CHECK(xr_xir_compile_type_use_verify(&c,&proof,(XrXirType)256)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&c,&proof,(XrXirType)256,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_markers_prove(&c,&proof,(XrXirType)256,XR_XIR_CONSTRAINT_EQUAL)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_type_is_owned(&f.types,(XrXirType)256));
    CHECK(xr_xir_iterator_element(&f.types,(XrXirType)256)==XR_XIR_I64);
    CHECK(xr_xir_task_element(&f.types,(XrXirType)256)==XR_XIR_UNIT);
    CHECK(xr_xir_compile_type_expression_shape(&c,&f.types,(XrXirType)256,0)==XR_XIR_OK);
    XrXirLayout layout={0};
    CHECK(xr_xir_compile_layout(&c,&f.types,(XrXirType)256,
        &(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_XIR_LAYOUT_STORAGE,&layout)==XR_XIR_OK);
    CHECK(layout.size==8 && layout.alignment==8);
    CHECK(iterator_substitution(&c)==XR_XIR_OK); owner_free(&c,baseline);
}
static void iterator_axes(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(iterator_pipeline(&c,false)==XR_XIR_OK);
    XrCompileResourceStats required=stats(&c);owner_free(&c,baseline);
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t exact=0;exact<2;++exact) {
        XrCompileResourceLimits limits=caps();
        if (!axis) limits.allocated_bytes=required.allocated_bytes-(exact ? 0 : 1);
        else if (axis==1) limits.live_bytes=required.peak_bytes-(exact ? 0 : 1);
        else limits.work=required.work-(exact ? 0 : 1);
        c=owner_new(limits);baseline=stats(&c).live_bytes;
        XrXirStatus s=iterator_pipeline(&c,false);
        CHECK(s==(exact ? XR_XIR_OK : XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    printf("Iterator complete pipeline exact/minus1 axes allocated=%llu peak=%llu work=%llu physical0\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
        (unsigned long long)required.work);
}
static void iterator_faults(void) {
    size_t sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        attempts=0;fail_at=ordinal ? ordinal-1 : SIZE_MAX;injected=false;
        XrCompileResourceLimits limits=caps();XrXirCompileContext c={0};
        XrCompileResourceStatus acquired=xr_compile_resources_new(&limits,&c.resources);
        XrXirStatus s=acquired==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY;
        uint64_t baseline=0;
        if (s==XR_XIR_OK) {
            c.limits=xr_xir_compile_default_limits();baseline=stats(&c).live_bytes;
            s=iterator_pipeline(&c,false);
            if (s==XR_XIR_OK) s=iterator_pipeline(&c,true);
            if (s==XR_XIR_OK) s=iterator_substitution(&c);
        }
        if (ordinal && c.resources) {
            CHECK(injected && s==XR_XIR_OUT_OF_MEMORY);
            XrCompileResourceStats failed=stats(&c);fail_at=SIZE_MAX;
            CHECK(iterator_pipeline(&c,false)==XR_XIR_OK);
            CHECK(iterator_pipeline(&c,true)==XR_XIR_OK);
            CHECK(iterator_substitution(&c)==XR_XIR_OK);
            CHECK(stats(&c).work>failed.work && stats(&c).allocated_bytes>failed.allocated_bytes);
        }
        if (c.resources) owner_free(&c,baseline);
        if (!ordinal) {CHECK(s==XR_XIR_OK);sites=attempts;CHECK(sites);}
        else CHECK(injected && s==XR_XIR_OUT_OF_MEMORY);
        CHECK(!live && !live_bytes);
    }
    fail_at=SIZE_MAX;printf("Iterator complete pipeline allocation prefixes=%zu physical0\n",sites);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--faults")) {iterator_faults();return 0;}
    CHECK(argc==1);iterator_rejections();iterator_obligations();iterator_axes();
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(iterator_pipeline(&c,false)==XR_XIR_OK);CHECK(iterator_pipeline(&c,true)==XR_XIR_OK);
    owner_free(&c,baseline);puts("Iterator independent Checked packets, producer-dead Lowered, constraints and substitution PASS");return 0;
}
