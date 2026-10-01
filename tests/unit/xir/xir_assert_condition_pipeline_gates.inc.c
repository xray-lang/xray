/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_pipeline_gates.inc.c - Provenance and publication lifetime
 */
static uint32_t assert_packet_u32(const XrXirCheckedPacket *packet,size_t *position) {
    CHECK(*position<=packet->length && packet->length-*position>=4);
    uint32_t value=0;
    for (uint32_t i=0;i<4;++i) value|=(uint32_t)packet->bytes[*position+i]<<(i*8);
    *position+=4;return value;
}
static void assert_packet_skip(const XrXirCheckedPacket *packet,size_t *position,uint64_t bytes) {
    CHECK(*position<=packet->length && bytes<=packet->length-*position);*position+=(size_t)bytes;
}
static size_t assert_packet_instruction(const XrXirCheckedPacket *packet,uint32_t function) {
    size_t position=64;
    CHECK(assert_packet_u32(packet,&position)==XR_XIR_PROGRAM);
    uint32_t count=assert_packet_u32(packet,&position);CHECK(function<count);
    CHECK(assert_packet_u32(packet,&position)==1);
    for (uint32_t f=0;f<=function;++f) {
        uint32_t length=assert_packet_u32(packet,&position);assert_packet_skip(packet,&position,length);
        uint32_t parameters=assert_packet_u32(packet,&position);assert_packet_skip(packet,&position,(uint64_t)parameters*4);
        assert_packet_u32(packet,&position);
        uint32_t blocks=assert_packet_u32(packet,&position);assert_packet_skip(packet,&position,(uint64_t)blocks*16);
        uint32_t instructions=assert_packet_u32(packet,&position);CHECK(instructions);
        if (f==function) return position;
        assert_packet_skip(packet,&position,(uint64_t)instructions*40);
        uint32_t operands=assert_packet_u32(packet,&position);assert_packet_skip(packet,&position,(uint64_t)operands*4);
    }
    CHECK(false);return 0;
}
static void assert_pipeline_oom(const XrXirArtifact *checked) {
    size_t baseline=runtime_live,bytes=runtime_bytes;
    XrXirArtifact *closed=NULL;CHECK(xr_xir_specialize(checked,NULL,&closed,NULL)==XR_XIR_OK);
    size_t kept=runtime_live,kept_bytes=runtime_bytes,sites=0;
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    for (uint32_t group=0;group<2;++group) for (size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirArtifact *output=NULL;
        XrXirStatus status=group ? xr_xir_lower(closed,&target,NULL,&output,NULL) :
            xr_xir_specialize(checked,NULL,&output,NULL);
        if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;}
        else CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_OUT_OF_MEMORY && !output);
        runtime_fail_at=SIZE_MAX;xr_xir_artifact_free(output);
        CHECK(runtime_live==kept && runtime_bytes==kept_bytes);
        if (point==sites) {printf("Assertion %s actual OOM=%zu physical baseline restored\n",group ? "lower" : "specialize",sites);break;}
    }
    sites=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirArtifact *lowered=NULL;CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
        XrXirArtifact *original=lowered;
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirProgram *program=NULL;
        XrXirStatus status=xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){16777216,64000000},&program);
        if (!point) {CHECK(status==XR_XIR_OK && !lowered && program);sites=runtime_attempts;}
        else CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_OUT_OF_MEMORY && !program && lowered==original);
        runtime_fail_at=SIZE_MAX;xr_xir_artifact_free(lowered);xr_xir_program_drop(program);
        CHECK(runtime_live==kept && runtime_bytes==kept_bytes);
        if (point==sites) {printf("Assertion owned VM seal actual OOM=%zu no failed lease transfer\n",sites);break;}
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(closed,NULL,&packet,NULL)==XR_XIR_OK);
    uint32_t owner=assert_find(xr_xir_artifact_module(closed),"assert");
    size_t instruction=assert_packet_instruction(&packet,owner), position=instruction;
    CHECK(assert_packet_u32(&packet,&position)==115);
    /* OUTPUT on the STRING parameter is locally typed, but is not the bound
     * assertion body recorded in the owned checked origin. */
    assert_word(packet.bytes+instruction,XR_XIR_OUTPUT);
    assert_word(packet.bytes+instruction+8,1);assert_word(packet.bytes+instruction+12,0);
    assert_word(packet.bytes+instruction+24,XR_XIR_STDOUT);assert_packet_hash(&packet);
    XrXirArtifact *read=NULL;
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&read,NULL)==XR_XIR_BAD_STRUCTURE && !read);
    xr_xir_checked_packet_free(&packet);
    XrXirInstruction *instructions=(XrXirInstruction *)closed->module.functions[owner].instructions;
    XrXirInstruction saved=instructions[0];
    instructions[0]=(XrXirInstruction){XR_XIR_OUTPUT,XR_XIR_UNIT,{1,0},{0},XR_XIR_STDOUT,{0}};
    XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_checked_write(closed,NULL,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_BAD_STRUCTURE && !lowered);
    instructions[0]=saved;
    xr_xir_artifact_free(closed);CHECK(runtime_live==baseline && runtime_bytes==bytes);
    puts("Locally typed counterfeit intrinsic body refused by owned provenance in reader/writer/lower PASS");
}
