/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_nonroot_seed_cases.h - Genuine nonroot environment seed loss
 */
#ifndef XIR_INVOCATION_NONROOT_SEED_CASES_H
#define XIR_INVOCATION_NONROOT_SEED_CASES_H
#include "xir_invocation_input_seed_range_cases.h"
static void invocation_nonroot_slot_erasure(const XrXirCompileContext *work,
    EffectInvocationCertificate *certificate) {
    EffectInvocationFinal *final=certificate->final;
    uint32_t caller=final->roots[0],symbolic=final->roots[1],target=UINT32_MAX;
    for (uint32_t n=0;n<final->count;++n)
        if (final->nodes[n].body==1 && final->nodes[n].root==final->nodes[caller].root && n!=symbolic)
            target=n;
    CHECK(target<final->count && target!=caller &&
        final->nodes[target].local_mask&XR_XIR_CALLABLE_ROOT_REQUIRED);
    CHECK(effect_invocation_closed_read(work,final)==XR_XIR_OK);
    EffectInvocationNode source=final->nodes[target],parent=final->nodes[caller];
    EffectInvocationCause source_causes[2]={source.causes[0],effect_invocation_local_causes(&source)[0]};
    EffectInvocationCause parent_causes[2]={parent.causes[0],effect_invocation_local_causes(&parent)[0]};
    EffectFinalFunctions *summary=&final->summary;XrXirRootEffects facts=summary->facts[0];
    uint32_t mask=summary->functions[0].formula.constant_mask;
    XrXirRootEffectWitness witnesses[4];
    for (uint32_t d=0;d<4;++d) {
        witnesses[d]=summary->witnesses[(size_t)d*summary->count];
        if (!(d&1)) summary->witnesses[(size_t)d*summary->count]=(XrXirRootEffectWitness){0};
    }
    const EffectInvocationCause absent={.next=UINT32_MAX,.edge=UINT32_MAX,.distance=UINT32_MAX};
    final->nodes[target].intrinsic_mask&=~XR_XIR_CALLABLE_ROOT_REQUIRED;
    final->nodes[target].local_mask&=~XR_XIR_CALLABLE_ROOT_REQUIRED;
    final->nodes[target].causes[0]=effect_invocation_local_causes(&final->nodes[target])[0]=absent;
    final->nodes[caller].intrinsic_mask&=~XR_XIR_CALLABLE_ROOT_REQUIRED;
    final->nodes[caller].causes[0]=absent;
    summary->functions[0].formula.constant_mask&=~XR_XIR_CALLABLE_ROOT_REQUIRED;
    summary->facts[0].requires_root=false;
    CHECK(final->nodes[symbolic].intrinsic_mask&XR_XIR_CALLABLE_ROOT_REQUIRED);
    CHECK(certificate->body->local_roots[1].requires_root &&
        certificate->body->declarations->slots[0].mutable);
    /* The receiving rows still form an exact union. Only current primitive
     * Input distinguishes this erased key from its intact symbolic root. */
    CHECK(effect_invocation_closed_read(work,final)==XR_XIR_OK);
    XrXirStatus status=effect_invocation_publication_match(work,certificate,summary);
    fprintf(stderr,"nonroot mutable-slot receiving erasure status%u symbolic%u target%u caller%u\n",
        status,symbolic,target,caller);
    CHECK(status==XR_XIR_BAD_STRUCTURE);
    final->nodes[target]=source;final->nodes[caller]=parent;
    final->nodes[target].causes[0]=source_causes[0];effect_invocation_local_causes(&final->nodes[target])[0]=source_causes[1];
    final->nodes[caller].causes[0]=parent_causes[0];effect_invocation_local_causes(&final->nodes[caller])[0]=parent_causes[1];
    summary->facts[0]=facts;summary->functions[0].formula.constant_mask=mask;
    for (uint32_t d=0;d<4;++d) summary->witnesses[(size_t)d*summary->count]=witnesses[d];
    CHECK(effect_invocation_publication_match(work,certificate,summary)==XR_XIR_OK);
}
#endif // XIR_INVOCATION_NONROOT_SEED_CASES_H
