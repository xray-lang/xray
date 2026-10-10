/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_invocation_controls.h - Exact producer error sets and opaque alternatives
 */
enum { EI_CALLABLE=258, EI_CASES=6 };
typedef struct InvocationErrorFixture {
    EdgeFixture edge;
    XrXirTypeNode nodes[3];
    XrXirType parameters[2];
} InvocationErrorFixture;

/* Literals distinguish an executed error from a caught error, an unused
 * authentic reference from an unbound callee, and two real PHI alternatives
 * from a PHI whose other arm remains opaque. */
static void ei_fixture(InvocationErrorFixture *fixture,unsigned mode) {
    CHECK(mode<EI_CASES);memset(fixture,0,sizeof(*fixture));
    EdgeFixture *f=&fixture->edge;ee_fixture(f,EE_JUMP,false);
    memcpy(fixture->nodes,f->nodes,sizeof(f->nodes));
    fixture->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE,
        .result=XR_XIR_UNIT};
    f->types.nodes=fixture->nodes;f->types.count=3;f->module.stage=XR_XIR_BUILT;
    f->functions[3]=f->functions[2];f->functions[3].name="second";f->functions[3].name_length=6;
    f->module.function_count=4;
    if (mode!=1 && mode!=2) {
        f->functions[2]=f->functions[0];f->functions[2].name="pure";f->functions[2].name_length=4;
    }
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)EI_CALLABLE,.immediate=2};
    f->subject[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.immediate=0};
    f->subject[2]=f->init;f->blocks[0]=(XrXirBlock){.count=3};
    f->functions[1].parameters=NULL;f->functions[1].parameter_count=0;
    f->functions[1].instruction_count=3;f->functions[1].block_count=1;
    if (mode==2) {
        f->subject[1]=(XrXirInstruction){.op=XR_XIR_INVOKE_INDIRECT,.immediate=0,.targets={1,2}};
        f->subject[3]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=1};
        f->subject[4]=f->init;f->blocks[0].count=2;
        f->blocks[1]=(XrXirBlock){.first=2,.count=1};
        f->blocks[2]=(XrXirBlock){.first=3,.count=2};
        f->functions[1].instruction_count=5;f->functions[1].block_count=3;
    } else if (mode==3) {
        fixture->parameters[0]=(XrXirType)EI_CALLABLE;
        f->functions[1].parameters=fixture->parameters;f->functions[1].parameter_count=1;
        /* The reference is SSA1, while this call actually consumes formal0. */
    } else if (mode>=4) {
        fixture->parameters[0]=XR_XIR_BOOL;fixture->parameters[1]=(XrXirType)EI_CALLABLE;
        f->functions[1].parameters=fixture->parameters;f->functions[1].parameter_count=mode==4?1u:2u;
        uint32_t branch=mode==4?2u:1u,phi=branch+3;
        if (mode==4) f->subject[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,
            .type=(XrXirType)EI_CALLABLE,.immediate=3};
        f->subject[branch]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}};
        f->subject[branch+1]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}};
        f->subject[branch+2]=f->subject[branch+1];
        f->operands[0]=1;f->operands[1]=mode==4?1u:2u;
        f->operands[2]=2;f->operands[3]=mode==4?2u:1u;
        f->subject[phi]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)EI_CALLABLE,.args={0,4}};
        f->subject[phi+1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,
            .immediate=f->functions[1].parameter_count+phi};
        f->subject[phi+2]=f->init;f->blocks[0].count=branch+1;
        f->blocks[1]=(XrXirBlock){.first=branch+1,.count=1};
        f->blocks[2]=(XrXirBlock){.first=branch+2,.count=1};
        f->blocks[3]=(XrXirBlock){.first=phi,.count=3};
        f->functions[1].instruction_count=phi+3;f->functions[1].block_count=4;
        f->functions[1].operands=f->operands;f->functions[1].operand_count=4;
    }
}

static void ei_golden(const XrXirEffects *e,unsigned mode) {
    static const uint64_t errors[EI_CASES]={0,8,0,1,8,1};
    CHECK(mode<EI_CASES && e && e->count==4 && e->words==1 && e->atom_count==2);
    CHECK(!e->errors[0] && e->errors[1]==errors[mode] && e->errors[3]==8);
    CHECK(e->errors[2]==(mode==1 || mode==2?8u:0u));
    CHECK(xr_xir_effects_error_unknown(e,1)==(mode==3 || mode==5));
    CHECK(!xr_xir_effects_error_unidentified(e,1));
    CHECK(!xr_xir_effects_error(e,1,(XrXirType)EE_ENUM,0));
    CHECK(xr_xir_effects_error(e,1,(XrXirType)EE_ENUM,1)==(mode==1 || mode==4));
    CHECK(e->task_errors[1]==(mode==3 || mode==5?XR_XIR_BAD_TYPE:XR_XIR_OK));
    CHECK(e->functions[1].throws==(mode==3 || mode==5?XR_XIR_EFFECT_UNKNOWN:
        mode==1 || mode==4?XR_XIR_EFFECT_MAY:XR_XIR_EFFECT_NONE));
}

static XrXirStatus ei_whole(const XrXirCompileContext *c,unsigned mode) {
    InvocationErrorFixture fixture;ei_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(c,&fixture.edge.module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) CHECK(!checked);
    memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    if (status!=XR_XIR_OK) CHECK(!effects);
    xr_xir_compile_artifact_free(checked);
    if (status==XR_XIR_OK) ei_golden(effects,mode);
    else if (status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"invocation errors mode%u status%u f%u b%u i%u\n",mode,status,
            diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}

static void ei_faults(unsigned mode) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=ei_whole(&c,mode);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<4096); }
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&c).live_bytes==baseline && ei_whole(&c,mode)==XR_XIR_OK);
        pc_release(&c,baseline);
    }
    printf("invocation errors mode%u whole OOM sites%zu sameownerretry physical0\n",mode,sites);
}

static XrCompileResourceStats ei_measured(unsigned mode,XrCompileResourceLimits caps,XrXirStatus expected) {
    XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(ei_whole(&c,mode)==expected);XrCompileResourceStats stats=pc_stats(&c);
    pc_release(&c,baseline);return stats;
}

static void ei_limits(unsigned mode) {
    XrCompileResourceStats a=ei_measured(mode,pc_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=ei_measured(mode,exact,XR_XIR_OK);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)ei_measured(mode,less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)ei_measured(mode,less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)ei_measured(mode,less,XR_XIR_BUDGET);
}

static void pc_invocation_controls(void) {
    for (unsigned mode=0;mode<EI_CASES;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(ei_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
        ei_faults(mode);ei_limits(mode);
    }
    puts("invocation errors6 pure/typed/caught/unbound/knownPHI/opaquePHI producerdeath fullFI3axes");
}
