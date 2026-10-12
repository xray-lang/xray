/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_context_constraint_vector_cases.h - Complete contextual target arguments
 *
 * KEY CONCEPT:
 *   Literal declaration facts precede contextual resolution. Every target
 *   binder remains a goal, and a failed request owns no published result.
 */
#ifndef XIR_CONTEXT_CONSTRAINT_VECTOR_CASES_H
#define XIR_CONTEXT_CONSTRAINT_VECTOR_CASES_H

static XrXirStatus context_constraint_vector_request(const XrXirCompileContext *cv_work,
    unsigned cv_attack) {
    ConditionalRawFixture cv_fixture;conditional_raw_fixture(&cv_fixture,0);
    XrXirTypeNode cv_nodes[8];memcpy(cv_nodes,cv_fixture.nodes,sizeof(cv_fixture.nodes));
    cv_nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    cv_nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)262};
    cv_fixture.types.nodes=cv_nodes;cv_fixture.types.count=8;
    XrXirConstraint cv_goals[9]={{0}};cv_goals[0]=cv_fixture.bound;
    cv_goals[8].markers=XR_XIR_CONSTRAINT_SENDABLE;
    XrXirType cv_arguments[9]={(XrXirType)256,(XrXirType)262,(XrXirType)263,
        (XrXirType)256,(XrXirType)263,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,(XrXirType)263};
    cv_fixture.generics[1].constraints=cv_goals;cv_fixture.generics[1].parameter_count=9;
    cv_fixture.generics[2].arguments=cv_arguments;cv_fixture.generics[2].argument_count=9;
    cv_fixture.caller_ops[0].type_arguments[1]=9;
    XrXirStatus cv_status=xr_xir_compile_types_structure_verify(cv_work,&cv_fixture.types);
    if (cv_status==XR_XIR_OK) cv_status=xr_xir_compile_interfaces_verify_structure(cv_work,
        &cv_fixture.interfaces,&cv_fixture.types);
    if (cv_status==XR_XIR_OK) cv_status=xr_xir_compile_generics_structure_verify(cv_work,&cv_fixture.module);
    if (cv_status==XR_XIR_OK) cv_status=xr_xir_compile_implementations_verify(cv_work,&cv_fixture.module);
    if (cv_status!=XR_XIR_OK) return cv_status;
    /* These are explicit counterfactuals after valid declaration admission,
     * not inputs authorized by a fabricated effect or execution summary. */
    if (cv_attack==1) cv_goals[8].markers=XR_XIR_CONSTRAINT_ERROR;
    if (cv_attack==2) cv_arguments[0]=XR_XIR_I64;
    if (cv_attack==3) cv_nodes[6].element=(XrXirType)263;
    if (cv_attack==4) cv_fixture.caller_ops[0].type_arguments[1]=8;
    EffectContextRequest cv_request={&cv_fixture.module,&cv_fixture.types,NULL,0,2,0};
    EffectContextResolved *cv_resolved=NULL;
    cv_status=effect_context_resolve(cv_work,&cv_request,&cv_resolved);
    if (cv_status==XR_XIR_OK) {
        CHECK(!cv_attack && cv_resolved && cv_resolved->known && cv_resolved->target==1 &&
            cv_resolved->argument_count==9 && cv_resolved->arguments[0]==(XrXirType)256 &&
            cv_resolved->arguments[8]==(XrXirType)263);
    } else CHECK(!cv_resolved);
    effect_context_free(cv_resolved);return cv_status;
}

static void context_constraint_vector_controls(void) {
    for (unsigned cv_attack=0;cv_attack<5;++cv_attack) {
        XrXirCompileContext cv_work=rp_owner(rp_caps());uint64_t cv_live=rp_stats(&cv_work).live_bytes;
        XrXirStatus cv_expected=cv_attack>=1 && cv_attack<=3 ? XR_XIR_BAD_TYPE :
            cv_attack ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
        CHECK(context_constraint_vector_request(&cv_work,cv_attack)==cv_expected);
        CHECK(rp_stats(&cv_work).live_bytes==cv_live);
        CHECK(context_constraint_vector_request(&cv_work,0)==XR_XIR_OK);
        rp_owner_free(&cv_work,cv_live);
    }
    puts("context target vector: complete nominal/array TYPE roots, late goal, wrong actual, recursive child, arity and same-owner recovery");
}
#endif // XIR_CONTEXT_CONSTRAINT_VECTOR_CASES_H
