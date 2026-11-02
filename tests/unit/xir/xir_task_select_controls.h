/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Fresh BUILT functions repeat the actual Channel-to-carrier relationship. */
static void scope_select_controls(const XrXirCompileContext *context) {
    const char *text="fn run(){const ch=Channel<i64>(1);select{v from ch->{} _->{}}}\n"
        "fn other(){const ch=Channel<string>(1);select{v from ch->{} _->{}}}";
    XrXirSourceResult source={0};CHECK(check(context,text,&source,NULL)==XR_XIR_OK);
    const XrXirModule *original=xr_xir_compile_artifact_module(source.checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source.checked);
    uint32_t run=UINT32_MAX,select=UINT32_MAX,integer=UINT32_MAX;XrXirType foreign=XR_XIR_UNIT;
    for(uint32_t f=0;f<original->function_count;++f) {
        const XrXirFunction *function=original->functions+f;
        bool wanted=function->name_length==3&&!memcmp(function->name,"run",3);
        if(wanted)run=f;
        for(uint32_t i=0;i<function->instruction_count;++i) {
            const XrXirInstruction *op=function->instructions+i;
            if(op->op==XR_XIR_CHANNEL_SELECT){if(wanted)select=i;else foreign=op->type;}
            if(wanted&&op->op==XR_XIR_CONST_INT)integer=function->parameter_count+i;
        }
    }
    CHECK(run!=UINT32_MAX&&select!=UINT32_MAX&&integer!=UINT32_MAX&&foreign!=XR_XIR_UNIT);
    const XrXirFunction *actual=original->functions+run;
    XrXirFunction *functions=xr_malloc((size_t)original->function_count*sizeof(*functions));CHECK(functions);
    XrXirInstruction *ops=xr_malloc((size_t)actual->instruction_count*sizeof(*ops));CHECK(ops);
    uint32_t *operands=xr_malloc(((size_t)actual->operand_count+1)*sizeof(*operands));CHECK(operands);
    memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
    XrXirModule built=*original;built.stage=XR_XIR_BUILT;built.provenance=NULL;built.functions=functions;
    for(unsigned mode=0;mode<6;++mode) {
        functions[run]=*actual;memcpy(ops,actual->instructions,(size_t)actual->instruction_count*sizeof(*ops));
        memcpy(operands,actual->operands,(size_t)actual->operand_count*sizeof(*operands));
        functions[run].instructions=ops;functions[run].operands=operands;
        if(mode==1)ops[select].immediate=3;
        if(mode==2)ops[select].immediate=2;
        if(mode==3)ops[select].type=foreign;
        if(mode==4)operands[ops[select].args[0]]=integer;
        if(mode==5) {
            CHECK(ops[select].args[0]+ops[select].args[1]==actual->operand_count);
            operands[actual->operand_count]=operands[ops[select].args[0]];
            ++functions[run].operand_count;++ops[select].args[1];ops[select].immediate=2;
        }
        XrXirStatus expected=mode==0?XR_XIR_OK:mode<=2?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        uint64_t baseline=held(context->resources);XrXirArtifact *output=NULL;XrXirDiagnostic issue={0};
        XrXirStatus status=xr_xir_compile_check_v2(context,&built,construction,&output,&issue);
        if(status!=expected)fprintf(stderr,"select control %u actual%u expected%u f%u b%u i%u\n",
            mode,status,expected,issue.function,issue.block,issue.instruction);
        CHECK(status==expected&&!!output==(mode==0));if(mode)CHECK(issue.function==run);
        xr_xir_compile_artifact_free(output);CHECK(held(context->resources)==baseline);
    }
    xr_free(operands);xr_free(ops);xr_free(functions);xr_xir_compile_source_result_free(&source);
}
