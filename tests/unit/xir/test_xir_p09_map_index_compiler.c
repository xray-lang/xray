/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_index_compiler.c - Whole compiler graph faults and exact resource boundaries
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir/xxir.h"
#include "xir/xxir_source.h"
#include "xir_library_compile_owner.h"
#include "p09_map_index_input.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"
typedef struct MembersCompilerInput { const char *root, *path, *stdlib; } MembersCompilerInput;

static XrXirStatus members_compile(const XrXirCompileContext *context, const MembersCompilerInput *input) {
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK)
        return created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,input->root};
    XrXirSourceRequest request={session,input->path,&authority,context,input->stdlib,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0}; XrXirSourceFailure failure={0};
    XrXirCheckedPacket packet={0}; XrXirArtifact *read=NULL,*specialized=NULL,*lowered=NULL;
    XrXirCSource source={0}; XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    xr_compile_session_free(session); session=NULL;
    if (status!=XR_XIR_OK) { CHECK(!result.checked && !result.snapshot); goto done; }
    CHECK(result.checked && result.snapshot && xr_xir_compile_source_failure_empty(&failure));
    CHECK(xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
    status=xr_xir_compile_checked_write(result.checked,&packet,NULL);
    if (status!=XR_XIR_OK) { CHECK(!packet.bytes && !packet.length); goto done; }
    xr_xir_compile_source_result_free(&result);
    status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    if (status!=XR_XIR_OK) { CHECK(!read); goto done; }
    memset(packet.bytes,0xa5,packet.length); xr_xir_compile_checked_packet_free(&packet);
    status=xr_xir_compile_specialize(read,&specialized,NULL);
    if (status!=XR_XIR_OK) { CHECK(!specialized); goto done; }
    xr_xir_compile_artifact_free(read); read=NULL;
    status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL);
    if (status!=XR_XIR_OK) { CHECK(!lowered); goto done; }
    xr_xir_compile_artifact_free(specialized); specialized=NULL;
    status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status!=XR_XIR_OK) goto done;
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered); uint32_t entries[INDEX_COUNT];
    index_entries(module,entries); uint32_t operations=0;
    for (uint32_t f=0;f<module->function_count;++f)
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
            XrXirOp op=module->functions[f].instructions[i].op;
            if (op>=XR_XIR_MAP_NEW && op<=XR_XIR_MAP_INDEX) operations|=1u<<(op-XR_XIR_MAP_NEW);
        }
    CHECK(operations==545 && entries[0]<module->function_count);
    status=xr_xir_compile_emit_c(lowered,"map_index_compiler",1048576,&source);
    if (status!=XR_XIR_OK) { CHECK(!source.text && !source.length); goto done; }
    CHECK(source.text && source.length && !strstr(source.text,"({"));
    xr_xir_compile_c_source_free(&source);
    XrXirArtifact *saved=lowered;
    status=xr_xir_compile_vm_program_take(&lowered,&program);
    if (status==XR_XIR_OK) CHECK(program && !lowered);
    else CHECK(!program && lowered==saved);
done:
    xr_xir_compile_program_drop(program); xr_xir_compile_c_source_free(&source);
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(specialized);
    xr_xir_compile_artifact_free(read); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_source_result_free(&result); xr_xir_compile_source_failure_free(&failure);
    return status;
}
static XrCompileResourceStats members_baseline(const MembersCompilerInput *input, size_t *sites) {
    LibraryCompileOwner owner={0}; CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;
    XrXirStatus first_status=members_compile(&owner.context,input);
    if (first_status!=XR_XIR_OK) {
        XrCompileResourceStats failed=library_compile_stats(&owner.context);
        fprintf(stderr,"Source compiler first pass status=%u work=%llu allocated=%llu live=%llu\n",first_status,
            (unsigned long long)failed.work,(unsigned long long)failed.allocated_bytes,(unsigned long long)failed.live_bytes);
    }
    CHECK(first_status==XR_XIR_OK); *sites=source_program_compile_attempts;
    XrCompileResourceStats required=library_compile_stats(&owner.context);
    CHECK(*sites && required.live_bytes==owner.baseline.live_bytes);
    CHECK(members_compile(&owner.context,input)==XR_XIR_OK);
    XrCompileResourceStats retried=library_compile_stats(&owner.context);
    CHECK(retried.allocated_bytes>required.allocated_bytes && retried.work>required.work);
    library_compile_owner_drop(&owner);
    printf("Map index whole compiler sites=%zu allocated=%llu peak=%llu work=%llu same owner repeat PASS\n",*sites,
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
    return required;
}
static void members_compiler_faults(const MembersCompilerInput *input, size_t sites) {
    for (size_t site=0;site<sites;++site) {
        LibraryCompileOwner owner={0}; CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        const size_t live=source_program_compile_live, bytes=source_program_compile_bytes;
        source_program_compile_attempts=0; source_program_compile_injected=false; source_program_compile_fail_at=site;
        XrXirStatus status=members_compile(&owner.context,input); source_program_compile_fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Map index whole compiler FI %zu/%zu status=%u\n",site,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected && source_program_compile_attempts>site);
        CHECK(source_program_compile_live==live && source_program_compile_bytes==bytes);
        XrCompileResourceStats paid=library_compile_stats(&owner.context);
        CHECK(paid.live_bytes==owner.baseline.live_bytes);
        CHECK(members_compile(&owner.context,input)==XR_XIR_OK);
        XrCompileResourceStats retried=library_compile_stats(&owner.context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes && retried.work>paid.work);
        library_compile_owner_drop(&owner);
    }
    printf("Map index whole compiler actual OOM sites=%zu same owner retry and physical refund PASS\n",sites);
}
static void members_compiler_limits(const MembersCompilerInput *input, XrCompileResourceStats required) {
    for (unsigned axis=0;axis<3;++axis) for (unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=library_compile_limits;
        uint64_t amount=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        CHECK(amount);
        if (!axis) limits.allocated_bytes=amount-less;
        else if (axis==1) limits.live_bytes=amount-less;
        else limits.work=amount-less;
        LibraryCompileOwner owner={0}; XrXirStatus status=library_compile_owner_new(&owner,&limits);
        if (status==XR_XIR_OK) status=members_compile(&owner.context,input);
        if (status!=(less?XR_XIR_BUDGET:XR_XIR_OK))
            fprintf(stderr,"Map index compiler axis=%u less=%u amount=%llu status=%u\n",axis,less,(unsigned long long)amount,status);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK)); library_compile_owner_drop(&owner);
    }
    puts("Map index whole compiler exact/minus1 allocated/live/work PASS");
}
int main(int argc, char **argv) {
    CHECK(argc>=1 && argc<=3);
    const char *mode=argc>1?argv[1]:"--full";
    CHECK(!strcmp(mode,"--normal") || !strcmp(mode,"--resources") || !strcmp(mode,"--full"));
    char directory[XR_TEST_PATH_MAX]="map-index-compiler-XXXXXX", root[XR_TEST_PATH_MAX], path[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,root,sizeof(root)));
    CHECK(snprintf(path,sizeof(path),"%s/root.xr",root)>0);
    FILE *file=fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(index_source,1,strlen(index_source),file)==strlen(index_source) && !fclose(file));
    MembersCompilerInput input={root,path,argc==3?argv[2]:XR_P09_HASH_STDLIB}; size_t sites=0;
    XrCompileResourceStats required=members_baseline(&input,&sites);
    if (!strcmp(mode,"--full")) members_compiler_faults(&input,sites);
    if (strcmp(mode,"--normal")) members_compiler_limits(&input,required);
    CHECK(!remove(path) && !xr_test_rmdir(directory));
    library_compile_observer_free(); return 0;
}
