/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_invocation_body_index.c - Exact owned keys across distinct function bodies
 */
#define main body_index_original_checks
#include "../../xir/test_xir_invocation_zero_inputs.c"
#undef main
typedef struct BodyKeyFixture {
    char names[32][12];
    XrXirFunction functions[32];
    XrXirFunctionIdentity identities[32];
    XrXirBlock blocks[32];
    XrXirInstruction leaf,constant[2],entry[3],initializer;
    XrXirType scalar;
    uint32_t operand;
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} BodyKeyFixture;

static void body_key_fixture(BodyKeyFixture *f) {
    memset(f,0,sizeof(*f));f->scalar=XR_XIR_I64;
    f->leaf=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->initializer=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->constant[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
    f->constant[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=73};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1}};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    for (uint32_t i=0;i<32;++i) {
        int length=snprintf(f->names[i],sizeof(f->names[i]),"body%u",i);
        CHECK(length>0 && (size_t)length<sizeof(f->names[i]));
        f->blocks[i]=(XrXirBlock){.count=i==30?3:i>=2 && i<30?2:1};
        f->functions[i]=(XrXirFunction){.name=f->names[i],.name_length=(uint32_t)length,
            .parameters=i<2?&f->scalar:NULL,.parameter_count=i<2?1:0,.result=i<31?XR_XIR_I64:XR_XIR_UNIT,
            .instructions=i<2?&f->leaf:i<30?f->constant:i==30?f->entry:&f->initializer,
            .instruction_count=i==30?3:i>=2 && i<30?2:1,.blocks=&f->blocks[i],.block_count=1,
            .operands=i==30?&f->operand:NULL,.operand_count=i==30?1:0};
    }
    f->source=(XrXirSourceModule){.name="body-keys",.name_length=9,.initializer=31};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.entry_function=30};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.declarations=&f->declarations,
        .functions=f->functions,.function_count=32,.linkage_kind=XR_XIR_PROGRAM};
}

static uint32_t body_key_linear(const EffectInvocationOwner *owner,uint32_t root,
    uint32_t body,const uint64_t *input,uint32_t words) {
    uint32_t found=UINT32_MAX;
    for (uint32_t n=0;n<owner->count;++n) {
        const EffectInvocationNode *node=&owner->nodes[n];
        if (node->root!=root || node->body!=body) continue;
        bool same=true;
        for (uint32_t w=0;w<words;++w) if (node->input[w]!=input[w]) same=false;
        if (same) { CHECK(found==UINT32_MAX);found=n; }
    }
    return found;
}
static void body_key_index_controls(EffectInvocationOwner *owner) {
    CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_OK);
    RootParameterMark held=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
    uint64_t foreign_live=rp_stats(&foreign).live_bytes,foreign_work=rp_stats(&foreign).work;
    CHECK(effect_invocation_node_index_verify(&foreign,owner)==XR_XIR_BAD_STRUCTURE &&
        rp_stats(&foreign).work==foreign_work);
    rp_owner_free(&foreign,foreign_live);rp_balanced(held);
    uint32_t count=owner->body_head_count,*heads=owner->body_heads;
    owner->body_head_count=count-1;
    CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_BAD_STRUCTURE);
    owner->body_head_count=count;owner->body_heads=NULL;
    CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_BAD_STRUCTURE);owner->body_heads=heads;
    uint32_t head=heads[0],next=owner->nodes[head].key_next,out=UINT32_C(0x12345678);
    size_t attempts=rp_attempts;
    heads[0]=owner->count;
    CHECK(effect_invocation_intern(owner,0,0,owner->nodes[head].input,&out)==XR_XIR_BAD_STRUCTURE &&
        out==UINT32_C(0x12345678) && rp_attempts==attempts);
    CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_BAD_STRUCTURE);
    heads[0]=UINT32_MAX;
    CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_BAD_STRUCTURE);heads[0]=head;
    owner->nodes[head].key_next=head;
    CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_invocation_intern(owner,owner->nodes[head].root,0,owner->nodes[head].input,&out)==XR_XIR_BAD_STRUCTURE &&
        out==UINT32_C(0x12345678) && rp_attempts==attempts);
    owner->nodes[head].key_next=next;
    heads[0]=heads[1];
    CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_invocation_intern(owner,0,0,owner->nodes[head].input,&out)==XR_XIR_BAD_STRUCTURE &&
        out==UINT32_C(0x12345678) && rp_attempts==attempts);
    heads[0]=head;CHECK(effect_invocation_node_index_verify(owner->work,owner)==XR_XIR_OK);
}
/* These deliberately varied key words stay in an unsolved private dictionary.
 * No synthetic input is sealed, published, executed or used as a permission. */
static XrXirStatus body_key_private(const XrXirCompileContext *context,
    EffectBodyMaterial *material,bool oracle) {
    XrXirStatus status=XR_XIR_OK;EffectGraph graph={0};
    EffectInvocationOwner *owner=xir_compile_alloc(context,sizeof(*owner),&status);
    if (!owner) return status;
    if (!xir_compile_work(context,sizeof(*owner))) { xr_compile_resources_free(owner);return XR_XIR_BUDGET; }
    *owner=(EffectInvocationOwner){.work=context,.graph=&graph,.material=material,
        .physical=material->physical,.cells=material->cells,.body=effect_body_owned_request(context,material),
        .maximum=effect_equation_maximum(context)};
    status=effect_invocation_node_index_new(owner);
    const uint64_t expected[4][2]={{0,0},{1,0},{0,1},{UINT64_MAX,UINT64_MAX}};
    uint32_t ids[4]={0};
    for (uint32_t k=0;k<4 && status==XR_XIR_OK;++k) {
        uint64_t input[2]={expected[k][0],expected[k][1]};
        status=effect_invocation_intern(owner,0,0,input,&ids[k]);memset(input,0xCE,sizeof(input));
        if (status==XR_XIR_OK) CHECK(ids[k]==k && owner->nodes[k].input &&
            owner->nodes[k].input[0]==expected[k][0] && owner->nodes[k].input[1]==expected[k][1]);
    }
    uint32_t other_root=UINT32_MAX;
    if (status==XR_XIR_OK) status=effect_invocation_intern(owner,1,0,expected[0],&other_root);
    if (status==XR_XIR_OK) CHECK(other_root==4);
    for (uint32_t f=1;f<32 && status==XR_XIR_OK;++f) {
        uint32_t id=UINT32_MAX;
        status=effect_invocation_intern(owner,0,f,f==1?expected[0]:NULL,&id);
        if (status==XR_XIR_OK) CHECK(id==f+4);
    }
    if (status==XR_XIR_OK) CHECK(owner->count==36 && owner->capacity>=64);
    for (uint32_t round=0;round<8 && status==XR_XIR_OK;++round) {
        for (uint32_t k=0;k<4 && status==XR_XIR_OK;++k) {
            uint32_t found=UINT32_MAX;
            CHECK(body_key_linear(owner,0,0,expected[k],2)==ids[k]);
            status=effect_invocation_intern(owner,0,0,expected[k],&found);
            if (status==XR_XIR_OK) CHECK(found==ids[k] && owner->count==36);
        }
        for (uint32_t f=1;f<32 && status==XR_XIR_OK;++f) {
            uint32_t found=UINT32_MAX;const uint64_t *input=f==1?expected[0]:NULL;
            CHECK(body_key_linear(owner,0,f,input,f==1?2:0)==f+4);
            status=effect_invocation_intern(owner,0,f,input,&found);
            if (status==XR_XIR_OK) CHECK(found==f+4 && owner->count==36);
        }
    }
    if (status==XR_XIR_OK && oracle) body_key_index_controls(owner);
    if (status==XR_XIR_OK) status=effect_invocation_node_index_verify(owner->work,owner);
    if (status==XR_XIR_OK) CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);
    effect_invocation_free(owner);xr_compile_resources_free(graph.queue);return status;
}


static XrXirStatus body_key_local_operation(const XrXirCompileContext *context) {
    BodyKeyFixture fixture;body_key_fixture(&fixture);
    for (uint32_t f=0;f<31;++f) {
        fixture.functions[f].parameters=NULL;fixture.functions[f].parameter_count=0;
        fixture.functions[f].instructions=fixture.constant;fixture.functions[f].instruction_count=2;
        fixture.blocks[f].count=2;fixture.functions[f].operands=NULL;fixture.functions[f].operand_count=0;
    }
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    EffectInvocationResult *receiving=NULL;
    if (status==XR_XIR_OK) {
        CHECK(effects->invocations && !effects->invocations->equations->work);
        status=effect_invocation_result_read_new(context,effects->invocations,&receiving);
    }
    effect_invocation_result_free(receiving);xr_xir_compile_effects_free(effects);return status;
}

static XrXirStatus body_key_operation(const XrXirCompileContext *context,bool oracle) {
    BodyKeyFixture fixture;body_key_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"fixture check=%u at=%u:%u:%u reason=%u\n",(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction,(unsigned)diagnostic.reason);
    if (status==XR_XIR_OK) { status=xr_xir_compile_effects_analyze(checked,&effects);
        if (status!=XR_XIR_OK) fprintf(stderr,"effects analyze=%u\n",(unsigned)status); }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) {
        CHECK(effects->count==32 && effects->invocations && effects->invocations->equations);
        EffectInvocationOwner replay=*effects->invocations->equations;
        replay.work=context;replay.body=effect_body_owned_request(context,replay.material);
        uint32_t expected=replay.roots[31];CHECK(expected<replay.count && replay.nodes[expected].body==31);
        uint64_t before=rp_stats(context).work;
        for (uint32_t q=0;q<64 && status==XR_XIR_OK;++q) {
            uint32_t found=UINT32_MAX;
            status=effect_invocation_intern(&replay,replay.nodes[expected].root,31,replay.nodes[expected].input,&found);
            if (status==XR_XIR_OK) CHECK(found==expected && replay.count==effects->invocations->equations->count);
        }
        if (status==XR_XIR_OK) {
            uint64_t cost=rp_stats(context).work-before;
            printf("Exact last-body key: 64 lookups work=%llu nodes=%u\n",(unsigned long long)cost,replay.count);
            CHECK(cost<64*80);
            for (uint32_t f=0;f<32;++f) {
                const XrXirRootEffects *root=xr_xir_effects_root(effects,f);
                CHECK(root && root->requires_root==(f==31) && !root->unresolved);
            }
        }
    }
    if (status==XR_XIR_OK) status=body_key_private(context,effects->invocations->body,oracle);
    if (status==XR_XIR_OK) {
        EffectInvocationOwner replay=*effects->invocations->equations;
        EffectGraph graph={.material=replay.material,.physical=replay.physical};
        replay.work=context;replay.graph=&graph;replay.body=effect_body_owned_request(context,replay.material);
        status=effect_invocation_closed(&replay);
    }
    xr_xir_compile_effects_free(effects);
    return status==XR_XIR_OK?body_key_local_operation(context):status;
}
static XrCompileResourceStats body_key_run(XrCompileResourceLimits caps,size_t fail,
    XrXirStatus expected,bool oracle) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(caps);
    uint64_t baseline=rp_stats(&context).live_bytes;rp_attempts=0;rp_injected=false;rp_fail_at=fail;
    XrXirStatus status=body_key_operation(&context,oracle);
    if (status!=expected) fprintf(stderr,"key fail=%zu status=%u expected=%u\n",fail,(unsigned)status,(unsigned)expected);
    CHECK(status==expected);size_t attempts=rp_attempts;rp_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) {
        CHECK(rp_injected && attempts==fail+1 && rp_stats(&context).live_bytes==baseline);
        CHECK(body_key_operation(&context,false)==XR_XIR_OK);
    }
    XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    rp_attempts=attempts;return measured;
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    (void)body_key_run(rp_caps(),SIZE_MAX,XR_XIR_OK,true);
    XrCompileResourceStats measured=body_key_run(rp_caps(),SIZE_MAX,XR_XIR_OK,false);
    size_t sites=rp_attempts;CHECK(sites);
    for (size_t i=0;i<sites;++i) if (resources || !i || i+1==sites)
        (void)body_key_run(rp_caps(),i,XR_XIR_OUT_OF_MEMORY,false);
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
    (void)body_key_run(exact,SIZE_MAX,XR_XIR_OK,false);
    for (uint32_t axis=0;axis<3;++axis) {
        XrCompileResourceLimits smaller=exact;
        if (!axis) --smaller.allocated_bytes;else if (axis==1) --smaller.live_bytes;else --smaller.work;
        (void)body_key_run(smaller,SIZE_MAX,XR_XIR_BUDGET,false);
    }
    printf("Body buckets: independent linear keys, both physical input words, root/body separation, 36 nodes through growth64, corruption refusal and producer death; sites=%zu injection=%s same-ledger retry, three axes physical0/0 PASS\n",
        sites,resources?"ALL":"FIRST_LAST_ONLY");return 0;
}
