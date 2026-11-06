/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_role_row_read_cases.h - Scoped rp_live row reads without allocator callbacks
 */
static uint32_t pc_role_row_frees;
static void pc_role_row_free(void *memory) {
    size_t bytes=pc_final_physical_size(memory);
    CHECK(memory && bytes);++pc_role_row_frees;
}
static void pc_role_row_literals(const XrXirCompileContext *work,EffectInvocationCertificate *certificate) {
    EffectBodyMaterial *body=certificate->body;EffectRoleProof *proof=&certificate->final->role_proof;
    CHECK(body->functions[1].parameter_count==5);
    size_t before_attempts=rp_attempts;XrCompileResourceStats before=rp_stats(work);
    CHECK(!rp_release_observer);pc_role_row_frees=0;rp_release_observer=pc_role_row_free;
    EffectRoleParametersRead row={0};CHECK(effect_role_parameters_open(work,body,proof,1,0,&row)==XR_XIR_OK);
    for (uint32_t p=0;p<5;++p) {
        uint32_t kind=UINT32_MAX;CHECK(effect_role_parameters_value(&row,p,&kind)==XR_XIR_OK);
        CHECK(kind==XR_XIR_EFFECT_PARAMETER_VARIABLE);
    }
    uint32_t unchanged=UINT32_MAX;
    CHECK(effect_role_parameters_value(&row,5,&unchanged)==XR_XIR_BAD_STRUCTURE && unchanged==UINT32_MAX);
    XrCompileResourceStats paid=rp_stats(work);
    CHECK(effect_role_parameters_open(work,body,proof,1,0,&row)==XR_XIR_BAD_STRUCTURE && conditional_memory_unchanged(work,paid));
    row=(EffectRoleParametersRead){0};
    CHECK(effect_role_parameters_open(work,body,proof,1,5,&row)==XR_XIR_BAD_STRUCTURE);
    CHECK(!row.work && !row.material && !row.proof && !row.function);
    XrXirType *parameters=(XrXirType *)body->functions[1].parameters;XrXirType saved_parameter=parameters[0];
    parameters[0]=XR_XIR_I64;
    CHECK(effect_role_parameters_open(work,body,proof,1,0,&row)==XR_XIR_BAD_STRUCTURE);
    parameters[0]=saved_parameter;
    CHECK(!row.work && !row.material && !row.proof && !row.function);
    EffectRoleProof original=*proof;EffectRoleFunction saved_row=proof->rows[1];
    EffectBodyFunction *functions=body->functions;EffectBodyFunction head=functions[1];
    const XrXirTypeNode *nodes=body->terms.types.nodes;
    for (uint32_t mode=0;mode<10;++mode) {
        if (mode==0) proof->facts=(void *)(uintptr_t)1;
        if (mode==1) proof->rows=(void *)(uintptr_t)1;
        if (mode==2) ++proof->functions;
        if (mode==3) ++proof->total;
        if (mode==4) body->functions=(void *)(uintptr_t)1;
        if (mode==5) functions[1].parameters=(void *)(uintptr_t)1;
        if (mode==6) body->terms.types.nodes=(void *)(uintptr_t)1;
        if (mode==7) original.rows[1].first=proof->total;
        if (mode==8) proof->resources=(void *)(uintptr_t)1;
        if (mode==9) --functions[1].parameter_count;
        CHECK(effect_role_parameters_open(work,body,proof,1,0,&row)==XR_XIR_BAD_STRUCTURE);
        CHECK(!row.work && !row.material && !row.proof && !row.function);
        *proof=original;original.rows[1]=saved_row;body->functions=functions;functions[1]=head;body->terms.types.nodes=nodes;
        CHECK(effect_role_parameters_open(work,body,proof,1,0,&row)==XR_XIR_OK);
        uint32_t kind=UINT32_MAX;CHECK(effect_role_parameters_value(&row,4,&kind)==XR_XIR_OK && kind==XR_XIR_EFFECT_PARAMETER_VARIABLE);
        row=(EffectRoleParametersRead){0};
    }
    rp_release_observer=NULL;XrCompileResourceStats after=rp_stats(work);
    CHECK(rp_attempts==before_attempts && !pc_role_row_frees && after.allocation_count==before.allocation_count &&
        after.allocated_bytes==before.allocated_bytes && after.held_bytes==before.held_bytes);
    fputs("scoped actual role row: five literal values, ten geometry/two callable-entry refusals, unchanged outputs, reopen, allocation0/free0 passed\n",stderr);
}
