/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_wire.c - Independent constant wire admission and physical fault replay
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_library_compile_owner.h"
#include "wire80_golden.h"
#include "retired29_slot.h"
#include "retired30_constants.h"
static XrXirStatus constant_wire_operation(const XrXirCompileContext *context,void *unused) {
    (void)unused;XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_checked_read(context,constant_wire,sizeof(constant_wire),&artifact,NULL);
    if (status==XR_XIR_OK) {
        status=xr_xir_compile_checked_write(artifact,&packet,NULL);
        if (status==XR_XIR_OK) CHECK(packet.length==sizeof(constant_wire) && !memcmp(packet.bytes,constant_wire,packet.length));
    }
    xr_xir_compile_artifact_free(artifact);xr_xir_compile_checked_packet_free(&packet);return status;
}
static void constant_word(uint8_t *bytes,uint32_t value) {
    for (uint32_t b=0;b<4;++b) bytes[b]=(uint8_t)(value>>(8*b));
}
static void constant_attacks(const XrXirCompileContext *context) {
    const uint32_t fields[][2]={
        {CONSTANT_SLOT+8,1},{CONSTANT_SLOT+12,2},{CONSTANT_SLOT+16,2},
        {CONSTANT_SLOT+20+16,1},{CONSTANT_SLOT+20+12,1},{CONSTANT_LITERAL+4,0},
        {CONSTANT_SLOT+12,0},{CONSTANT_READ+24,1},{CONSTANT_READ+4,XR_XIR_BOOL},
        {CONSTANT_DECL+20+19+12,1}};
    for (uint32_t a=0;a<sizeof(fields)/sizeof(fields[0]);++a) {
        uint8_t bytes[sizeof(constant_wire)];memcpy(bytes,constant_wire,sizeof(bytes));
        constant_word(bytes+fields[a][0],fields[a][1]);
        XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,bytes,32);
        xr_sha256_update(&hash,bytes+64,sizeof(bytes)-64);xr_sha256_final(&hash,bytes+32);
        XrXirArtifact *output=NULL;
        XrXirStatus status=xr_xir_compile_checked_read(context,bytes,sizeof(bytes),&output,NULL);
        if (status==XR_XIR_OK) fprintf(stderr,"unexpected accepted constant attack %u\n",a);
        CHECK(status!=XR_XIR_OK && !output);
    }
}
static void constant_program_match(const XrXirCompileContext *context) {
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,constant_wire,sizeof(constant_wire),&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&entries)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirDeclarations declarations=*module->declarations;
    CHECK(declarations.slot_count==2);
    XrXirSlot slots[2];memcpy(slots,declarations.slots,sizeof(slots));declarations.slots=slots;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,
        &declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    const XrXirFunctionLayout *layouts=xr_xir_compile_artifact_layout(lowered,0);
    CHECK(xr_xir_compile_program_match(context,&spec,layouts,lowered)==XR_XIR_OK);
    /* This remains a valid unique public name, but differs from the proof. */
    slots[0].name=1;
    CHECK(xr_xir_compile_program_match(context,&spec,layouts,lowered)==XR_XIR_BAD_STRUCTURE);
    slots[0].name=0;slots[1].exported=1;slots[1].name=1;
    CHECK(xr_xir_compile_program_match(context,&spec,layouts,lowered)==XR_XIR_BAD_STRUCTURE);
    memcpy(slots,module->declarations->slots,sizeof(slots));spec.abi_version=32;
    XrXirProgram *old=NULL;
    CHECK(xr_xir_compile_program_seal(context,&spec,&old)!=XR_XIR_OK && !old);
    xr_compile_resources_free(bindings);xr_compile_resources_free(entries);
    xr_xir_compile_artifact_free(lowered);
}
int main(void) {
    library_compile_operation_cases("Owned constant packet read/write",constant_wire_operation,NULL);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrCompileResourceStats before=library_compile_stats(&owner.context);size_t attempts=source_program_compile_attempts;
    XrXirArtifact *old=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,retired30_constants,sizeof(retired30_constants),&old,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!old && source_program_compile_attempts==attempts);
    CHECK(xr_xir_compile_checked_read(&owner.context,retired29_slot,sizeof(retired29_slot),&old,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!old && source_program_compile_attempts==attempts);
    XrCompileResourceStats after=library_compile_stats(&owner.context);
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes);
    constant_attacks(&owner.context);constant_program_match(&owner.context);
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts("Independent constant wire, rehashed visibility attacks, exact budgets and physical FI PASS");return 0;
}
