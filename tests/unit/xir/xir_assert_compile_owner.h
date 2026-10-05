/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_compile_owner.h - Bounded assertion compiler fixture ownership
 */
#ifndef XIR_ASSERT_COMPILE_OWNER_H
#define XIR_ASSERT_COMPILE_OWNER_H
#include "xir_library_compile_owner.h"
#include "xir/xxir_generic.h"
static LibraryCompileOwner assert_compiler;
static const XrXirCompileContext *assert_compile_context;
static size_t assert_compile_baseline_blocks,assert_compile_baseline_bytes;
static inline size_t assert_compile_extra_blocks(void){CHECK(source_program_compile_live>=assert_compile_baseline_blocks);return source_program_compile_live-assert_compile_baseline_blocks;}
static inline size_t assert_compile_extra_bytes(void){CHECK(source_program_compile_bytes>=assert_compile_baseline_bytes);return source_program_compile_bytes-assert_compile_baseline_bytes;}
static void assert_compile_begin(void) {
    CHECK(library_compile_owner_new(&assert_compiler,&library_compile_limits)==XR_XIR_OK);
    assert_compile_context=&assert_compiler.context;
    assert_compile_baseline_blocks=source_program_compile_live;assert_compile_baseline_bytes=source_program_compile_bytes;
}
static void assert_compile_end(void) {
    library_compile_owner_drop(&assert_compiler);library_compile_observer_free();assert_compile_context=NULL;
}
typedef enum AssertCompileStage {ASSERT_COMPILE_READER,ASSERT_COMPILE_WRITER,ASSERT_COMPILE_SPECIALIZE,ASSERT_COMPILE_LOWER,ASSERT_COMPILE_TAKE} AssertCompileStage;
typedef struct AssertCompileFixture {const void *bytes;size_t length;AssertCompileStage stage;} AssertCompileFixture;
static inline XrXirStatus assert_compile_stage_operation(const XrXirCompileContext *context,void *opaque) {
    const AssertCompileFixture *fixture=opaque;XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_checked_read(context,fixture->bytes,fixture->length,&read,NULL);
    if(status!=XR_XIR_OK){CHECK(!read);goto done;}
    if(fixture->stage==ASSERT_COMPILE_READER)goto done;
    if(fixture->stage==ASSERT_COMPILE_WRITER){
        status=xr_xir_compile_checked_write(read,&packet,NULL);
        if(status==XR_XIR_OK)CHECK(packet.length==fixture->length&&!memcmp(packet.bytes,fixture->bytes,packet.length));else CHECK(!packet.bytes&&!packet.length);
        goto done;
    }
    status=xr_xir_compile_specialize(read,&closed,NULL);
    if(status!=XR_XIR_OK){CHECK(!closed);goto done;}
    if(fixture->stage==ASSERT_COMPILE_SPECIALIZE)goto done;
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    if(status!=XR_XIR_OK){CHECK(!lowered);goto done;}
    if(fixture->stage==ASSERT_COMPILE_TAKE){XrXirArtifact *original=lowered;status=xr_xir_compile_vm_program_take(&lowered,&program);
        if(status==XR_XIR_OK)CHECK(!lowered&&program);else CHECK(!program&&lowered==original);
    }
 done:
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_program_drop(program);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(read);return status;
}
static inline void assert_compile_stage_cases(const XrXirArtifact *checked,AssertCompileStage stage,const char *name) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    AssertCompileFixture fixture={packet.bytes,packet.length,stage};library_compile_operation_cases(name,assert_compile_stage_operation,&fixture);
    xr_xir_compile_checked_packet_free(&packet);
}
#endif
