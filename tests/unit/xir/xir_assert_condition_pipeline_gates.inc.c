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
    assert_compile_stage_cases(checked,ASSERT_COMPILE_SPECIALIZE,"Assertion specialize/replay");
    assert_compile_stage_cases(checked,ASSERT_COMPILE_LOWER,"Assertion lower/replay");
    assert_compile_stage_cases(checked,ASSERT_COMPILE_TAKE,"Assertion owned VM take/replay");
    size_t baseline=runtime_live,bytes=runtime_bytes;
    XrXirArtifact *closed=NULL;CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    uint32_t owner=assert_find(xr_xir_compile_artifact_module(closed),"assert");
    size_t instruction=assert_packet_instruction(&packet,owner), position=instruction;
    CHECK(assert_packet_u32(&packet,&position)==115);
    /* OUTPUT on the STRING parameter is locally typed, but is not the bound
     * assertion body recorded in the owned checked origin. */
    assert_word(packet.bytes+instruction,XR_XIR_OUTPUT);
    assert_word(packet.bytes+instruction+8,1);assert_word(packet.bytes+instruction+12,0);
    assert_word(packet.bytes+instruction+24,XR_XIR_STDOUT);assert_packet_hash(&packet);
    XrXirArtifact *read=NULL;
    CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_BAD_STRUCTURE && !read);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirInstruction *instructions=(XrXirInstruction *)closed->module.functions[owner].instructions;
    XrXirInstruction saved=instructions[0];
    instructions[0]=(XrXirInstruction){XR_XIR_OUTPUT,XR_XIR_UNIT,{1,0},{0},XR_XIR_STDOUT,{0}};
    XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_BAD_STRUCTURE && !lowered);
    instructions[0]=saved;
    xr_xir_compile_artifact_free(closed);CHECK(runtime_live==baseline && runtime_bytes==bytes);
    puts("Locally typed counterfeit intrinsic body refused by owned provenance in reader/writer/lower PASS");
}
