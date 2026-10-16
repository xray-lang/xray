/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_node_key_cases.h - Exact storage keys under forced collisions
 */
#ifndef XIR_INVOCATION_NODE_KEY_CASES_H
#define XIR_INVOCATION_NODE_KEY_CASES_H
/* These literal rows have no body material, effect permission or certificate.
 * A shared bucket hint must not erase a root, body, parameter or input word. */
static void invocation_node_key_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    uint64_t input[6][4]={{1,2,3,4},{1,2,3,4},{1,2,3,4},{1,2,3,4},{1,2,3,5},{5,2,3,4}};
    EffectInvocationNodeKey keys[6]={{1,2,2,7,4,1,input[0]},{2,2,2,7,4,1,input[1]},
        {1,3,2,7,4,1,input[2]},{1,2,3,7,4,1,input[3]},
        {1,2,2,7,4,1,input[4]},{1,2,2,7,4,1,input[5]}};
    EffectInvocationNode nodes[6]={0};EffectInvocationWorkspace owner={.work=&context,.final={.nodes=nodes}};
    XrXirStatus allocated=XR_XIR_OK;uint32_t *slots=xir_compile_calloc(&context,16,sizeof(*slots),&allocated);CHECK(allocated==XR_XIR_OK && slots);
    for(uint32_t n=0;n<6;++n){size_t at=SIZE_MAX;CHECK(effect_invocation_node_slot(&owner,slots,16,&keys[n],&at)==XR_XIR_OK && !slots[at]);
        CHECK(xir_compile_work(&context,sizeof(*nodes)+sizeof(*slots)));
        nodes[n]=(EffectInvocationNode){.root=keys[n].root,.body=keys[n].body,.parameter_count=keys[n].parameters,.instruction_count=7,.input=input[n]};slots[at]=n+1;++owner.final.count;
    }
    for(uint32_t n=0;n<6;++n){size_t at=SIZE_MAX;CHECK(effect_invocation_node_slot(&owner,slots,16,&keys[n],&at)==XR_XIR_OK && slots[at]==n+1);}
    uint64_t equal[4]={1,2,3,4};EffectInvocationNodeKey copied=keys[0];copied.input=equal;size_t at=SIZE_MAX;
    CHECK(effect_invocation_node_slot(&owner,slots,16,&copied,&at)==XR_XIR_OK && slots[at]==1);
    copied.instructions=8;at=SIZE_MAX;CHECK(effect_invocation_node_slot(&owner,slots,16,&copied,&at)==XR_XIR_BAD_STRUCTURE && at==SIZE_MAX);
    uint32_t saved[16];memcpy(saved,slots,sizeof(saved));for(unsigned i=0;i<16;++i)slots[i]=UINT32_MAX;
    at=SIZE_MAX;CHECK(effect_invocation_node_slot(&owner,slots,16,&keys[0],&at)==XR_XIR_BAD_STRUCTURE && at==SIZE_MAX);memcpy(slots,saved,sizeof(saved));
    CHECK(effect_invocation_node_slot(&owner,slots,15,&keys[0],&at)==XR_XIR_BAD_STRUCTURE && at==SIZE_MAX);
    xr_compile_resources_free(slots);rp_owner_free(&context,baseline);rp_balanced(physical);
}
#endif // XIR_INVOCATION_NODE_KEY_CASES_H
