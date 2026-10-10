/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_return_coverage_controls.h - Exhaustive returns and typed escaping errors
 */
enum { ER_CALLBACK=258, ER_FACTORY=259, ER_CASES=6 };
typedef struct ReturnErrorFixture {
    EdgeFixture edge;
    XrXirTypeNode nodes[4];
    XrXirFunction functions[6];
    XrXirFunctionIdentity identities[6];
    XrXirInstruction maker[5],base[2];
    XrXirBlock maker_blocks[3],base_block;
    XrXirType parameter;
} ReturnErrorFixture;

/* The factory returns a real pure target, a typed thrower, both concrete
 * exits, an unbound alternative, a relayed result, or an unrooted call cycle. */
static void er_fixture(ReturnErrorFixture *fixture,unsigned mode) {
    CHECK(mode<ER_CASES);memset(fixture,0,sizeof(*fixture));
    EdgeFixture *f=&fixture->edge;ee_fixture(f,EE_JUMP,false);
    memcpy(fixture->nodes,f->nodes,sizeof(f->nodes));
    fixture->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,
        .flags=XR_XIR_CALLABLE_ROOT_NONE,.result=XR_XIR_UNIT};
    fixture->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,
        .flags=XR_XIR_CALLABLE_ROOT_NONE,.result=(XrXirType)ER_CALLBACK};
    f->types.nodes=fixture->nodes;f->types.count=4;
    memcpy(fixture->functions,f->functions,sizeof(f->functions));
    f->module.functions=fixture->functions;f->module.function_count=mode==4?6u:5u;
    f->declarations.functions=fixture->identities;f->module.stage=XR_XIR_BUILT;
    fixture->functions[3]=fixture->functions[2];
    fixture->functions[3].name="second";fixture->functions[3].name_length=6;
    if (mode!=1) {
        fixture->functions[2]=fixture->functions[0];
        fixture->functions[2].name="pure";fixture->functions[2].name_length=4;
    }
    fixture->maker[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,
        .type=(XrXirType)ER_CALLBACK,.immediate=2};
    fixture->maker[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    fixture->maker_blocks[0]=(XrXirBlock){.count=2};
    fixture->functions[4]=(XrXirFunction){.name="maker",.name_length=5,
        .result=(XrXirType)ER_CALLBACK,.instructions=fixture->maker,.instruction_count=2,
        .blocks=fixture->maker_blocks,.block_count=1};
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)ER_CALLBACK,.immediate=4};
    f->subject[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.immediate=0};
    f->subject[2]=f->init;f->blocks[0]=(XrXirBlock){.count=3};
    fixture->functions[1].parameters=NULL;fixture->functions[1].parameter_count=0;
    fixture->functions[1].instructions=f->subject;fixture->functions[1].instruction_count=3;
    fixture->functions[1].blocks=f->blocks;fixture->functions[1].block_count=1;
    if (mode==2 || mode==3) {
        fixture->parameter=mode==2?XR_XIR_BOOL:(XrXirType)ER_CALLBACK;
        fixture->functions[1].parameters=fixture->functions[4].parameters=&fixture->parameter;
        fixture->functions[1].parameter_count=fixture->functions[4].parameter_count=1;
        f->operands[0]=0;fixture->functions[1].operands=f->operands;
        fixture->functions[1].operand_count=1;f->subject[0].args[1]=1;
        f->subject[1].immediate=1;
        if (mode==3) fixture->maker[1].args[0]=0; /* Formal, despite the unused authentic SSA1. */
        else {
            fixture->maker[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,
                .type=(XrXirType)ER_CALLBACK,.immediate=3};
            fixture->maker[2]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}};
            fixture->maker[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
            fixture->maker[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2}};
            fixture->maker_blocks[0].count=3;
            fixture->maker_blocks[1]=(XrXirBlock){.first=3,.count=1};
            fixture->maker_blocks[2]=(XrXirBlock){.first=4,.count=1};
            fixture->functions[4].instruction_count=5;fixture->functions[4].block_count=3;
        }
    } else if (mode==4) {
        fixture->base[0]=fixture->maker[0];fixture->base[1]=fixture->maker[1];
        fixture->base_block=(XrXirBlock){.count=2};
        fixture->functions[5]=(XrXirFunction){.name="base",.name_length=4,
            .result=(XrXirType)ER_CALLBACK,.instructions=fixture->base,.instruction_count=2,
            .blocks=&fixture->base_block,.block_count=1};
        fixture->maker[0]=(XrXirInstruction){.op=XR_XIR_CALL,
            .type=(XrXirType)ER_CALLBACK,.immediate=5};
    } else if (mode==5) {
        fixture->maker[0]=(XrXirInstruction){.op=XR_XIR_CALL,
            .type=(XrXirType)ER_CALLBACK,.immediate=4};
        f->subject[3]=f->subject[2];f->subject[2]=f->subject[1];f->subject[2].immediate=1;
        f->subject[1]=f->subject[0];f->subject[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,
            .type=(XrXirType)ER_CALLBACK,.immediate=2};
        fixture->functions[1].instruction_count=4;f->blocks[0].count=4;
    }
}

static void er_golden(const XrXirEffects *effects,unsigned mode) {
    static const uint64_t errors[ER_CASES]={0,8,8,1,0,1};
    CHECK(mode<ER_CASES && effects && effects->count==(mode==4?6u:5u));
    CHECK(effects->words==1 && effects->atom_count==2 && !effects->errors[0]);
    CHECK(effects->errors[1]==errors[mode] && effects->errors[3]==8 && !effects->errors[4]);
    CHECK(!xr_xir_effects_error_unidentified(effects,1));
    CHECK(xr_xir_effects_error_unknown(effects,1)==(mode==3 || mode==5));
    CHECK(!xr_xir_effects_error(effects,1,(XrXirType)EE_ENUM,0));
    CHECK(xr_xir_effects_error(effects,1,(XrXirType)EE_ENUM,1)==(mode==1 || mode==2));
    CHECK(effects->task_errors[1]==(mode==3 || mode==5?XR_XIR_BAD_TYPE:XR_XIR_OK));
    CHECK(effects->invocations && effects->invocations->equations);
    const EffectInvocationOwner *owner=effects->invocations->equations;
    CHECK(owner->producer_enabled && !owner->work && !owner->module && !owner->graph);
    CHECK((owner->nodes[owner->roots[4]].return_coverage!=0)==(mode!=3 && mode!=5));
    /* Coverage must not erase the original opaque NONE advertisement. */
    CHECK(effects->contracts[1].formula.constant_mask==0);
}

static XrXirStatus er_whole(const XrXirCompileContext *context,unsigned mode) {
    ReturnErrorFixture fixture;er_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.edge.module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) CHECK(!checked);
    memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    if (status!=XR_XIR_OK) CHECK(!effects);
    xr_xir_compile_artifact_free(checked);
    if (status==XR_XIR_OK) er_golden(effects,mode);
    else if (status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"return errors mode%u status%u f%u b%u i%u\n",mode,status,
            diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}

static void er_faults(unsigned mode) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=er_whole(&c,mode);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<4096); }
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&c).live_bytes==baseline && er_whole(&c,mode)==XR_XIR_OK);
        pc_release(&c,baseline);
    }
    printf("return errors mode%u whole OOM sites%zu sameownerretry physical0\n",mode,sites);
}

static XrCompileResourceStats er_measured(unsigned mode,XrCompileResourceLimits caps,XrXirStatus expected) {
    XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(er_whole(&c,mode)==expected);XrCompileResourceStats stats=pc_stats(&c);
    pc_release(&c,baseline);return stats;
}

static void er_limits(unsigned mode) {
    XrCompileResourceStats a=er_measured(mode,pc_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=er_measured(mode,exact,XR_XIR_OK);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)er_measured(mode,less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)er_measured(mode,less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)er_measured(mode,less,XR_XIR_BUDGET);
}

static void pc_return_coverage_controls(void) {
    for (unsigned mode=0;mode<ER_CASES;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(er_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
        er_faults(mode);er_limits(mode);
    }
    puts("return errors6 pure/typed/allreturns/unbound/relay/cycle producerdeath fullFI3axes");
}
