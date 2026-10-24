/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_input_seeds.c - Actual public environment seeds at final Source reception
 */
#define main root_seed_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main
static EffectInvocationCertificate *seed_certificate;
static uint32_t seed_cut,seed_frees,seed_attacks,seed_attack,seed_shape;
static const uint64_t seed_words[16]={8,4,4,4,8,28,4,28,8,24,40,56,8,24,4,28};
static void seed_free(void *memory,size_t bytes) {
    CHECK(memory && bytes);++seed_frees;
    if(!seed_certificate || seed_frees!=seed_cut) return;
    pc_free_hook=NULL;++seed_attacks;EffectInvocationFinal *final=seed_certificate->final;
    uint32_t function=seed_attack==2?4:seed_attack==3?5:3;
    EffectInvocationNode *node=&final->nodes[final->roots[function]];
    if(seed_attack<=3) {
        /* Adversarial writes do not call the ledger from its free callback. */
        memset(node->input,0,6*sizeof(*node->input));
    } else if(seed_attack==4) node->input[0]|=1;
    else if(seed_attack==5) {
        uint32_t mask=seed_shape&3,other=mask?0:2;
        seed_certificate->declared->functions[3].bounds[0].mask=other?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0;
        node->input[0]=seed_words[(seed_shape&12)|other];
    } else CHECK(false);
}
static void seed_forbid(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
static uint32_t seed_source_once(unsigned shape,uint32_t cut,unsigned attack) {
    const uint32_t flags[4]={XR_XIR_CALLABLE_ROOT_NONE,XR_XIR_CALLABLE_ROOT_REQUIRED,XR_XIR_CALLABLE_ROOT_UNRESOLVED,XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    uint32_t advertised=flags[shape&3],mask=advertised&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);bool ref=(shape&4)!=0,fixed=(shape&8)!=0;
    XrXirCallableParameter reference={(XrXirType)257,XR_PARAM_REF};
    XrXirNominalDeclaration declaration={.module={"inventory",9},.name={"Local",5},.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table={.declarations=&declaration,.count=1};
    XrXirTypeNode nodes[4]={{.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_CALLABLE,.flags=advertised,.parameters=ref?&reference:NULL,.parameter_count=ref?1u:0u}};
    XrXirTypes types={.nodes=nodes,.count=mask || ref?4u:3u,.nominals=&table};
    XrXirType scalar=XR_XIR_I64,callable=(XrXirType)(mask || ref?259:256),cell=(XrXirType)257,object=(XrXirType)258;
    XrXirInstruction ret={.op=XR_XIR_RETURN},escape[2]={{.op=XR_XIR_FUNCTION_WEAKEN,.type=callable},{.op=XR_XIR_RETURN}};
    XrXirInstruction ops[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=2},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={1,1},.immediate=2},
        {.op=XR_XIR_RETURN,.args={1}}};uint32_t operands[2]={0,0};
    XrXirBlock blocks[6]={{.count=1},{.count=4},{.count=1},{.count=fixed?2u:1u},{.count=1},{.count=1}};
    XrXirFunction functions[6]={
        {.name="init",.name_length=4,.instructions=&ret,.instruction_count=1,.blocks=&blocks[0],.block_count=1},
        {.name="factory",.name_length=7,.result=(XrXirType)256,.instructions=ops,.instruction_count=4,.blocks=&blocks[1],.block_count=1,.operands=operands,.operand_count=2},
        {.name="scalar",.name_length=6,.parameters=&scalar,.parameter_count=1,.instructions=&ret,.instruction_count=1,.blocks=&blocks[2],.block_count=1},
        {.name="callback",.name_length=8,.parameters=&callable,.parameter_count=1,.result=XR_XIR_UNIT,.instructions=fixed?escape:&ret,.instruction_count=fixed?2u:1u,.blocks=&blocks[3],.block_count=1},
        {.name="cell",.name_length=4,.parameters=&cell,.parameter_count=1,.instructions=&ret,.instruction_count=1,.blocks=&blocks[4],.block_count=1},
        {.name="object",.name_length=6,.parameters=&object,.parameter_count=1,.instructions=&ret,.instruction_count=1,.blocks=&blocks[5],.block_count=1}};
    const XrXirSourceModule source={.name="inventory",.name_length=9,.dependencies=NULL,.dependency_count=0,
        .initializer=0,.lexical_scope=0,.storage=XR_XIR_MODULE_FRESH_STORAGE};XrXirFunctionIdentity identities[6]={0};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirEffectParameter plain={0},callback={.kind=fixed?XR_XIR_EFFECT_PARAMETER_FIXED:XR_XIR_EFFECT_PARAMETER_VARIABLE,
        .uses=fixed?XR_XIR_EFFECT_USE_FIXED:0};
    XrXirFunctionEffectContract contracts[6]={{.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED}},{0},
        {.parameter_count=1,.parameters=&plain},{.parameter_count=1,.parameters=&callback},
        {.parameter_count=1,.parameters=&plain},{.parameter_count=1,.parameters=&plain}};
    const XrXirProvenance provenance={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=6};
    const XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_LIBRARY,.functions=functions,.function_count=6,.types=&types,.declarations=&declarations,.provenance=&provenance};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"seed admission%u shape%u f%u i%u reason%u\n",status,shape,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK);status=xr_xir_compile_effects_analyze(checked,&effects);
    if(status!=XR_XIR_OK) fprintf(stderr,"seed infer%u shape%u\n",status,shape);CHECK(status==XR_XIR_OK);
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationFinal *final=certificate->final;
    CHECK(final->site_count==2 && final->binding_count==2 && final->root_spaces[3]==3 && final->root_spaces[4]==4 && final->root_spaces[5]==5);
    uint64_t saved[3][16];EffectInvocationBasis basis={0};
    CHECK(effect_invocation_basis_parameters_read(&context,final,1,&basis)==XR_XIR_OK && basis.row_words==6 && basis.fn_words==5);
    for(uint32_t f=3;f<6;++f) {
        const EffectInvocationNode *node=&final->nodes[final->roots[f]];CHECK(node->root==f && node->body==f && node->parameter_count==1);
        memcpy(saved[f-3],node->input,(size_t)basis.row_words*sizeof(*node->input));
        for(uint32_t w=0;w<basis.row_words;++w) {
            uint64_t expected=f==3?(w?0:seed_words[shape]):w==basis.fn_words?(f==4?9u:12u):0;
            if(node->input[w]!=expected) fprintf(stderr,"seed shape%u f%u w%u actual%llu expected%llu kind%u\n",shape,f,w,(unsigned long long)node->input[w],(unsigned long long)expected,effect_final_outcomes(effects)->contracts[3].parameters[0].kind);
            CHECK(node->input[w]==expected);
        }
    }
    CHECK(certificate->declared->functions[3].count>=1 && certificate->declared->functions[3].bounds[0].value==0);
    CHECK(certificate->declared->functions[3].bounds[0].mask==mask);
    seed_certificate=attack?certificate:NULL;seed_cut=cut;seed_frees=0;seed_attacks=0;seed_attack=attack;seed_shape=shape;
    CHECK(!pc_free_hook);pc_free_hook=seed_free;
    status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects);
    pc_free_hook=NULL;seed_certificate=NULL;uint32_t frees=seed_frees;CHECK(!attack || seed_attacks==1);
    fprintf(stderr,"actual Source root seed shape%u attack%u cut%u frees%u status%u expected%u\n",shape,attack,cut,frees,status,attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK);
    for(uint32_t f=3;f<6;++f) memcpy(final->nodes[final->roots[f]].input,saved[f-3],(size_t)basis.row_words*sizeof(uint64_t));
    certificate->declared->functions[3].bounds[0].mask=mask;
    CHECK(xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(ops,0xDC,sizeof(ops));memset(functions,0xDC,sizeof(functions));
    size_t allocated=attempts;uint64_t physical=live_bytes;pc_allocate_hook=seed_forbid;pc_free_hook=seed_forbid;
    CHECK(effect_final_inventories_match(&context,certificate,final->outcomes)==XR_XIR_OK);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==allocated && live_bytes==physical);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    CHECK(status==(attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));return frees;
}
int main(int argc,char **argv) {
    CHECK(argc==3);unsigned shape=(unsigned)strtoul(argv[1],NULL,10),attack=(unsigned)strtoul(argv[2],NULL,10);CHECK(shape<16 && attack<6);
    uint32_t frees=seed_source_once(shape,UINT32_MAX,0);CHECK(frees);
    if(attack) CHECK(seed_source_once(shape,frees,attack)==frees);return 0;
}
