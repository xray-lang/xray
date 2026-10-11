/* Complete private copies; these nonzero unused fields are not raw admission. */
#ifndef XIR_ORDINARY_INSTRUCTION_COPY_CASES_H
#define XIR_ORDINARY_INSTRUCTION_COPY_CASES_H

static XrXirInstruction ordinary_copy_literal(uint32_t index,XrXirType type) {
    return (XrXirInstruction){.op=XR_XIR_COPY,.type=type,
        .args={17+index,31+index},.targets={47+index,61+index},
        .immediate=INT64_C(0x1234567800000000)+index,.type_arguments={79+index,97+index}};
}

/* This component does not publish an Artifact or grant any execution rights.
 * Field poison deliberately distinguishes a full copy from valid raw IR. */
static XrXirStatus conditional_ordinary_instruction_copy(const XrXirCompileContext *context,uint32_t mode) {
    uint32_t count=mode==2?0:mode==0?17:3;bool generic=mode==1 || mode==3;
    XrXirType input=generic?(XrXirType)XR_XIR_TYPE_PARAMETER_BASE:XR_XIR_I64;
    XrXirType parameters[2]={input,XR_XIR_I64},argument=XR_XIR_I64;
    XrXirInstruction ops[17];char name[520];uint32_t operands[3]={11,23,37};
    XrXirBlock block={.count=count};
    for (uint32_t i=0;i<count;++i) ops[i]=ordinary_copy_literal(i,input);
    if (mode==3) ops[count-1].type=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    for (size_t i=0;i<sizeof(name);++i) name[i]=(char)('A'+i%23);
    XrXirFunction source={.name=name,.name_length=(uint32_t)sizeof(name),
        .parameters=parameters,.parameter_count=2,.result=input,.blocks=&block,.block_count=1,
        .instructions=count?ops:NULL,.instruction_count=count,.operands=operands,.operand_count=3};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&source,.function_count=1};
    XrXirFunction output={.name="untouched",.name_length=9,.parameters=&argument,.parameter_count=1,
        .result=XR_XIR_BOOL,.blocks=&block,.block_count=1,.instructions=ops,.instruction_count=99,
        .operands=operands,.operand_count=1};
    EffectOrdinaryNode node={.declaration=0,.arguments=generic?&argument:NULL,
        .argument_count=generic?1:0,.source_parameters=&argument};
    EffectOrdinaryContexts owned={.terms={.remaining=context},.nodes=&node,.functions=&output,
        .count=1,.capacity=1,.base_count=generic?0:1};
    XrXirStatus status=effect_ordinary_function(&owned,&module,0);
    if (status==XR_XIR_OK) {
        CHECK(mode!=3 && output.name!=name && output.parameters!=parameters && output.operands!=operands);
        CHECK(output.name_length==sizeof(name) && output.parameter_count==2 && output.result==XR_XIR_I64 &&
            !output.blocks && !output.block_count && output.instruction_count==count && output.operand_count==3);
        CHECK(node.source_parameters==output.parameters && output.parameters[0]==XR_XIR_I64 &&
            output.parameters[1]==XR_XIR_I64 && output.operands[0]==11 && output.operands[1]==23 && output.operands[2]==37);
        CHECK(count ? output.instructions && output.instructions!=ops : !output.instructions);
        memset(ops,0xCE,sizeof(ops));memset(name,0xCE,sizeof(name));
        memset(parameters,0xCE,sizeof(parameters));memset(operands,0xCE,sizeof(operands));
        for (size_t i=0;i<sizeof(name);++i) CHECK(output.name[i]==(char)('A'+i%23));
        for (uint32_t i=0;i<count;++i) {
            XrXirInstruction expected=ordinary_copy_literal(i,XR_XIR_I64),actual=output.instructions[i];
            CHECK(actual.op==expected.op && actual.type==expected.type && actual.immediate==expected.immediate &&
                actual.args[0]==expected.args[0] && actual.args[1]==expected.args[1] &&
                actual.targets[0]==expected.targets[0] && actual.targets[1]==expected.targets[1] &&
                actual.type_arguments[0]==expected.type_arguments[0] && actual.type_arguments[1]==expected.type_arguments[1]);
        }
    } else {
        CHECK(output.name_length==9 && !strcmp(output.name,"untouched") && output.parameters==&argument &&
            output.parameter_count==1 && output.result==XR_XIR_BOOL && output.blocks==&block && output.block_count==1 &&
            output.instructions==ops && output.instruction_count==99 && output.operands==operands && output.operand_count==1);
        CHECK(node.source_parameters==&argument);
    }
    effect_terms_free(&owned.terms);return status;
}

static void conditional_ordinary_instruction_resources(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        size_t sites=0;XrCompileResourceStats census={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
            XrCompileResources *identity=context.resources;RootParameterMark retained=rp_mark();
            size_t first=rp_attempts;rp_fail_at=pass?first+pass-1:SIZE_MAX;
            XrXirStatus status=conditional_ordinary_instruction_copy(&context,mode);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-first;census=rp_stats(&context);CHECK(sites);
                printf("ORDINARY_INSTRUCTION_COPY mode=%u sites=%zu allocated=%llu live=%llu peak=%llu work=%llu\n",
                    mode,sites,(unsigned long long)census.allocated_bytes,(unsigned long long)census.live_bytes,
                    (unsigned long long)census.peak_bytes,(unsigned long long)census.work);
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-first==pass);
                CHECK(context.resources==identity && rp_stats(&context).live_bytes==baseline);rp_balanced(retained);
                rp_fail_at=SIZE_MAX;size_t retry=rp_attempts;
                CHECK(conditional_ordinary_instruction_copy(&context,mode)==XR_XIR_OK && context.resources==identity);
                CHECK(rp_attempts-retry==sites);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(physical);
        }
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();uint64_t limit=measured[axis]-minus;
            if (axis==0) limits.allocated_bytes=limit;
            else if (axis==1) limits.live_bytes=limit;
            else limits.work=limit;
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(limits);
            uint64_t baseline=rp_stats(&context).live_bytes;
            CHECK(conditional_ordinary_instruction_copy(&context,mode)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(conditional_ordinary_instruction_copy(&context,3)==XR_XIR_BAD_TYPE);
    CHECK(conditional_ordinary_instruction_copy(&context,1)==XR_XIR_OK);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}
#endif
