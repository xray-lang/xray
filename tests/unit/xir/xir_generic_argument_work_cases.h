/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_argument_work_cases.h - Bounded actual call span before work
 */
#ifndef XIR_GENERIC_ARGUMENT_WORK_CASES_H
#define XIR_GENERIC_ARGUMENT_WORK_CASES_H
/* This is a real unpublished Raw generic producer with two call spans. The
 * last legal span ends exactly at its owned-lifetime argument table boundary.
 * The construction helper creates and releases authentic receiving facts. */
static void generic_argument_work_cases(void) {
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
         .blocks=&blocks[0],.block_count=1,.instructions=calls,.instruction_count=3,.operands=operands,.operand_count=2},
        {.name="identity",.name_length=8,.parameters=&parameter,.parameter_count=1,.result=parameter,
         .blocks=&blocks[1],.block_count=1,.instructions=body,.instruction_count=2}};
    XrXirGeneric generics[2]={{.arguments=arguments,.argument_count=2},
        {.constraints=&sendable,.parameter_count=1}};
    XrXirModule raw={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .generics=generics,.linkage_kind=XR_XIR_PROGRAM};
    XrCompileResourceLimits caps={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
    XrXirCompileContext measured=consumer_context_ephemeral(caps);
    XrCompileResourceStats baseline=consumer_context_stats(&measured);XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_verify(&measured,&raw,&diagnostic)==XR_XIR_OK);
    uint64_t legal=consumer_context_stats(&measured).work-baseline.work;
    CHECK(legal && consumer_context_stats(&measured).live_bytes==baseline.live_bytes);
    consumer_context_ephemeral_free(&measured,baseline);
    fprintf(stderr,"generic argument legal-lastspan work=%llu; compare unchanged before/after product fix\n",
        (unsigned long long)legal);
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits exact=caps;exact.work=baseline.work+legal-minus;
        XrXirCompileContext context=consumer_context_ephemeral(exact);
        XrCompileResourceStats initial=consumer_context_stats(&context);
        XrXirStatus status=xir_fixture_verify(&context,&raw,NULL);
        CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(minus?consumer_context_stats(&context).work<=exact.work:
            consumer_context_stats(&context).work==exact.work);
        consumer_context_ephemeral_free(&context,initial);
    }
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
        /* A malformed count cannot debit work proportional to attacker data.
         * The complete legal proof supplies a measured finite control bound. */
        CHECK(consumer_context_stats(&context).work-initial.work<=legal);
        calls[1].type_arguments[0]=1;calls[1].type_arguments[1]=1;
        CHECK(xir_fixture_verify(&context,&raw,NULL)==XR_XIR_OK);
        consumer_context_ephemeral_free(&context,initial);
    }
    puts("Raw generic bad spans reject structurally before unbounded work; legal lastspan/exact-minus1/restored neighbor physical0");
}
#endif // XIR_GENERIC_ARGUMENT_WORK_CASES_H
