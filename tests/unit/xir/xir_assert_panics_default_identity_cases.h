/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_default_identity_cases.h - Default metadata remains authenticated
 */
#ifndef XIR_ASSERT_PANICS_DEFAULT_IDENTITY_CASES_H
#define XIR_ASSERT_PANICS_DEFAULT_IDENTITY_CASES_H
static void panics_default_identity_gate(const XrXirArtifact *checked,XrXirCheckedPacket *packet) {
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    uint32_t function=panics_find(module,"typedError"),found=0;
    LibraryCompileOwner receiver={0};CHECK(library_compile_owner_new(&receiver,&library_compile_limits)==XR_XIR_OK);
    uint8_t digest[32];memcpy(digest,packet->bytes+32,sizeof(digest));
    XrXirArtifact *positive=NULL;XrXirDiagnostic positive_diagnostic={0};
    XrXirStatus positive_status=xr_xir_compile_checked_read(&receiver.context,packet->bytes,packet->length,&positive,&positive_diagnostic);
    if (positive_status!=XR_XIR_OK) fprintf(stderr,"default identity positive packet status=%u f=%u b=%u i=%u\n",positive_status,
        positive_diagnostic.function,positive_diagnostic.block,positive_diagnostic.instruction);
    CHECK(positive_status==XR_XIR_OK && positive);xr_xir_compile_artifact_free(positive);
    const XrXirFunction *body=&module->functions[function];
    size_t start=panics_instruction_offset(packet,function);
    size_t compiler_live=source_program_compile_live,compiler_bytes=source_program_compile_bytes;
    size_t live=runtime_live,bytes=runtime_bytes;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        if (op->op!=XR_XIR_INVOKE_DEFAULT) continue;
        ++found;
        size_t opcode=start+(size_t)i*40;CHECK(panics_word_at(packet,&opcode)==(uint32_t)op->op);
        for (uint32_t field=0;field<2;++field) {
            size_t offset=start+(size_t)i*40+8+(size_t)field*4;
            size_t word=offset;CHECK(panics_word_at(packet,&word)==op->args[field]);
            panics_word(packet->bytes+offset,UINT32_MAX);panics_hash(packet);
            for (unsigned occupied=0;occupied<2;++occupied) {
                XrXirArtifact *sentinel=occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL,*output=sentinel;
                XrXirDiagnostic diagnostic={0};
                XrXirStatus status=xr_xir_compile_checked_read(&receiver.context,packet->bytes,packet->length,&output,&diagnostic);
                /* Invalid generic owners fail typing; absent ordinals fail structure. */
                XrXirStatus expected=occupied || field ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE;
                if (status!=expected || output!=sentinel)
                    fprintf(stderr,"default identity field=%u occupied=%u status=%u f=%u b=%u i=%u unchanged=%u\n",field,occupied,status,
                        diagnostic.function,diagnostic.block,diagnostic.instruction,output==sentinel ? 1u : 0u);
                CHECK(status==expected && output==sentinel);
                CHECK(source_program_compile_live==compiler_live && source_program_compile_bytes==compiler_bytes && runtime_live==live && runtime_bytes==bytes);
            }
            panics_word(packet->bytes+offset,op->args[field]);panics_hash(packet);
        }
    }
    CHECK(found && !memcmp(digest,packet->bytes+32,sizeof(digest)));library_compile_owner_drop(&receiver);
    puts("Original typed error callback and authenticated default helper: rehashed foreign owner/ordinal rejected; empty/occupied output and physical baselines preserved");
}
#endif // XIR_ASSERT_PANICS_DEFAULT_IDENTITY_CASES_H
