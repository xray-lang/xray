/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_primitive_cases.h - Current primitive Input across allocation
 */
#ifndef XIR_INVOCATION_PRIMITIVE_CASES_H
#define XIR_INVOCATION_PRIMITIVE_CASES_H
static EffectBodyMaterial *primitive_callback_body;
static uint32_t primitive_callback_kind,primitive_callback_count;
static void primitive_input_allocated(void *memory) {
    (void)memory;++primitive_callback_count;
    if(!primitive_callback_kind) --primitive_callback_body->domain.function_count;
    else ++primitive_callback_body->functions[0].instruction_count;
}
static void invocation_primitive_allocation_controls(const XrXirCompileContext *work,
    EffectBodyMaterial *body) {
    CHECK(body && effect_body_count(body)>1 && !rp_allocation_observer && !rp_release_observer);
    RootParameterMark held=rp_mark();uint64_t baseline=rp_stats(work).live_bytes;
    uint32_t count=body->domain.function_count,instructions=body->functions[0].instruction_count;
    for(uint32_t kind=0;kind<2;++kind) {
        EffectInvocationWorkspace probe={.final={.material=body},
            .read={.work=work,.body=effect_body_owned_request(work,body)}};
        primitive_callback_body=body;primitive_callback_kind=kind;primitive_callback_count=0;
        rp_allocation_observer=primitive_input_allocated;
        XrXirStatus status=effect_invocation_intrinsics(&probe);rp_allocation_observer=NULL;
        CHECK(status==XR_XIR_BAD_STRUCTURE && primitive_callback_count==1 && probe.primitives);
        body->domain.function_count=count;body->functions[0].instruction_count=instructions;
        effect_invocation_discovery_clear(&probe);
        CHECK(!probe.primitives && !probe.primitive_count && rp_stats(work).live_bytes==baseline);
        rp_balanced(held);
    }
    primitive_callback_body=NULL;
    EffectInvocationWorkspace probe={.final={.material=body},
        .read={.work=work,.body=effect_body_owned_request(work,body)}};
    size_t previous_fail=rp_fail_at;bool previous_injected=rp_injected;
    rp_fail_at=rp_attempts;rp_injected=false;
    XrXirStatus status=effect_invocation_intrinsics(&probe);rp_fail_at=previous_fail;
    CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && !probe.primitives && !probe.primitive_count);
    rp_injected=previous_injected;
    CHECK(rp_stats(work).live_bytes==baseline);rp_balanced(held);
    CHECK(effect_invocation_intrinsics(&probe)==XR_XIR_OK && probe.primitives && probe.primitive_count==count);
    EffectInvocationPrimitive actual={0};CHECK(effect_invocation_primitive_read(work,body,0,&actual)==XR_XIR_OK &&
        actual.facts.requires_root==probe.primitives[0].facts.requires_root &&
        actual.facts.unresolved==probe.primitives[0].facts.unresolved &&
        effect_invocation_witness_equal(&actual.witnesses[0],&probe.primitives[0].witnesses[0]) &&
        effect_invocation_witness_equal(&actual.witnesses[1],&probe.primitives[0].witnesses[1]));
    effect_invocation_discovery_clear(&probe);CHECK(rp_stats(work).live_bytes==baseline);rp_balanced(held);
    fprintf(stderr,"primitive Input: two allocator geometry attacks, real OOM and retry passed\n");
}
#endif // XIR_INVOCATION_PRIMITIVE_CASES_H
