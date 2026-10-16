/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_replay_scratch_cases.h - Nested replay lifetime and reset controls
 */
static void invocation_replay_scratch_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    EffectPhysicalRecord record={.value=0,.mask=EFFECT_PHYSICAL_CALLABLE};
    EffectPhysicalFunction body={.values=3,.parameters=1,.instructions=2,.rows=1,
        .count=1,.capacity=1,.records=&record};
    EffectPhysicalDomain domain={.resources=context.resources,.count=1,.functions=&body};
    EffectInvocationNode node={.body=0};
    EffectInvocationFinal final={.count=1,.nodes=&node,.physical=&domain};
    EffectInvocationRead owner={.work=&context,.final=&final};
    owner.replay_scratch=&owner.scratch_head;
    EffectBodyFunctionHead head={.parameter_count=1,.instruction_count=2};
    EffectInvocationBasis basis={.fn_words=1,.cell_words=1};
    EffectInvocationFlow first={.read=&owner,.function=&head,.basis=basis};
    CHECK(effect_invocation_flow_storage(&first)==XR_XIR_OK && first.memory && first.cell_row);
    first.rows[0]=17;first.fixed[0]=3;first.certified[0]=5;
    EffectInvocationFlow nested={.read=&owner,.function=&head,.basis=basis};
    CHECK(effect_invocation_flow_storage(&nested)==XR_XIR_OK && nested.memory!=first.memory &&
        nested.scratch!=first.scratch && first.rows[0]==17 && first.fixed[0]==3 && first.certified[0]==5);
    nested.rows[0]=19;nested.fixed[0]=7;nested.certified[0]=1;
    void *reusable=nested.memory;size_t attempts=rp_attempts;
    effect_invocation_flow_finish(&nested);
    CHECK(!nested.memory && !nested.scratch && !nested.cell_row);
    EffectInvocationFlow again={.read=&owner,.function=&head,.basis=basis};
    CHECK(effect_invocation_flow_storage(&again)==XR_XIR_OK && again.memory==reusable && rp_attempts==attempts &&
        !again.rows[0] && !again.fixed[0] && !again.certified[0] &&
        again.row_indices[0]==0 && again.row_indices[1]==UINT32_MAX && again.row_indices[2]==UINT32_MAX);
    CHECK(first.rows[0]==17 && first.fixed[0]==3 && first.certified[0]==5);
    effect_invocation_flow_finish(&again);effect_invocation_flow_finish(&first);
    EffectInvocationScratch *small=owner.scratch_head;
    CHECK(small && !small->claimed);
    EffectInvocationScratch *large=NULL;
    CHECK(effect_invocation_scratch_claim(&owner,8192,&large)==XR_XIR_OK && large && large->claimed);
    effect_invocation_scratch_release(&owner,large);
    for (EffectInvocationScratch *m=owner.scratch_head;m;m=m->next) CHECK(m->capacity<=4096 && !m->claimed);
    EffectInvocationScratch *blocked=NULL;
    CHECK(effect_invocation_scratch_claim(&owner,SIZE_MAX,&blocked)==XR_XIR_BUDGET && !blocked);
    effect_invocation_scratch_free(&owner);
    CHECK(!owner.scratch_head && rp_stats(&context).live_bytes==baseline);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void invocation_replay_scratch_failures(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        RootParameterMark physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
        if (mode==2) limits.work=sizeof(EffectInvocationScratch)-1;
        XrXirCompileContext context=rp_owner(limits);uint64_t baseline=rp_stats(&context).live_bytes;
        EffectInvocationRead owner={.work=&context};EffectInvocationScratch *memory=NULL;
        owner.replay_scratch=&owner.scratch_head;
        if (mode==1) {
            CHECK(effect_invocation_scratch_claim(&owner,16,&memory)==XR_XIR_OK);
            effect_invocation_scratch_release(&owner,memory);memory=NULL;
        }
        EffectInvocationScratch *retained=owner.scratch_head;
        uint64_t live=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;rp_injected=false;
        if (mode<2) rp_fail_at=attempts;
        XrXirStatus expected=mode<2?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
        CHECK(effect_invocation_scratch_claim(&owner,32,&memory)==expected && !memory &&
            owner.scratch_head==retained && rp_stats(&context).live_bytes==live);
        CHECK(mode<2 ? rp_injected && rp_attempts==attempts+1 : !rp_injected);
        rp_fail_at=SIZE_MAX;attempts=rp_attempts;
        if (mode<2) {
            CHECK(effect_invocation_scratch_claim(&owner,32,&memory)==XR_XIR_OK && memory && rp_attempts==attempts+1);
            effect_invocation_scratch_release(&owner,memory);
        }
        effect_invocation_scratch_free(&owner);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}
