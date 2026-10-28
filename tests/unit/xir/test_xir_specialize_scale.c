/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_specialize_scale.c - Complete reachable generic closure scale
 *
 * KEY CONCEPT:
 *   Genuine Built/Checked modules discover each generic declaration twice.
 *   Literal call ordinals define expected identities, independent of lookup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); exit(1); } } while (0)
#include "xir/xxir.h"
#include "xir_library_compile_owner.h"
#include "xir_construction_fixture.h"
#include "xir/xxir_generic.h"

enum { SCALE_MAX = 512 };
typedef struct ScaleFixture {
    XrXirFunction functions[SCALE_MAX+1];
    XrXirGeneric generics[SCALE_MAX+1];
    XrXirInstruction calls[SCALE_MAX*2+2],body[2];
    XrXirBlock entry_block,body_block;
    XrXirType arguments[SCALE_MAX*2];
    XrXirConstraint constraint;
    XrXirModule module;
} ScaleFixture;
static void scale_fixture(ScaleFixture *f,uint32_t n) {
    CHECK(n && n<=SCALE_MAX);memset(f,0,sizeof(*f));
    for (uint32_t i=0;i<n*2;++i) f->arguments[i]=XR_XIR_I64;
    f->body[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->body[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}};
    f->body_block=(XrXirBlock){0,2,0,0};f->entry_block=(XrXirBlock){0,n*2+2,0,0};
    for (uint32_t i=0;i<n*2;++i)
        f->calls[i]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1+i%n,.type_arguments={i,1}};
    f->calls[n*2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->calls[n*2+1]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={n*2}};
    f->functions[0]=(XrXirFunction){.name="run",.name_length=3,.result=XR_XIR_I64,.blocks=&f->entry_block,
        .block_count=1,.instructions=f->calls,.instruction_count=n*2+2};
    f->generics[0]=(XrXirGeneric){.arguments=f->arguments,.argument_count=n*2};
    for (uint32_t i=1;i<=n;++i) {
        f->functions[i]=(XrXirFunction){.name="stamp",.name_length=5,.result=XR_XIR_I64,.blocks=&f->body_block,
            .block_count=1,.instructions=f->body,.instruction_count=2};
        f->generics[i]=(XrXirGeneric){.constraints=&f->constraint,.parameter_count=1};
    }
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=n+1,
        .generics=f->generics,.linkage_kind=XR_XIR_PROGRAM};
}
static void scale_operation(uint32_t n) {
    ScaleFixture fixture;scale_fixture(&fixture,n);LibraryCompileOwner owner={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrCompileResourceStats start=library_compile_stats(&owner.context);
    XrXirArtifact *checked=NULL,*read=NULL,*closed=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&owner.context,&fixture.module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"scale check status%u f%u b%u i%u\n",(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked,&packet,&diagnostic)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&read,&diagnostic)==XR_XIR_OK);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrCompileResourceStats before=library_compile_stats(&owner.context);
    status=xr_xir_compile_specialize(read,&closed,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"scale specialize status%u f%u b%u i%u\n",(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    XrCompileResourceStats after=library_compile_stats(&owner.context);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);const XrXirProvenance *proof=module->provenance;
    CHECK(module->function_count==n+1 && proof && proof->count==n+1 && !module->generics);
    for (uint32_t i=0;i<n*2;++i) CHECK(module->functions[0].instructions[i].immediate==1+i%n);
    CHECK(module->functions[0].instructions[n*2].immediate==42);
    for (uint32_t i=1;i<=n;++i) CHECK(proof->origins[i].function==i && proof->origins[i].argument_count==1 &&
        proof->origins[i].arguments[0]==XR_XIR_I64 && !proof->origins[i].effect_argument_count);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(closed,&diagnostic)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower_take(&closed,&target, NULL,&lowered,&diagnostic)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(lowered,&diagnostic)==XR_XIR_OK);xr_xir_compile_artifact_free(lowered);
    XrCompileResourceStats final=library_compile_stats(&owner.context);CHECK(final.live_bytes==start.live_bytes);
    printf("SPEC_CLOSURE_SCALE N%u keyElements%u functions%u rows%u calls%u specializeWork%llu specializeAllocated%llu specializeSites%llu wholeWork%llu wholeAllocated%llu wholeSites%llu live%llu peak%llu\n",
        n,n*5+4,n+1,n*4+2,n*2,(unsigned long long)(after.work-before.work),
        (unsigned long long)(after.allocated_bytes-before.allocated_bytes),(unsigned long long)(after.allocation_count-before.allocation_count),
        (unsigned long long)(final.work-start.work),(unsigned long long)(final.allocated_bytes-start.allocated_bytes),
        (unsigned long long)(final.allocation_count-start.allocation_count),(unsigned long long)final.live_bytes,(unsigned long long)final.peak_bytes);
    library_compile_owner_drop(&owner);
}
int main(void) {
    for (uint32_t n=64;n<=256;n*=2) scale_operation(n);
    library_compile_observer_free();return 0;
}
