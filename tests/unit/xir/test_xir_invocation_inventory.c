/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_invocation_inventory.c - Current callable basis after the last receiving free
 */
#define main inventory_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main
static EffectInvocationFinal *inventory_final;
static uint32_t inventory_frees,inventory_cut,inventory_mode,inventory_attacks;
static void inventory_free(void *memory,size_t bytes) {
    CHECK(memory && bytes);++inventory_frees;
    if(inventory_frees!=inventory_cut || !inventory_final) return;
    pc_free_hook=NULL;++inventory_attacks;EffectInvocationFinal *final=inventory_final;
    switch(inventory_mode) {
    case 1: final->sites[0].target=1;break;
    case 2: final->sites[0].function=0;break;
    case 3: ++final->sites[0].instruction;break;
    case 4: ++final->binding_count;break;
    case 5: ++final->sites[0].captures;++final->binding_count;break;
    case 6: final->site_count=0;final->sites=NULL;final->binding_count=0;break;
    case 7: CHECK(final->count<final->capacity);final->nodes[final->count]=final->nodes[0];++final->count;break;
    case 8: final->root_spaces[1]=1;final->nodes[final->roots[1]].root=1;break;
    case 9: final->nodes[final->roots[1]].body=2;break;
    case 10: final->sites[1]=final->sites[0];break;
    case 11: { EffectInvocationSite temporary=final->sites[0];final->sites[0]=final->sites[1];final->sites[1]=temporary;break; }
    case 12: final->sites=(void *)(uintptr_t)1;break;
    case 13: final->nodes[final->roots[2]].input[0]|=1;break;
    default: CHECK(false);
    }
}
static void inventory_no_callback(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
static uint32_t inventory_source_once(uint32_t cut,unsigned mode) {
    XrXirTypeNode type={.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypes types={.nodes=&type,.count=1};XrXirType scalar=XR_XIR_I64;
    XrXirInstruction ret={.op=XR_XIR_RETURN};
    XrXirInstruction ops[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=2},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={1,1},.immediate=2},
        {.op=XR_XIR_RETURN,.args={1}}};uint32_t operands[2]={0,0};
    XrXirBlock blocks[3]={{.count=1},{.count=4},{.count=1}};
    XrXirFunction functions[3]={{.name="init",.name_length=4,.instructions=&ret,.instruction_count=1,.blocks=&blocks[0],.block_count=1},
        {.name="factory",.name_length=7,.result=(XrXirType)256,.instructions=ops,.instruction_count=4,.blocks=&blocks[1],.block_count=1,.operands=operands,.operand_count=2},
        {.name="target",.name_length=6,.parameters=&scalar,.parameter_count=1,.instructions=&ret,.instruction_count=1,.blocks=&blocks[2],.block_count=1}};
    const XrXirSourceModule source={.name="inventory",.name_length=9,.dependencies=NULL,
        .dependency_count=0,.initializer=0,.lexical_scope=0,.storage=0};
    const XrXirFunctionIdentity identities[3]={{.method_kind=XR_XIR_NON_MEMBER},{.method_kind=XR_XIR_NON_MEMBER},{.method_kind=XR_XIR_NON_MEMBER}};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirEffectParameter scalar_effect={0};
    XrXirFunctionEffectContract contracts[3]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0},{.parameter_count=1,.parameters=&scalar_effect}};
    const XrXirProvenance provenance={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=3};
    const XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_LIBRARY,.functions=functions,.function_count=3,.types=&types,.declarations=&declarations,.provenance=&provenance};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"inventory admission%u f%u i%u reason%u\n",status,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK);status=xr_xir_compile_effects_analyze(checked,&effects);
    if(status!=XR_XIR_OK) fprintf(stderr,"inventory infer%u\n",status);CHECK(status==XR_XIR_OK);
    EffectInvocationFinal *final=effects->invocations->final;CHECK(final->site_count==2 && final->binding_count==2);
    CHECK(final->sites[0].function==1 && final->sites[0].instruction==1 && final->sites[0].target==2 && final->sites[0].captures==1 && final->sites[0].binding==0);
    CHECK(final->sites[1].function==1 && final->sites[1].instruction==2 && final->sites[1].target==2 && final->sites[1].captures==1 && final->sites[1].binding==1);
    EffectInvocationSite *sites=final->sites,saved_sites[2];memcpy(saved_sites,sites,sizeof(saved_sites));
    uint64_t saved_input=final->nodes[final->roots[2]].input[0];
    uint32_t saved_count=final->count,saved_space=final->root_spaces[1];EffectInvocationNode saved_node=final->nodes[final->roots[1]];
    inventory_final=mode?final:NULL;inventory_frees=0;inventory_cut=cut;inventory_attacks=0;inventory_mode=mode;
    CHECK(!pc_free_hook);pc_free_hook=inventory_free;
    status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects);
    pc_free_hook=NULL;inventory_final=NULL;uint32_t frees=inventory_frees;
    CHECK(!mode || inventory_attacks==1);
    fprintf(stderr,"actual Source inventory mode%u cut%u frees%u status%u expected%u\n",mode,cut,frees,status,mode?XR_XIR_BAD_STRUCTURE:XR_XIR_OK);
    final->sites=sites;final->site_count=2;final->binding_count=2;memcpy(sites,saved_sites,sizeof(saved_sites));
    final->nodes[final->roots[2]].input[0]=saved_input;
    final->count=saved_count;final->root_spaces[1]=saved_space;final->nodes[final->roots[1]]=saved_node;
    CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(ops,0xDC,sizeof(ops));memset(functions,0xDC,sizeof(functions));
    CHECK(error_finite_outcomes_match(&context,effects->invocations->body,final->outcomes)==XR_XIR_OK);
    size_t allocated=attempts;uint64_t physical=live_bytes;pc_allocate_hook=inventory_no_callback;pc_free_hook=inventory_no_callback;
    CHECK(effect_final_inventories_match(&context,effects->invocations,final->outcomes)==XR_XIR_OK);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==allocated && live_bytes==physical);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    CHECK(status==(mode?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));return frees;
}
int main(int argc,char **argv) {
    CHECK(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);CHECK(mode<=13);
    uint32_t frees=inventory_source_once(UINT32_MAX,0);CHECK(frees);
    if(mode) CHECK(inventory_source_once(frees,mode)==frees);
    return 0;
}
