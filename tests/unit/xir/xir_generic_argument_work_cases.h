/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_argument_work_cases.h - Owned actual call spans and structural refusal
 */
#ifndef XIR_GENERIC_ARGUMENT_WORK_CASES_H
#define XIR_GENERIC_ARGUMENT_WORK_CASES_H
/* This is a real unpublished Raw generic producer with two call spans. The
 * last legal span ends exactly at its owned-lifetime argument table boundary.
 * The construction helper creates and releases authentic receiving facts. */
static void generic_argument_span_cases(void) {
    XrXirType parameter=XR_XIR_TYPE_PARAMETER_BASE,arguments[2]={XR_XIR_I64,XR_XIR_I64};
    XrXirType caller_parameter=XR_XIR_I64;XrXirConstraint sendable={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    XrXirInstruction calls[3]={
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=1,.type_arguments={0,1}},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={1,1},.immediate=1,.type_arguments={1,1}},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction body[2]={{.op=XR_XIR_COPY,.type=parameter,.args={0,0}},
        {.op=XR_XIR_RETURN,.args={1,0}}};
    uint32_t operands[2]={0,1};XrXirBlock blocks[2]={{.count=3},{.count=2}};
    XrXirFunction functions[2]={
        {.name="caller",.name_length=6,.parameters=&caller_parameter,.parameter_count=1,.result=XR_XIR_I64,
         .blocks=&blocks[0],.block_count=1,.instructions=calls,.instruction_count=3,.operands=operands,.operand_count=2,.error_type=XR_XIR_NEVER},
        {.name="identity",.name_length=8,.parameters=&parameter,.parameter_count=1,.result=parameter,
         .blocks=&blocks[1],.block_count=1,.instructions=body,.instruction_count=2,.error_type=XR_XIR_NEVER}};
    XrXirGeneric generics[2]={{.arguments=arguments,.argument_count=2},
        {.constraints=&sendable,.parameter_count=1}};
    XrXirModule raw={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .generics=generics,.linkage_kind=XR_XIR_PROGRAM};
    XrCompileResourceLimits caps={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext measured=consumer_context_ephemeral(caps);
    XrCompileResourceStats baseline=consumer_context_stats(&measured);XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_verify(&measured,&raw,&diagnostic)==XR_XIR_OK);
    XrCompileResourceStats legal=consumer_context_stats(&measured);
    CHECK(legal.held_bytes==baseline.held_bytes && legal.peak_bytes>=baseline.held_bytes);
    consumer_context_ephemeral_free(&measured,baseline);
    caps=(XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,legal.peak_bytes,true};
    const uint32_t invalid_counts[2]={UINT32_C(0xff000000),UINT32_C(120000000)};
    for (unsigned mode=0;mode<3;++mode) {
        XrXirCompileContext context=consumer_context_ephemeral(caps);
        XrCompileResourceStats initial=consumer_context_stats(&context);
        if (mode<2) calls[1].type_arguments[1]=invalid_counts[mode];
        else calls[1].type_arguments[0]=UINT32_C(0xff000000);
        diagnostic=(XrXirDiagnostic){0};
        CHECK(xir_fixture_verify(&context,&raw,&diagnostic)==XR_XIR_BAD_STRUCTURE);
        CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE && diagnostic.function==0 &&
            diagnostic.block==0 && diagnostic.instruction==1);
        /* A hostile count must reject structurally within the legal proof's
         * measured held-memory boundary, before allocating attacker-sized data. */
        CHECK(consumer_context_stats(&context).held_bytes==initial.held_bytes);
        calls[1].type_arguments[0]=1;calls[1].type_arguments[1]=1;
        CHECK(xir_fixture_verify(&context,&raw,NULL)==XR_XIR_OK);
        consumer_context_ephemeral_free(&context,initial);
    }
    puts("Raw generic bad spans reject structurally within legal held memory; lastspan/restored neighbor physical0");
}
#endif // XIR_GENERIC_ARGUMENT_WORK_CASES_H
