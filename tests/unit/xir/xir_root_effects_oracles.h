/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_effects_oracles.h - Literal root facts and finite owned cause paths
 */
#ifndef XIR_ROOT_EFFECTS_ORACLES_H
#define XIR_ROOT_EFFECTS_ORACLES_H
typedef struct RootExpected {const char *name;bool root,unknown,closed_unknown;} RootExpected;
static const RootExpected root_expected[]={
    {"main",false,false,false},{"pure",false,false,false},{"selfPure",false,false,false},
    {"pureA",false,false,false},{"pureB",false,false,false},
    {"mutableRead",true,false,false},{"mutableAtomicContainerRead",true,false,false},{"projection",true,false,false},
    {"constRead",false,false,false},{"constAtomicRead",false,false,false},{"constBoxRead",true,false,false},
    {"atomicParameter",false,false,false},{"atomicSnapshot",true,false,false},
    {"local",false,false,false},{"localCell",false,false,false},{"localBox",false,false,false},
    {"relay",true,false,false},{"rootA",true,false,false},{"rootB",true,false,false},
    {"rootFail",true,false,false},{"invoke",true,false,false},{"defaultOwner",false,false,false},
    {"defaultUse",true,false,false},{"cleanupOwner",true,false,false},
    {"indirect",false,true,true},{"indirectInvoke",false,true,true},
    {"requirement",false,true,false},{"instantiateRequirement",false,true,false},
    {"reference",false,false,false},{"mixed",true,true,true},{"mixedRelay",true,true,true},
    {"worker",false,false,false},{"goParent",true,false,false},
    {"left",true,false,false},{"right",true,false,false},{"diamond",true,false,false},{"emptyUse",false,false,false}};
static uint32_t root_find(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==length&&!memcmp(module->functions[f].name,name,length))return f;
    fprintf(stderr,"root oracle missing function %s\n",name);return UINT32_MAX;
}
static void root_fact(const XrXirEffects *effects,uint32_t f,bool root,bool unresolved) {
    const XrXirRootEffects *fact=xr_xir_effects_root(effects,f);
    if(!fact||fact->requires_root!=root||fact->unresolved!=unresolved)
        fprintf(stderr,"root f%u expected(%u,%u) actual(%u,%u)\n",f,root,unresolved,
            fact?fact->requires_root:9,fact?fact->unresolved:9);
    CHECK(fact&&fact->requires_root==root&&fact->unresolved==unresolved);
    CHECK((xr_xir_effects_root_witness(effects,f)!=NULL)==root);
    CHECK((xr_xir_effects_unresolved_witness(effects,f)!=NULL)==unresolved);
}
static void root_paths(const XrXirModule *module,const XrXirEffects *effects) {
    for(uint32_t f=0;f<module->function_count;++f)for(unsigned kind=0;kind<2;++kind){
        uint32_t current=f,steps=0;
        const XrXirRootEffectWitness *w=kind?xr_xir_effects_unresolved_witness(effects,current):xr_xir_effects_root_witness(effects,current);
        if(!w)continue;
        while(w->distance){
            CHECK(++steps<module->function_count&&w->callee<module->function_count);
            CHECK(w->cause==XR_XIR_ROOT_CAUSE_CALL||w->cause==XR_XIR_ROOT_CAUSE_CLEANUP);
            CHECK(w->instruction<module->functions[current].instruction_count&&w->slot==UINT32_MAX);
            XrXirOp op=module->functions[current].instructions[w->instruction].op;
            CHECK(w->cause==XR_XIR_ROOT_CAUSE_CLEANUP?op==XR_XIR_CLEANUP_REGISTER:
                op==XR_XIR_CALL||op==XR_XIR_INVOKE||op==XR_XIR_CALL_DEFAULT||op==XR_XIR_INVOKE_DEFAULT);
            uint32_t distance=w->distance;current=w->callee;
            w=kind?xr_xir_effects_unresolved_witness(effects,current):xr_xir_effects_root_witness(effects,current);
            CHECK(w&&w->distance+1==distance);
        }
        CHECK(w->callee==UINT32_MAX);
        if(w->instruction==UINT32_MAX){
            CHECK(!kind&&w->cause==XR_XIR_ROOT_CAUSE_INITIALIZER&&w->slot==UINT32_MAX);
            CHECK(module->declarations->modules[module->declarations->functions[current].module].initializer==current);
        }else{
            CHECK(w->instruction<module->functions[current].instruction_count);
            if(kind)CHECK(w->cause==XR_XIR_ROOT_CAUSE_INDIRECT||w->cause==XR_XIR_ROOT_CAUSE_REQUIREMENT);
            else CHECK(w->slot<module->declarations->slot_count);
        }
    }
}
static void root_source_oracles(const XrXirArtifact *artifact,bool closed) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);XrXirEffects *effects=NULL;
    CHECK(effect_analyze(artifact,&effects)==XR_XIR_OK);
    for(size_t e=0;e<sizeof(root_expected)/sizeof(root_expected[0]);++e){
        const RootExpected *expected=&root_expected[e];uint32_t matches=0;
        for(uint32_t f=0;f<module->function_count;++f){
            const XrXirFunction *fn=&module->functions[f];size_t length=strlen(expected->name);
            if(fn->name_length!=length||memcmp(fn->name,expected->name,length))continue;
            root_fact(effects,f,expected->root,closed?expected->closed_unknown:expected->unknown);++matches;
        }
        if(!matches)fprintf(stderr,"root stage%u missing %s\n",module->stage,expected->name);
        CHECK(matches||(closed&&!strcmp(expected->name,"requirement")));
    }
    bool empty_initializer=false;
    for(uint32_t m=0;m<module->declarations->module_count;++m){
        uint32_t initializer=module->declarations->modules[m].initializer;root_fact(effects,initializer,true,false);
        const XrXirFunction *body=&module->functions[initializer];
        if(body->instruction_count==1&&body->instructions[0].op==XR_XIR_RETURN){
            const XrXirRootEffectWitness *w=xr_xir_effects_root_witness(effects,initializer);
            CHECK(w->cause==XR_XIR_ROOT_CAUSE_INITIALIZER&&w->instruction==UINT32_MAX&&w->distance==0);
            empty_initializer=true;
        }
    }
    CHECK(empty_initializer);
    uint32_t box=root_find(module,"constBoxRead"),projection=root_find(module,"projection"),cleanup=root_find(module,"cleanupOwner");
    CHECK(box<module->function_count&&projection<module->function_count&&cleanup<module->function_count);
    CHECK(xr_xir_effects_root_witness(effects,box)->cause==XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST);
    CHECK(xr_xir_effects_root_witness(effects,projection)->cause==XR_XIR_ROOT_CAUSE_MUTABLE_SLOT);
    CHECK(xr_xir_effects_root_witness(effects,cleanup)->cause==XR_XIR_ROOT_CAUSE_CLEANUP);
    uint32_t diamond=root_find(module,"diamond"),right=root_find(module,"right");
    CHECK(diamond<module->function_count&&right<module->function_count);
    const XrXirRootEffectWitness *chosen=xr_xir_effects_root_witness(effects,diamond);
    CHECK(chosen->cause==XR_XIR_ROOT_CAUSE_CALL&&chosen->instruction==0&&chosen->callee==right&&chosen->distance==2);
    root_paths(module,effects);
    CHECK(!xr_xir_effects_root(NULL,0)&&!xr_xir_effects_root(effects,module->function_count));
    CHECK(!xr_xir_effects_root_witness(NULL,0)&&!xr_xir_effects_unresolved_witness(effects,UINT32_MAX));
    effect_summary_free(effects);CHECK(effect_balanced());
}
static void root_permutation(void) {
    XrXirRootEffects facts[4]={{true,false},{true,false},{true,false},{true,false}};
    XrXirRootEffectWitness expected[4]={{XR_XIR_ROOT_CAUSE_CALL,2,2,UINT32_MAX,2},
        {XR_XIR_ROOT_CAUSE_CALL,0,3,UINT32_MAX,1},{XR_XIR_ROOT_CAUSE_CALL,0,3,UINT32_MAX,1},
        {XR_XIR_ROOT_CAUSE_MUTABLE_SLOT,0,UINT32_MAX,0,0}};
    for(unsigned reverse=0;reverse<2;++reverse){
        XrXirRootEffectWitness witnesses[4]={{0}};witnesses[3]=expected[3];
        XrXirEffects effects={.count=4,.root=facts,.root_witnesses=witnesses};
        uint32_t heads[]={UINT32_MAX,2,3,reverse?1u:0u},queue[4]={0};
        EffectEdge edges[]={{1,reverse?UINT32_MAX:1u,0,false},{2,reverse?0u:UINT32_MAX,0,false},
            {0,UINT32_MAX,8,false},{0,UINT32_MAX,2,false}};
        EffectGraph graph={.heads=heads,.queue=queue,.edges=edges};
        CHECK(effect_root_witnesses(&effects,&graph,&effect_context,false)==XR_XIR_OK);
        for(uint32_t f=0;f<4;++f){
            CHECK(witnesses[f].cause==expected[f].cause&&witnesses[f].instruction==expected[f].instruction);
            CHECK(witnesses[f].callee==expected[f].callee&&witnesses[f].slot==expected[f].slot&&witnesses[f].distance==expected[f].distance);
        }
    }
}
#endif // XIR_ROOT_EFFECTS_ORACLES_H
