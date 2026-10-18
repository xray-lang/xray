/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_sparse_storage_cases.h - Real SSA domains and original bounds
 */
#ifndef XIR_INVOCATION_SPARSE_STORAGE_CASES_H
#define XIR_INVOCATION_SPARSE_STORAGE_CASES_H
#include "xir_raw_physical_correspondence.h"

/* Every body first passes Built checking and owned verification. Scalar
 * stores, COPY, local operations and PHI keep their full-SSA fixed flags. */
static void invocation_storage_fixture(InvocationDeferredFixture *fixture,uint32_t mode,
    XrXirInstruction *wide,XrXirInstruction *init,XrXirBlock *blocks,uint32_t *incoming) {
    invocation_deferred_fixture(fixture,0);
    for (uint32_t i=0;i<94;++i)
        wide[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    wide[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=fixture->cell,.args={2,0}};
    wide[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={2,0}};
    wide[3]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=XR_XIR_I64,.args={4,0}};
    wide[4]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=XR_XIR_I64,.args={5,0}};
    wide[5]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={5,6}};
    if (mode) wide[63]=fixture->entry[2];
    if (mode==2) wide[64]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)257,.args={65,0}};
    wide[94]=fixture->apply[0];wide[94].immediate=mode==2 ? 66 : mode ? 65 : 0;
    wide[95]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={96,0}};
    blocks[0]=(XrXirBlock){.count=96};
    fixture->functions[1].instructions=wide;fixture->functions[1].instruction_count=96;
    fixture->functions[1].blocks=&blocks[0];
    init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    init[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=fixture->cell};
    init[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
    init[3]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
    init[4]=(XrXirInstruction){.op=XR_XIR_PHI,.type=XR_XIR_I64,.args={0,2}};
    init[5]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=XR_XIR_I64,.args={4,0}};
    init[6]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=XR_XIR_I64,.args={5,0}};
    init[7]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={5,6}};
    init[8]=(XrXirInstruction){.op=XR_XIR_RETURN};
    blocks[1]=(XrXirBlock){.count=4};blocks[2]=(XrXirBlock){.first=4,.count=5};
    incoming[0]=0;incoming[1]=2;
    fixture->functions[3].instructions=init;fixture->functions[3].instruction_count=9;
    fixture->functions[3].blocks=&blocks[1];fixture->functions[3].block_count=2;
    fixture->functions[3].operands=incoming;fixture->functions[3].operand_count=2;
}

/* This shape copier proves sparse advertisements against every physical SSA.
 * It produces no executable permission. The real owner is sealed separately. */
static XrXirStatus invocation_storage_copy_bounds(const XrXirCompileContext *context,
    const XrXirModule *module,const EffectInvocationDeclaredBounds *bounds) {
    EffectBodyMaterial *material=NULL;
    EffectInvocationDeclaredBounds *selected=NULL;
    XrXirStatus status=effect_body_material_create(context,module,&material);
    if (status==XR_XIR_OK) status=invocation_raw_physical_match(context,material->physical,module);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_copy_physical(context,bounds,
        material->physical,&selected);
    if (status==XR_XIR_OK) {
        /* This is the production explicit-bound receiving operation before
         * equations consume a Material, followed by the real seal membership gate. */
        effect_invocation_bounds_free(material->declared);material->declared=selected;selected=NULL;
        EffectInvocationCertificate receiving={.resources=context->resources,.body=material,
            .terms={.remaining=context}};
        status=effect_invocation_bind_bounds(&receiving,material->declared,material->physical);
        if (status==XR_XIR_OK) CHECK(receiving.declared==material->declared);
        effect_terms_free(&receiving.terms);
    } else CHECK(!selected);
    effect_invocation_bounds_free(selected);effect_body_material_free(material);return status;
}

static void invocation_storage_rejections(const XrXirCompileContext *context,
    const XrXirModule *module,EffectInvocationDeclaredBounds *bounds) {
    CHECK(invocation_storage_copy_bounds(context,module,bounds)==XR_XIR_OK);
    EffectInvocationFunctionBounds *body=&bounds->functions[1];
    CHECK(body->count && body->bounds && !body->bounds[0].value);
    uint32_t output=UINT32_C(0x12345678);
    CHECK(effect_invocation_bounds_mask(context,bounds,1,2,&output)==XR_XIR_BAD_TYPE &&
        output==UINT32_C(0x12345678));
    uint32_t count=body->count;body->count=count-1;
    CHECK(invocation_storage_copy_bounds(context,module,bounds)==XR_XIR_BAD_STRUCTURE);
    body->count=count;
    uint32_t value=body->bounds[0].value;body->bounds[0].value=2;
    CHECK(invocation_storage_copy_bounds(context,module,bounds)==XR_XIR_BAD_STRUCTURE);
    body->bounds[0].value=value;
    body->bounds[0].callable=false;
    CHECK(invocation_storage_copy_bounds(context,module,bounds)==XR_XIR_BAD_STRUCTURE);
    body->bounds[0].callable=true;
    uint32_t mask=body->bounds[0].mask;body->bounds[0].mask=UINT32_C(0x80000000);
    CHECK(invocation_storage_copy_bounds(context,module,bounds)==XR_XIR_BAD_STRUCTURE);
    body->bounds[0].mask=mask;
    if (count>1) {
        EffectInvocationValueBound first=body->bounds[0];body->bounds[0]=body->bounds[1];body->bounds[1]=first;
        CHECK(invocation_storage_copy_bounds(context,module,bounds)==XR_XIR_BAD_STRUCTURE);
        body->bounds[1]=body->bounds[0];body->bounds[0]=first;
    }
    CHECK(invocation_storage_copy_bounds(context,module,bounds)==XR_XIR_OK);
}


static bool invocation_storage_fixed(EffectInvocationFlow *flow,uint32_t value) {
    bool fixed=false;CHECK(effect_invocation_flag_get(flow,value,false,&fixed)==XR_XIR_OK);return fixed;
}
static void invocation_storage_bitmap_controls(EffectInvocationFlow *flow) {
    CHECK(flow->values && effect_invocation_flags_match(flow)==XR_XIR_OK);
    bool sentinel=true,output=sentinel,changed=false;
    CHECK(effect_invocation_flag_get(flow,flow->values,false,&output)==XR_XIR_BAD_STRUCTURE && output==sentinel);
    CHECK(effect_invocation_flag_put(flow,flow->values,true,&changed)==XR_XIR_BAD_STRUCTURE && !changed);
    uint32_t tail=flow->values%64;
    if (tail) {
        uint32_t last=(flow->values-1)/64;uint64_t bit=UINT64_C(1)<<tail;
        flow->fixed[last]|=bit;
        CHECK(effect_invocation_flags_match(flow)==XR_XIR_BAD_STRUCTURE);
        flow->fixed[last]&=~bit;flow->certified[last]|=bit;
        CHECK(effect_invocation_flags_match(flow)==XR_XIR_BAD_STRUCTURE);
        flow->certified[last]&=~bit;CHECK(effect_invocation_flags_match(flow)==XR_XIR_OK);
    }
}
static XrXirStatus invocation_storage_rows(EffectInvocationWorkspace *owner,uint32_t node,uint32_t mode,bool oracle) {
    EffectInvocationFlow flow={.owner=owner,.read=&owner->read,.node=node,.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_discovery_origins(&flow);
    if (status==XR_XIR_OK && oracle) {
        uint32_t body=owner->final.nodes[node].body,expected=0;
        for (uint32_t v=0;v<flow.values;++v) {
            XrXirType type=XR_XIR_UNIT;CHECK(effect_invocation_flow_type(&flow,v,&type)==XR_XIR_OK);
            bool callable=xr_xir_callable_signature(flow.body.domain.types,type)!=NULL;
            CHECK((effect_invocation_row(&flow,v)!=NULL)==callable);
            if (callable) CHECK(flow.row_indices[v]==expected++);
            else CHECK(!flow.row_indices || flow.row_indices[v]==UINT32_MAX);
        }
        CHECK(flow.row_count==expected && flow.fixed && flow.certified);
        invocation_storage_bitmap_controls(&flow);
        if (!expected) CHECK(!flow.row_indices);
        if (body==1) {
            CHECK(expected==mode+1 && flow.values==98 && invocation_storage_fixed(&flow,2) && invocation_storage_fixed(&flow,4) &&
                invocation_storage_fixed(&flow,5) && invocation_storage_fixed(&flow,6));
            uint32_t row=flow.row_indices[0];flow.row_indices[0]=UINT32_MAX;
            CHECK(effect_invocation_origin_bit(&flow,0,0)==XR_XIR_BAD_TYPE);
            CHECK(effect_invocation_origin_join(&flow,0,0)==XR_XIR_BAD_STRUCTURE);
            flow.row_indices[0]=row;
        } else if (body==3) {
            CHECK(!expected && invocation_storage_fixed(&flow,0) && invocation_storage_fixed(&flow,2) && invocation_storage_fixed(&flow,4) && invocation_storage_fixed(&flow,5) && invocation_storage_fixed(&flow,6));
            CHECK(effect_invocation_origin_join(&flow,4,2)==XR_XIR_OK);
        }
    }
    xr_compile_resources_free(flow.memory);return status;
}

static XrXirStatus conditional_storage_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (mode>=3) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;XrXirInstruction wide[96]={{0}},init[9]={{0}};
    XrXirBlock blocks[3]={{0}};uint32_t incoming[2]={0};
    invocation_storage_fixture(&fixture,mode,wide,init,blocks,incoming);
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationWorkspace *owner=NULL;EffectFinalFunctions *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) { phase=2;status=invocation_core_storage(context,module,&effects); }
    if (status==XR_XIR_OK) { phase=3;status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context); }
    if (status==XR_XIR_OK) { phase=4;status=effect_parameters_derive(module,&effects,&graph,context); }
    if (status==XR_XIR_OK) { phase=5;status=effect_invocation_bounds_new(context,&bounds); }
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK && oracle) invocation_storage_rejections(context,module,bounds);
    if (status==XR_XIR_OK) { phase=6;status=effect_invocations_receive(context,module,&effects,&graph,bounds,&owner); }
    if (status==XR_XIR_OK) { phase=7;status=effect_invocations_solve(owner); }
    for (uint32_t n=0;status==XR_XIR_OK && n<owner->final.count;++n)
        status=invocation_storage_rows(owner,n,mode,oracle);
    if (status==XR_XIR_OK) { phase=8;status=effect_invocation_stage(owner,&solution); }
    EffectInvocationSealRequest request={&owner,&solution,NULL};
    if (status==XR_XIR_OK) { phase=9;status=effect_invocation_seal(context,&request,&certificate,NULL); }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_STORAGE mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    effect_invocation_free(owner);effect_invocation_result_free(solution);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    effect_invocation_bounds_free(bounds);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xCE,sizeof(fixture));memset(wide,0xCE,sizeof(wide));memset(init,0xCE,sizeof(init));
    memset(blocks,0xCE,sizeof(blocks));memset(incoming,0xCE,sizeof(incoming));
    if (status==XR_XIR_OK) {
        uint32_t mask=UINT32_MAX;
        phase=10;status=effect_invocation_certificate_node_mask(context,certificate,certificate->final->roots[0],&mask);
        if (status==XR_XIR_OK && oracle) {
            CHECK(!mask && certificate->declared && certificate->declared->functions[1].values==98);
            const EffectInvocationFunctionBounds *body=&certificate->declared->functions[1];
            CHECK(body->count==mode+1 && !body->bounds[0].value);
            if (mode) CHECK(body->bounds[1].value==65);
            if (mode==2) CHECK(body->bounds[2].value==66);
            for (uint32_t r=0;r<body->count;++r) {
                CHECK(body->bounds[r].callable && body->bounds[r].mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
                uint32_t saved=UINT32_MAX;
                CHECK(effect_invocation_bounds_mask(context,certificate->declared,1,body->bounds[r].value,&saved)==XR_XIR_OK &&
                    saved==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            }
            CHECK(!certificate->declared->functions[3].count && !certificate->declared->functions[3].bounds);
            CHECK((certificate->final->material!=NULL));
        }
    }
    effect_invocation_certificate_free(certificate);return status;
}

/* This literal bitmap/geometry component fixture never admits a language
 * artifact or publishes its small structural input as a Final certificate. */
static uint64_t invocation_storage_geometry(const XrXirCompileContext *context,
    uint32_t values,uint32_t rows,bool check_bits,XrXirStatus expected) {
    EffectPhysicalRecord record={.value=0,.mask=EFFECT_PHYSICAL_CALLABLE};
    EffectPhysicalFunction physical={.instructions=values,.values=values,.rows=rows,
        .count=rows,.capacity=rows,.records=rows?&record:NULL};
    EffectPhysicalDomain domain={.resources=context->resources,.count=1,.functions=&physical};
    EffectInvocationNode node={.body=0};
    EffectInvocationFinal shape={.physical=&domain,.nodes=&node,.count=1};
    EffectInvocationRead read={.work=context,.final=&shape};
    EffectBodyFunctionHead head={.instruction_count=values};
    EffectInvocationFlow flow={.read=&read,.function=&head,.basis={.fn_words=2}};
    flow.local_flags[0]=UINT64_MAX;flow.local_flags[1]=UINT64_MAX;
    uint64_t before=rp_stats(context).work,allocated=rp_stats(context).allocated_bytes;
    size_t attempts=rp_attempts;
    XrXirStatus status=effect_invocation_flow_storage(&flow);
    uint64_t used=rp_stats(context).work-before;
    CHECK(status==expected);
    if (status==XR_XIR_OK && check_bits) {
        bool local=!rows && values && values<=64;
        CHECK(flow.values==values && flow.row_count==rows);
        CHECK((flow.memory!=NULL)==(values && !local));
        CHECK((rp_attempts==attempts)==(!values || local));
        CHECK((rp_stats(context).allocated_bytes==allocated)==(!values || local));
        if (local) CHECK(flow.rows==flow.local_flags && flow.fixed==flow.local_flags && flow.certified==flow.local_flags+1);
        CHECK(effect_invocation_flags_match(&flow)==XR_XIR_OK);
        bool expected_fixed[129]={0},expected_certified[129]={0};
        for (uint32_t v=0;v<values;++v) {
            bool first=true,second=true,changed=false;
            CHECK(effect_invocation_flag_get(&flow,v,false,&first)==XR_XIR_OK && !first);
            CHECK(effect_invocation_flag_get(&flow,v,true,&second)==XR_XIR_OK && !second);
            expected_fixed[v]=(v%3)==1;expected_certified[v]=(v%2)==0;
            if (expected_fixed[v]) CHECK(effect_invocation_flag_put(&flow,v,false,&changed)==XR_XIR_OK && changed);
            changed=false;
            if (expected_certified[v]) CHECK(effect_invocation_flag_put(&flow,v,true,&changed)==XR_XIR_OK && changed);
        }
        uint64_t packed[2][3]={{0}};
        for (uint32_t v=0;v<values;++v) {
            bool actual=false;
            CHECK(effect_invocation_flag_get(&flow,v,false,&actual)==XR_XIR_OK && actual==expected_fixed[v]);
            CHECK(effect_invocation_flag_get(&flow,v,true,&actual)==XR_XIR_OK && actual==expected_certified[v]);
            if (expected_fixed[v]) packed[0][v/64]|=UINT64_C(1)<<(v%64);
            if (expected_certified[v]) packed[1][v/64]|=UINT64_C(1)<<(v%64);
        }
        for (uint32_t word=0;word<(values+63)/64;++word)
            CHECK(flow.fixed[word]==packed[0][word] && flow.certified[word]==packed[1][word]);
        if (values) invocation_storage_bitmap_controls(&flow);
        if (local) {
            uint64_t other[2]={0};flow.fixed=other;flow.certified=other+1;
            CHECK(effect_invocation_flags_match(&flow)==XR_XIR_BAD_STRUCTURE);
            flow.fixed=flow.local_flags;flow.certified=flow.local_flags+1;
            flow.memory=flow.local_flags;
            CHECK(effect_invocation_flags_match(&flow)==XR_XIR_BAD_STRUCTURE);
            flow.memory=NULL;
        }
    }
    xr_compile_resources_free(flow.memory);return used;
}

static void invocation_storage_geometry_literals(void) {
    const uint32_t sizes[]={0,1,2,63,64,65,127,128,129};
    unsigned cases=0,real_budget_cuts=0,actual_oom_retries=0;
    for (uint32_t row=0;row<2;++row) for (size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        uint32_t values=sizes[i];if (row && !values) continue;
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        uint64_t work=invocation_storage_geometry(&context,values,row,true,XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);++cases;
        for (unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();limits.work=work+1-minus;
            physical=rp_mark();context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
            invocation_storage_geometry(&context,values,row,false,minus?XR_XIR_BUDGET:XR_XIR_OK);
            rp_owner_free(&context,baseline);rp_balanced(physical);++real_budget_cuts;
        }
        if (values && (row || values>64)) {
            physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
            XrCompileResources *identity=context.resources;
            rp_fail_at=rp_attempts;rp_injected=false;
            invocation_storage_geometry(&context,values,row,false,XR_XIR_OUT_OF_MEMORY);
            CHECK(rp_injected);uint64_t failed_work=rp_stats(&context).work;
            rp_fail_at=SIZE_MAX;
            invocation_storage_geometry(&context,values,row,true,XR_XIR_OK);
            CHECK(context.resources==identity && rp_stats(&context).work>failed_work);
            rp_owner_free(&context,baseline);rp_balanced(physical);++actual_oom_retries;
        }
    }
    printf("local flag component %u geometry/Boolean cases, %u real exact/minus work cuts, %u actual OOM same-owner retries PASS\n",
        cases,real_budget_cuts,actual_oom_retries);
}


static void conditional_storage_literals(void) {
    invocation_storage_geometry_literals();
    for (uint32_t mode=0;mode<3;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_storage_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void conditional_storage_resources(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
            uint64_t baseline=entry.live_bytes;RootParameterMark retained=rp_mark();
            XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_storage_case(&context,mode,false);
            if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context); }
            else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
        ConditionalRetryTrial retry={.which=mode+66,.ordinal=pass-1,.sites=sites,
            .resources=identity,.initial=entry,.single=single,.retained=retained,.first=status};
        conditional_retry_operation(&context,&retry);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(physical);
        }
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        size_t census_attempts=rp_attempts;
        CHECK(conditional_storage_case(&context,mode,false)==XR_XIR_OK);
        XrCompileResourceStats census=rp_stats(&context);size_t census_sites=rp_attempts-census_attempts;
        rp_owner_free(&context,baseline);rp_balanced(physical);
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) {
            CHECK(measured[axis]>0);
            for (uint32_t pass=0;pass<2;++pass) {
                XrCompileResourceLimits limits=rp_caps();uint64_t value=measured[axis]-(pass?0:1);
                if (axis==0) limits.allocated_bytes=value;
                else if (axis==1) limits.live_bytes=value;
                else limits.work=value;
                physical=rp_mark();context=rp_owner(limits);XrCompileResourceStats entry=rp_stats(&context);
                baseline=entry.live_bytes;RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
                CHECK(conditional_storage_case(&context,mode,false)==(pass?XR_XIR_OK:XR_XIR_BUDGET));
        ConditionalRetryTrial retry={.which=mode+66,.ordinal=SIZE_MAX,.sites=census_sites,
            .resources=identity,.initial=entry,.single=census,.retained=retained,.first=pass?XR_XIR_OK:XR_XIR_BUDGET};
        conditional_retry_operation(&context,&retry);
                rp_owner_free(&context,baseline);rp_balanced(physical);
            }
        }
    }
}
#endif // XIR_INVOCATION_SPARSE_STORAGE_CASES_H
