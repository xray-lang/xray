/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_roots.c - Independent producers, captures and projection paths
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_roots_internal.h"
#include "tuple_owner_observer.h"
#include "xir_view_root_fixture.h"
typedef struct RootObserved {
    XrXirViewRootEvent events[32];XrXirViewRootStep steps[32][8];uint32_t selections[32][8],count;
} RootObserved;
static XrXirStatus root_observe(void *user,const XrXirCompileContext *c,const XrXirViewRootEvent *e) {
    RootObserved *o=user;CHECK(o->count<32 && e->step_count<=8 && e->selection_count<=8);
    if (!xir_compile_work(c,1)) return XR_XIR_BUDGET;
    uint32_t i=o->count++;o->events[i]=*e;
    if (e->step_count) memcpy(o->steps[i],e->steps,e->step_count*sizeof(*e->steps));
    if (e->selection_count) memcpy(o->selections[i],e->selection,e->selection_count*sizeof(*e->selection));
    o->events[i].steps=o->steps[i];o->events[i].selection=o->selections[i];return XR_XIR_OK;
}
static XrXirStatus root_prove(const XrXirCompileContext *c,const RootFixture *f,RootObserved *o,XrXirDiagnostic *d) {
    *o=(RootObserved){0};return xir_compile_view_roots_verified(c,&f->module,root_observe,o,d);
}
static bool root_rejected(uint32_t kind) {return kind==2 || kind==3 || kind==5 || kind==7 || kind==9 || kind==17;}
static void root_want(const RootObserved *o,uint32_t index,uint32_t function,uint32_t value,XrXirViewRootKind kind) {
    CHECK(index<o->count);const XrXirViewRootEvent *e=&o->events[index];
    CHECK(e->root_function==function && e->root_value==value && e->kind==kind);
    CHECK(e->dynamic_identity==(kind==XR_XIR_VIEW_ROOT_OBJECT));
}
static void root_assert(uint32_t kind,const RootObserved *o) {
    if (kind==0) {
        CHECK(o->count==2);root_want(o,0,0,9,XR_XIR_VIEW_ROOT_CELL);root_want(o,1,0,9,XR_XIR_VIEW_ROOT_CELL);
        CHECK(!o->events[0].step_count && !o->events[1].opaque_suffix);
    } else if (kind==1 || kind==4 || kind==18) {
        CHECK(o->count==1);root_want(o,0,0,kind==4?2u:1u,XR_XIR_VIEW_ROOT_PARAMETER);
    } else if (kind==6) {
        CHECK(o->count==2);root_want(o,0,1,0,XR_XIR_VIEW_ROOT_PARAMETER);
        root_want(o,1,0,1,XR_XIR_VIEW_ROOT_PARAMETER);
    } else if (kind==8) {
        CHECK(o->count==3);root_want(o,0,0,1,XR_XIR_VIEW_ROOT_PARAMETER);
        root_want(o,1,1,1,XR_XIR_VIEW_ROOT_BORROWED_PARAMETER);root_want(o,2,0,1,XR_XIR_VIEW_ROOT_PARAMETER);
    } else if (kind>=10 && kind<=12) {
        CHECK(o->count==1);root_want(o,0,0,kind==10?5u:kind==11?4u:3u,
            kind==12?XR_XIR_VIEW_ROOT_OBJECT:XR_XIR_VIEW_ROOT_PARAMETER);
        const XrXirViewRootEvent *e=&o->events[0];CHECK(e->step_count==1 && !e->opaque_suffix);
        CHECK(e->steps[0].function==0 && e->steps[0].value==10 && e->steps[0].selector==(kind==12?0u:7u));
        CHECK(e->steps[0].operation==(kind==12?XR_XIR_FIELD_PLACE:XR_XIR_INDEX_PLACE));
        CHECK(e->steps[0].container==(kind==10?ROOT_NESTED:kind==11?ROOT_MAP:ROOT_OBJECT));
    } else if (kind==13) {
        CHECK(o->count==3);root_want(o,0,0,1,XR_XIR_VIEW_ROOT_PARAMETER);
        root_want(o,1,0,2,XR_XIR_VIEW_ROOT_PARAMETER);root_want(o,2,0,2,XR_XIR_VIEW_ROOT_PARAMETER);
    } else if (kind==14 || kind==15) {
        CHECK(o->count==2);root_want(o,0,0,1,XR_XIR_VIEW_ROOT_PARAMETER);root_want(o,1,0,1,XR_XIR_VIEW_ROOT_PARAMETER);
        CHECK(!o->events[1].opaque_suffix && o->events[1].function==0);
    } else if (kind==16) {
        CHECK(o->count==1);root_want(o,0,UINT32_MAX,0,XR_XIR_VIEW_ROOT_MODULE);
    } else if (kind==19) {
        CHECK(o->count==1);root_want(o,0,1,0,XR_XIR_VIEW_ROOT_BORROWED_PARAMETER);
        CHECK(o->events[0].selection_count==2 && o->selections[0][0]==2 && o->selections[0][1]==UINT32_MAX);
    } else if (kind==20 || kind==21) {
        CHECK(o->count==(kind==20?3u:2u));root_want(o,0,0,1,XR_XIR_VIEW_ROOT_PARAMETER);
        CHECK(o->events[0].opaque_suffix);root_want(o,1,1,0,XR_XIR_VIEW_ROOT_PARAMETER);
        CHECK(!o->events[1].opaque_suffix);if (kind==20) root_want(o,2,0,1,XR_XIR_VIEW_ROOT_PARAMETER);
    } else CHECK(false);
}
static XrXirStatus root_stop(void *user,const XrXirCompileContext *c,const XrXirViewRootEvent *e) {
    (void)c;CHECK(e->kind==XR_XIR_VIEW_ROOT_CELL);++*(uint32_t *)user;return XR_XIR_BAD_VALUE;
}
static void root_type_access(const XrXirCompileContext *c) {
    RootFixture f;root_prepare(&f,1);uint64_t baseline=stats(c).live_bytes;
    CHECK(xr_xir_compile_type_access(c,&f.module,0,ROOT_SHARED)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_access(c,&f.module,0,ROOT_MUT)==XR_XIR_OK);
    XrXirTypeNode nodes[18];memcpy(nodes,f.nodes,sizeof(f.nodes));
    nodes[16]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=ROOT_OBJECT};
    nodes[17]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=ROOT_OBJECT};f.types.nodes=nodes;f.types.count=18;
    XrXirSourceModule modules[]={f.source,{.name="other",.name_length=5,.initializer=2}};
    uint32_t dependency=0;modules[1].dependencies=&dependency;modules[1].dependency_count=1;
    f.declarations.modules=modules;f.declarations.module_count=2;f.identities[0].module=f.identities[2].module=1;
    f.functions[2].parameter_count=0;f.functions[2].parameters=NULL;f.nominal.exported=0;
    CHECK(xr_xir_compile_types_structure_verify(c,&f.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_access(c,&f.module,0,(XrXirType)272)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_access(c,&f.module,0,(XrXirType)273)==XR_XIR_BAD_TYPE);
    f.nominal.exported=1;
    CHECK(xr_xir_compile_type_access(c,&f.module,0,(XrXirType)272)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_access(c,&f.module,0,(XrXirType)273)==XR_XIR_OK);
    CHECK(stats(c).live_bytes==baseline);
}
static void root_normal(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    for (uint32_t kind=0;kind<22;++kind) {
        RootFixture f;RootObserved o;XrXirDiagnostic d={0};root_prepare(&f,kind);
        XrXirStatus status=xr_xir_compile_types_structure_verify(&c,&f.types);
        if (status!=XR_XIR_OK) fprintf(stderr,"root type case=%u status=%u\n",kind,status);CHECK(status==XR_XIR_OK);
        status=root_prove(&c,&f,&o,&d);XrXirStatus expected=root_rejected(kind)?XR_XIR_BAD_VALUE:XR_XIR_OK;
        if (status!=expected) fprintf(stderr,"root case=%u status=%u at %u/%u/%u\n",kind,status,d.function,d.block,d.instruction);
        CHECK(status==expected && stats(&c).live_bytes==baseline);
        if (expected==XR_XIR_OK) root_assert(kind,&o);
        XrXirArtifact *checked=NULL;status=xir_fixture_check(&c,&f.module,&checked,&d);
        expected=root_rejected(kind)?XR_XIR_BAD_VALUE:XR_XIR_BAD_TYPE;
        if (status!=expected) fprintf(stderr,"root common case=%u status=%u at %u/%u/%u\n",kind,status,d.function,d.block,d.instruction);
        CHECK(status==expected && !checked && stats(&c).live_bytes==baseline);
        if (root_rejected(kind)) CHECK(d.instruction!=UINT32_MAX);
    }
    root_type_access(&c);RootFixture f;root_prepare(&f,0);uint32_t stopped=0;XrXirDiagnostic d={0};
    CHECK(xir_compile_view_roots_verified(&c,&f.module,root_stop,&stopped,&d)==XR_XIR_BAD_VALUE && stopped==1);
    CHECK(d.function==0 && d.instruction==4);owner_free(&c,baseline);
    puts("root observations: 22 producer/capture/PHI/call/Field/Index/Map/wrapper/module/loop/invoke families PASS");
}
static void root_resources(uint32_t kind) {
    RootFixture f;RootObserved o;root_prepare(&f,kind);XrXirStatus expected=root_rejected(kind)?XR_XIR_BAD_VALUE:XR_XIR_OK;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;size_t start=attempts;
    CHECK(root_prove(&c,&f,&o,NULL)==expected);size_t count=attempts-start;
    /* Direct parameter and module roots need fewer buffers than constructed
     * roots. Fault coverage follows every actual allocation, not a fixed floor. */
    CHECK(count && count<2048);
    XrCompileResourceStats measured=stats(&c);CHECK(root_prove(&c,&f,&o,NULL)==expected);owner_free(&c,baseline);
    for (size_t point=0;point<count;++point) {
        c=owner_new(caps());baseline=stats(&c).live_bytes;injected=false;fail_at=attempts+point;
        XrXirStatus status=root_prove(&c,&f,&o,NULL);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"root FI case=%u point=%zu/%zu status=%u\n",kind,point,count,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&c,baseline);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t adjust=0;adjust<3;++adjust) {
        XrCompileResourceLimits limits=caps();uint64_t *limit=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+adjust-1u;
        c=owner_new(limits);baseline=stats(&c).live_bytes;
        CHECK(root_prove(&c,&f,&o,NULL)==(adjust?expected:XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    c=owner_new(caps());baseline=stats(&c).live_bytes;c.limits.frame_bytes=1;
    CHECK(root_prove(&c,&f,&o,NULL)==XR_XIR_BUDGET);owner_free(&c,baseline);
    printf("root case=%u: %zu complete allocation failures; same owner2/three axes exact-minus1-plus1/frame/physical0 PASS\n",kind,count);
}
static void root_work_sweep(void) {
    RootFixture f;RootObserved o;root_prepare(&f,13);XrXirCompileContext c=owner_new(caps());
    uint64_t baseline=stats(&c).live_bytes;CHECK(root_prove(&c,&f,&o,NULL)==XR_XIR_OK);
    uint64_t work=stats(&c).work;CHECK(work>100 && work<100000);owner_free(&c,baseline);
    for (uint64_t limit=1;limit<work;++limit) {
        XrCompileResourceLimits limits=caps();limits.work=limit;c=owner_new(limits);baseline=stats(&c).live_bytes;
        CHECK(root_prove(&c,&f,&o,NULL)==XR_XIR_BUDGET);owner_free(&c,baseline);
    }
    printf("root wrapper: all %llu positive work cutoffs/resize-failure cleanup/physical0 PASS\n",(unsigned long long)(work-1u));
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);root_normal();
    if (!normal) {for (uint32_t kind=0;kind<22;++kind) root_resources(kind);root_work_sweep();}
    puts("Root observations issue no access, disjointness, physical END or Source execution permission PASS");return 0;
}
