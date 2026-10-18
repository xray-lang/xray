/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_capture_final_view_cases.h - Sealed finite views after producer death
 *
 * KEY CONCEPT:
 *   A literal callable row survives producer retirement. Scratch allocation
 *   must reframe current dimensions, storage geometry and the actual key.
 */
#ifndef XIR_CAPTURE_FINAL_VIEW_CASES_H
#define XIR_CAPTURE_FINAL_VIEW_CASES_H
static EffectInvocationFinal *complete_view_target;
static uint32_t complete_view_selected,complete_view_attack,complete_view_callbacks;
static void complete_view_allocate(void *memory,size_t bytes) {
    (void)memory;(void)bytes;++complete_view_callbacks;
    EffectInvocationNode *node=&complete_view_target->nodes[complete_view_selected];
    if(complete_view_attack==0) ++node->instruction_count;
    if(complete_view_attack==1) complete_view_target->capture.count=complete_view_target->capture.capacity+1;
    if(complete_view_attack==2) ++complete_view_target->material->functions[node->body].instruction_count;
    if(complete_view_attack==3) node->root=1;
    if(complete_view_attack==4) ++complete_view_target->site_count;
}
static void complete_final_view_boundary(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate) {
    EffectInvocationFinal *final=certificate->final;uint32_t selected=final->roots[0];
    CHECK(final->site_count==1 && !final->binding_count && final->nodes[selected].body==0 &&
        final->nodes[selected].root==0 && !final->material->functions[1].parameter_count);
    EffectInvocationRead read={0};CHECK(effect_invocation_read_open(work,certificate,final,&read)==XR_XIR_OK);
    CHECK(!read.discovery);
    uint64_t outer=pc_stats(work).live_bytes;size_t physical=live_bytes,blocks=live;
    EffectInvocationFlow head={.read=&read,.node=selected};size_t before=attempts;
    CHECK(effect_invocation_final_head(&head,&read.body)==XR_XIR_OK && attempts==before &&
        !head.memory && !head.rows && !head.fixed && !head.certified);
    EffectInvocationFlow view={.read=&read,.node=selected};
    CHECK(effect_invocation_final_view(&view)==XR_XIR_OK && !view.changed && !view.certified &&
        view.basis.fn_words==1 && view.row_count==1 && view.values==3);
    const uint64_t *row=effect_invocation_row(&view,0);CHECK(row && row[0]==UINT64_C(3));
    bool certified=true;uint64_t paid=pc_stats(work).work;
    CHECK(effect_invocation_flag_get(&view,0,true,&certified)==XR_XIR_BAD_STRUCTURE && certified && pc_stats(work).work==paid);
    xr_compile_resources_free(view.memory);
    CHECK(pc_stats(work).live_bytes==outer && live_bytes==physical && live==blocks);
    EffectInvocationFlow denied={.read=&read,.node=selected};paid=pc_stats(work).work;before=attempts;
    CHECK(effect_invocation_discovery_origins(&denied)==XR_XIR_BAD_STRUCTURE && !denied.memory &&
        pc_stats(work).work==paid && attempts==before);
    denied.owner=(void *)(uintptr_t)1;
    CHECK(effect_invocation_final_view(&denied)==XR_XIR_BAD_STRUCTURE && !denied.memory &&
        pc_stats(work).work==paid && attempts==before);
    denied.owner=NULL;EffectInvocationRead forged=read;forged.discovery=(void *)(uintptr_t)1;denied.read=&forged;
    CHECK(effect_invocation_final_view(&denied)==XR_XIR_BAD_STRUCTURE && !denied.memory &&
        pc_stats(work).work==paid && attempts==before);
    for(uint32_t attack=0;attack<5;++attack) {
        EffectInvocationNode saved=final->nodes[selected];EffectCaptureProof proof=final->capture;
        uint32_t sites=final->site_count;
        EffectBodyFunction body=final->material->functions[saved.body];
        complete_view_target=final;complete_view_selected=selected;complete_view_attack=attack;complete_view_callbacks=0;
        view=(EffectInvocationFlow){.read=&read,.node=selected};CHECK(!pc_allocate_hook && !pc_free_hook);
        pc_allocate_hook=complete_view_allocate;
        XrXirStatus status=effect_invocation_final_view(&view);pc_allocate_hook=NULL;
        CHECK(status==XR_XIR_BAD_STRUCTURE && complete_view_callbacks==1 && view.memory);
        final->nodes[selected]=saved;final->capture=proof;final->site_count=sites;final->material->functions[saved.body]=body;
        xr_compile_resources_free(view.memory);
        CHECK(pc_stats(work).live_bytes==outer && live_bytes==physical && live==blocks);
        view=(EffectInvocationFlow){.read=&read,.node=selected};CHECK(effect_invocation_final_view(&view)==XR_XIR_OK);
        row=effect_invocation_row(&view,0);CHECK(row && row[0]==UINT64_C(3));xr_compile_resources_free(view.memory);
    }
    complete_view_target=NULL;view=(EffectInvocationFlow){.read=&read,.node=selected};
    fail_at=attempts;injected=false;XrXirStatus failed=effect_invocation_final_view(&view);fail_at=SIZE_MAX;
    CHECK(injected && failed==XR_XIR_OUT_OF_MEMORY && !view.memory);
    CHECK(pc_stats(work).live_bytes==outer && live_bytes==physical && live==blocks);
    view=(EffectInvocationFlow){.read=&read,.node=selected};CHECK(effect_invocation_final_view(&view)==XR_XIR_OK);
    row=effect_invocation_row(&view,0);CHECK(row && row[0]==UINT64_C(3));xr_compile_resources_free(view.memory);
    CHECK(pc_stats(work).live_bytes==outer && live_bytes==physical && live==blocks);
    effect_invocation_read_close(&read);
    fprintf(stderr,"sealed finite row literal, no discovery/certification capability, five allocator attacks and real allocation recovery passed\n");
}
#endif // XIR_CAPTURE_FINAL_VIEW_CASES_H
