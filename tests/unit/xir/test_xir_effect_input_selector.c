/* An empty opcode result must still reject impossible owned storage. */
#define main selector_original_pending_entry
#include "test_xir_error_cfg_pending.c"
#undef main
static void selector_no_callback(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
static void selector_scalar_domain(void) {
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE}};
    XrXirTypes types={.nodes=nodes,.count=2};XrXirType parameter=XR_XIR_I64;
    XrXirInstruction done={.op=XR_XIR_RETURN},ops[3]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},{.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock blocks[2]={{.count=1},{.count=3}};
    XrXirFunction functions[2]={{.name="init",.name_length=4,.instructions=&done,.instruction_count=1,.blocks=blocks,.block_count=1},
        {.name="scalar",.name_length=6,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
         .instructions=ops,.instruction_count=3,.blocks=&blocks[1],.block_count=1}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirSourceModule identity={.name="scalar",.name_length=6};
    XrXirDeclarations declarations={.modules=&identity,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule module={.stage=XR_XIR_BUILT,.types=&types,.functions=functions,.function_count=2,
        .declarations=&declarations,.linkage_kind=XR_XIR_LIBRARY};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_infer_verified(&context,&module,&effects)==XR_XIR_OK);
    EffectInvocationFinal *final=effects->invocations->final;EffectBodyMaterial *material=final->material;
    EffectCaptureRead read={.work=&context,.final=final,.body=effect_body_owned_request(&context,material)};
    memset(functions,0xDC,sizeof(functions));memset(ops,0xDC,sizeof(ops));memset(nodes,0xDC,sizeof(nodes));
    size_t allocated=attempts;uint64_t physical=live_bytes;bool scalar=false;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && scalar);
    CHECK(effect_capture_match(&context,final)==XR_XIR_OK);
    EffectBodyFunction *head=&material->functions[1];XrXirType *formal=(XrXirType *)head->parameters;
    XrXirType saved_type=formal[0];formal[0]=(XrXirType)256;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);formal[0]=(XrXirType)4;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);formal[0]=saved_type;
    XrXirType result=head->result;head->result=(XrXirType)257;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);head->result=result;
    EffectBodyRun *first=&head->runs[0];CHECK(first->count==2);
    uint32_t mask=first->mask,opcode_delta=first->delta[0],type_delta=first->delta[1];
    first->mask|=UINT32_C(2);first->delta[1]=256u-(uint32_t)first->base.type;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);
    first->mask=mask;first->delta[1]=type_delta;
    first->mask|=UINT32_C(1);first->delta[0]=(uint32_t)XR_XIR_FUNCTION_REF-(uint32_t)first->base.op;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && !scalar);
    first->mask=mask;first->delta[0]=opcode_delta;
    XrXirOp saved_opcode=first->base.op;uint32_t argument_delta=first->delta[2];
    first->base.op=XR_XIR_LOCAL_WRITE;first->mask|=UINT32_C(4);first->delta[2]=UINT32_MAX;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_BAD_STRUCTURE && !scalar);
    first->base.op=saved_opcode;first->mask=mask;first->delta[2]=argument_delta;
    first->base.op=XR_XIR_OP_COUNT;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_BAD_STRUCTURE && !scalar);first->base.op=saved_opcode;
    EffectInvocationNode *root=&final->nodes[final->roots[1]];CHECK(root->input && !root->input[0]);
    root->input[0]=1;CHECK(effect_capture_match(&context,final)==XR_XIR_BAD_STRUCTURE);root->input[0]=0;
    CHECK(effect_capture_scalar_domain(&read,&scalar)==XR_XIR_OK && scalar);
    CHECK(effect_capture_match(&context,final)==XR_XIR_OK && attempts==allocated && live_bytes==physical);
    pc_allocate_hook=NULL;pc_free_hook=NULL;xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    puts("primitive capture domain: unused Cell/callable types, live formal and affine type/opcode changes, invalid opcode, false scalar input, dead producer and physical0");
}
int main(void) {
    selector_scalar_domain();
    EdgeFixture fixture;pc_empty_fixture(&fixture,4);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectBodyMaterial *material=NULL;CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    EffectBodyRequest body=effect_body_owned_request(&context,material);
    EffectBodyFunction *head=&material->functions[1];EffectBodyFunction saved=*head;
    CHECK(head->instruction_count==1 && head->run_count && head->run_capacity && head->runs);
    size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=selector_no_callback;pc_free_hook=selector_no_callback;
    for(unsigned mode=0;mode<3;++mode) {
        if(mode==1) { head->instruction_count=0;head->run_count=0; }
        if(mode==2) *head=saved;
        EffectFinalInputSelect selected;XrXirInstruction op;uint32_t ordinal=UINT32_MAX;bool found=true;
        CHECK(effect_final_input_select_begin(&body,EFFECT_FINAL_SELECT_REFERENCE,&selected)==XR_XIR_OK);
        XrXirStatus actual=effect_final_input_select_next(&body,1,&selected,&ordinal,&op,&found);
        if(mode==1) {
            EffectBodyRecordCursor old;CHECK(effect_body_request_sequence_begin(&body,&old)==XR_XIR_OK);
            CHECK(effect_body_request_sequence_finish(&body,1,&old)==XR_XIR_BAD_STRUCTURE);
            fprintf(stderr,"actual zero instruction/nonzero owned storage selector status%u expected%u\n",actual,XR_XIR_BAD_STRUCTURE);
            CHECK(actual==XR_XIR_BAD_STRUCTURE);
        } else CHECK(actual==XR_XIR_OK && !found);
    }
    memset(&fixture,0xDC,sizeof(fixture));EffectFinalInputSelect selected;XrXirInstruction op;uint32_t ordinal=0;bool found=true;
    CHECK(effect_final_input_select_begin(&body,EFFECT_FINAL_SELECT_REFERENCE,&selected)==XR_XIR_OK);
    CHECK(effect_final_input_select_next(&body,1,&selected,&ordinal,&op,&found)==XR_XIR_OK && !found);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==allocated && physical==live_bytes);
    effect_body_material_free(material);pc_release(&context,baseline);
    puts("actual opcode selector: zero instruction/storage inconsistency rejected; restoration, producer death, no callbacks and physical0");return 0;
}
