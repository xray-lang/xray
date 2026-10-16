/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_precision_needed_cases.h - Conservative dynamic replay selection
 */
#ifndef XIR_INVOCATION_PRECISION_NEEDED_CASES_H
#define XIR_INVOCATION_PRECISION_NEEDED_CASES_H

/* Selection only chooses whether to replay after initial expansion. None of
 * these physical literals is an admitted program or a ROOT permission proof. */
static void invocation_precision_needed_literals(void) {
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;XrXirType parameter=XR_XIR_I64;
    EffectBodyFunction body={.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64};
    XrXirNominalDeclaration declaration={.kind=XR_XIR_NOMINAL_CLASS};
    XrXirNominalTable nominals={&declaration,1,NULL};
    XrXirTypeNode type={.kind=XR_XIR_TYPE_NOMINAL};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&body};
    material.terms.types=(XrXirTypes){.nodes=&type,.count=1,.nominals=&nominals};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectPhysicalFunction function={.parameters=1,.values=1};
    EffectPhysicalDomain physical={.resources=context.resources,.count=1,.functions=&function};
    material.physical=&physical;
    EffectInvocationNode node={.parameter_count=1};EffectInvocationState state={.expanded=true};
    EffectInvocationWorkspace owner={.work=&context,.body=effect_body_owned_request(&context,&material),
        .states=&state,.final={.count=1,.nodes=&node,.physical=&physical,.material=&material}};
    bool needed=true;CHECK(effect_invocation_precision_needed(&owner,0,&needed)==XR_XIR_OK && !needed);
    for(unsigned mode=0;mode<9;++mode){
        if(!mode)function.rows=1;
        else if(mode==1)function.classes=1;
        else if(mode==2)node.class_count=1;
        else if(mode==3)node.producer_count=1;
        else if(mode==4)node.deferred=true;
        else if(mode==5)node.local_deferred=true;
        else if(mode==6)body.result=(XrXirType)256;
        else if(mode==7)node.class_return_mask=EFFECT_CLASS_KNOWN;
        else {type.kind=XR_XIR_TYPE_CALLABLE;type.result=XR_XIR_I64;body.result=(XrXirType)256;}
        needed=false;CHECK(effect_invocation_precision_needed(&owner,0,&needed)==XR_XIR_OK && needed);
        function.rows=function.classes=node.class_count=node.producer_count=node.class_return_mask=0;
        node.deferred=node.local_deferred=false;body.result=XR_XIR_I64;
    }
    state.expanded=false;needed=true;
    CHECK(effect_invocation_precision_needed(&owner,0,&needed)==XR_XIR_BAD_STRUCTURE && needed);
    rp_owner_free(&context,baseline);rp_balanced(initial);
}
#endif // XIR_INVOCATION_PRECISION_NEEDED_CASES_H
