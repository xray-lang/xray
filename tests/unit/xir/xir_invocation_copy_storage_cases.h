/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_copy_storage_cases.h - Owned initialized copies and failure tails
 */
#ifndef XIR_INVOCATION_COPY_STORAGE_CASES_H
#define XIR_INVOCATION_COPY_STORAGE_CASES_H

/* The comparison uses real allocator calls on one finite owner. Rounded tail
 * bytes never enter either comparison or an initialization operation. */
static XrXirStatus conditional_copy_bytes(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    unsigned char source[4097];size_t count=mode?sizeof(source):17;
    for (size_t i=0;i<count;++i) source[i]=(unsigned char)(i*13+7);
    EffectTerms raw={.remaining=context},zero={.remaining=context};
    XrCompileResourceStats before=rp_stats(context);
    unsigned char *copy=effect_invocation_copy(&raw,source,count,1);
    XrCompileResourceStats middle=rp_stats(context);
    unsigned char *reference=raw.status==XR_XIR_OK?effect_terms_alloc(&zero,count,1):NULL;
    if (reference && !xir_compile_work(context,count)) zero.status=XR_XIR_BUDGET;
    if (reference && zero.status==XR_XIR_OK) memcpy(reference,source,count);
    XrXirStatus status=raw.status!=XR_XIR_OK?raw.status:zero.status;
    XrCompileResourceStats end=rp_stats(context);
    memset(source,0xCE,sizeof(source));
    if (status==XR_XIR_OK && oracle) {
        CHECK(copy && reference && !memcmp(copy,reference,count));
        for (size_t i=0;i<count;++i) CHECK(copy[i]==(unsigned char)(i*13+7));
        CHECK(end.work-middle.work==middle.work-before.work+count);
        CHECK(end.allocated_bytes-middle.allocated_bytes==middle.allocated_bytes-before.allocated_bytes);
        CHECK((uintptr_t)copy%_Alignof(EffectTermMemory)==0 &&
            raw.memory->used==(count+_Alignof(EffectTermMemory)-1)/_Alignof(EffectTermMemory)*_Alignof(EffectTermMemory));
        CHECK(raw.memory->used<=raw.memory->capacity && !raw.memory->next);
        CHECK(effect_invocation_copy(&raw,NULL,0,1)==NULL && raw.status==XR_XIR_OK);
        CHECK(effect_invocation_copy(&raw,NULL,1,1)==NULL && raw.status==XR_XIR_BAD_STRUCTURE);
        uint64_t work=rp_stats(context).work;
        CHECK(effect_invocation_copy(&raw,source,1,0)==NULL && raw.status==XR_XIR_BAD_STRUCTURE);
        CHECK(rp_stats(context).work==work);
    }
    effect_terms_free(&zero);effect_terms_free(&raw);return status;
}

/* Checking owns the producer first. The receiving certificate copies all
 * typed children and remains readable after both raw and Checked producers die. */
static XrXirStatus conditional_copy_bodies(const XrXirCompileContext *context,bool oracle) {
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    EffectInvocationCertificate *copy=status==XR_XIR_OK?xir_compile_calloc(context,1,sizeof(*copy),&status):NULL;
    if (copy) { copy->resources=context->resources;copy->terms.remaining=context;
        status=effect_invocation_copy_bodies(copy,xr_xir_compile_artifact_module(checked)); }
    XrXirInstruction saved[2];memcpy(saved,fixture.apply,sizeof(saved));
    if (status==XR_XIR_OK && oracle) {
        const XrXirModule *original=xr_xir_compile_artifact_module(checked);
        CHECK(copy->bodies.function_count==original->function_count && !copy->bodies.provenance);
        for (uint32_t f=0;f<original->function_count;++f) {
            const XrXirFunction *a=&original->functions[f],*b=&copy->bodies.functions[f];
            CHECK(a->result==b->result && a->name_length==b->name_length &&
                a->parameter_count==b->parameter_count && a->instruction_count==b->instruction_count &&
                a->block_count==b->block_count && a->operand_count==b->operand_count);
            CHECK(!memcmp(a->name,b->name,a->name_length));
            if (a->parameter_count) CHECK(a->parameters!=b->parameters &&
                !memcmp(a->parameters,b->parameters,a->parameter_count*sizeof(*a->parameters)));
            CHECK(a->instructions!=b->instructions &&
                !memcmp(a->instructions,b->instructions,a->instruction_count*sizeof(*a->instructions)));
            CHECK(a->blocks!=b->blocks && !memcmp(a->blocks,b->blocks,a->block_count*sizeof(*a->blocks)));
            if (a->operand_count) CHECK(a->operands!=b->operands &&
                !memcmp(a->operands,b->operands,a->operand_count*sizeof(*a->operands)));
        }
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK && oracle) {
        CHECK(copy->bodies.functions[1].instruction_count==2 &&
            !memcmp(copy->bodies.functions[1].instructions,saved,sizeof(saved)));
        CHECK(!memcmp(copy->bodies.functions[1].name,"apply",5));
        CHECK(copy->terms.types.count==2 && copy->terms.types.nodes[1].parameter_count==1 &&
            copy->terms.types.nodes[1].parameters[0].mode==XR_PARAM_REF);
    }
    effect_invocation_certificate_free(copy);return status;
}

static XrXirStatus conditional_copy_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    return mode<2?conditional_copy_bytes(context,mode,oracle):conditional_copy_bodies(context,oracle);
}

/* The copy fee is refused after storage admission and before reading source.
 * Destruction reaches initialized headers only; a fresh pool can reuse the owner. */
static void conditional_copy_refusals(void) {
    unsigned char source[17]={0};
    for (uint32_t mode=0;mode<3;++mode) {
        RootParameterMark physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
        if (!mode) limits.work=1+18+sizeof(EffectTermMemory)+1+sizeof(source)-1;
        XrXirCompileContext context=rp_owner(limits);uint64_t baseline=rp_stats(&context).live_bytes;
        EffectTerms terms={.remaining=&context};
        if (!mode) CHECK(effect_invocation_copy(&terms,source,sizeof(source),1)==NULL &&
            terms.status==XR_XIR_BUDGET && terms.memory && terms.memory->used>=sizeof(source));
        else if (mode==1) CHECK(effect_invocation_copy(&terms,source,UINT64_MAX,1)==NULL &&
            terms.status==XR_XIR_BUDGET && !terms.memory);
        else CHECK(effect_invocation_copy(&terms,source,1,0)==NULL &&
            terms.status==XR_XIR_BAD_STRUCTURE && !terms.memory);
        XrXirStatus first=terms.status;uint64_t work=rp_stats(&context).work;
        CHECK(effect_invocation_copy(&terms,NULL,1,1)==NULL && terms.status==first && rp_stats(&context).work==work);
        effect_terms_free(&terms);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;EffectTerms failed={.remaining=&context};
    rp_attempts=0;rp_fail_at=0;rp_injected=false;
    CHECK(effect_invocation_copy(&failed,source,sizeof(source),1)==NULL &&
        rp_injected && failed.status==XR_XIR_OUT_OF_MEMORY && !failed.memory);
    rp_fail_at=SIZE_MAX;CHECK(effect_invocation_copy(&failed,NULL,1,1)==NULL && failed.status==XR_XIR_OUT_OF_MEMORY);
    effect_terms_free(&failed);EffectTerms retry={.remaining=&context};
    CHECK(effect_invocation_copy(&retry,source,sizeof(source),1)!=NULL && retry.status==XR_XIR_OK);
    effect_terms_free(&retry);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void conditional_copy_literals(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_copy_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    conditional_copy_refusals();
}

static void conditional_copy_resources(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
            uint64_t baseline=entry.live_bytes;RootParameterMark retained=rp_mark();
            XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_copy_case(&context,mode,false);
            if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context); }
            else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
        ConditionalRetryTrial retry={.which=mode+69,.ordinal=pass-1,.sites=sites,
            .resources=identity,.initial=entry,.single=single,.retained=retained,.first=status};
        conditional_retry_operation(&context,&retry);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(physical);
        }
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        size_t census_attempts=rp_attempts;
        CHECK(conditional_copy_case(&context,mode,false)==XR_XIR_OK);
        XrCompileResourceStats census=rp_stats(&context);size_t census_sites=rp_attempts-census_attempts;
        rp_owner_free(&context,baseline);rp_balanced(physical);
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t pass=0;pass<2;++pass) {
            XrCompileResourceLimits limits=rp_caps();uint64_t value=measured[axis]-(pass?0:1);
            if (axis==0) limits.allocated_bytes=value;
            else if (axis==1) limits.live_bytes=value;
            else limits.work=value;
            physical=rp_mark();context=rp_owner(limits);XrCompileResourceStats entry=rp_stats(&context);
                baseline=entry.live_bytes;RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            CHECK(conditional_copy_case(&context,mode,false)==(pass?XR_XIR_OK:XR_XIR_BUDGET));
        ConditionalRetryTrial retry={.which=mode+69,.ordinal=SIZE_MAX,.sites=census_sites,
            .resources=identity,.initial=entry,.single=census,.retained=retained,.first=pass?XR_XIR_OK:XR_XIR_BUDGET};
        conditional_retry_operation(&context,&retry);
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}
#endif // XIR_INVOCATION_COPY_STORAGE_CASES_H
