/* Current layout rejects actual old metadata before touching protected pointees. */
#include "xir/xxir_program.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_program_internal.h"
#include "execution/xr_xir_host_execution.h"
#include "base/xsha256.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_sdk_resource_test.h"
extern const XrXirProgramSpec current29_program;
extern const XrXirProgramSpec old28_program;
extern const void *old28_guarded(unsigned declarations);
extern const void *old28_guard_page(void);
extern unsigned old28_callback_count(void);
extern unsigned old28_release_count(void);
extern unsigned old28_lease_count(void);
extern void old28_guard_clear(void);
static const XrCompileResourceLimits limits={16777216,8388608,67108864};
static void unchanged(XrCompileResources *resources,XrCompileResourceStats before,size_t attempts) {
    XrCompileResourceStats after=sdk_stats(resources);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
    CHECK(after.live_bytes==before.live_bytes && after.peak_bytes==before.peak_bytes && after.work==before.work);
    CHECK(runtime_attempts==attempts && !old28_callback_count() && !old28_release_count() && !old28_lease_count());
}
int main(void) {
    _Static_assert(XR_XIR_PROGRAM_ABI_VERSION==29 && XR_XIR_VALUE_ABI_VERSION==21 && XR_XIR_CALL_ABI_VERSION==26,"current identity");
    _Static_assert(sizeof(XrXirNominalDeclaration)==128 && sizeof(XrXirNominalIdentity)==112,"actual new record strides");
    XrXirCompileContext context={sdk_ledger(&limits),xr_xir_compile_default_limits()};
    size_t base_live=runtime_live,base_bytes=runtime_bytes;
    for (unsigned declarations=0;declarations<2;++declarations) {
        const XrXirProgramSpec *spec=old28_guarded(declarations);CHECK(spec->abi_version==28);
        MEMORY_BASIC_INFORMATION page;
        CHECK(VirtualQuery(old28_guard_page(),&page,sizeof(page))==sizeof(page) && page.Protect==PAGE_NOACCESS);
        XrCompileResourceStats before=sdk_stats(context.resources);size_t attempts=runtime_attempts;
        XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_program_seal(&context,spec,&program)==XR_XIR_BAD_LAYOUT && !program);
        unchanged(context.resources,before,attempts);CHECK(runtime_live==base_live && runtime_bytes==base_bytes);
        XrXirProgram *occupied=(XrXirProgram *)(uintptr_t)1;
        CHECK(xr_xir_compile_program_seal(&context,spec,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==(XrXirProgram *)(uintptr_t)1);
        unchanged(context.resources,before,attempts);old28_guard_clear();
    }
    CHECK(current29_program.abi_version==29 && current29_program.target.abi_version==21);
    CHECK(current29_program.types && current29_program.types->nominals->count==2);
    const XrXirNominalIdentity *identities=current29_program.types->nominals->identities;
    CHECK(identities && !current29_program.types->nominals->declarations);
    CHECK(identities[0].name.length==5 && !memcmp(identities[0].name.bytes,"First",5));
    CHECK(identities[1].name.length==6 && !memcmp(identities[1].name.bytes,"Second",6));
    for (uint32_t i=0;i<2;++i) {
        CHECK(!identities[i].native.native_id);
        for (uint32_t n=0;n<32;++n) CHECK(!identities[i].native.source_fingerprint[n]);
    }
    CHECK(old28_program.proof.length>64 && old28_program.proof.bytes[8]==24 && old28_program.proof.bytes[12]==63);
    uint8_t old_hash[32];xr_sha256(old28_program.proof.bytes,old28_program.proof.length,old_hash);
    CHECK(!memcmp(old_hash,old28_program.proof.identity,32));
    XrXirProgramSpec stale_proof=current29_program;stale_proof.proof=old28_program.proof;
    XrCompileResourceStats before=sdk_stats(context.resources);size_t attempts=runtime_attempts;
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&stale_proof,&program)==XR_XIR_BAD_STRUCTURE && !program);
    XrCompileResourceStats after=sdk_stats(context.resources);
    CHECK(after.work>before.work && after.allocation_count>=before.allocation_count && after.live_bytes==before.live_bytes);
    CHECK(runtime_attempts>=attempts && runtime_live==base_live && runtime_bytes==base_bytes);
    printf("Current metadata shape + old proof refusal: actual temporary allocations=%zu, charged work=%llu\n",
        runtime_attempts-attempts,(unsigned long long)(after.work-before.work));
    XrXirArtifact *old_checked=NULL;
    CHECK(xr_xir_compile_checked_read(&context,old28_program.proof.bytes,old28_program.proof.length,
        &old_checked,NULL)==XR_XIR_BAD_STRUCTURE && !old_checked);
    CHECK(!old28_callback_count() && !old28_release_count() && !old28_lease_count());
    XrXirNominalIdentity *producer_ids=xr_malloc(sizeof(XrXirNominalIdentity[2]));CHECK(producer_ids);
    char *producer_names=xr_malloc(11);CHECK(producer_names);
    memcpy(producer_ids,identities,sizeof(XrXirNominalIdentity[2]));
    memcpy(producer_names,"FirstSecond",11);
    producer_ids[0].name.bytes=producer_names;producer_ids[1].name.bytes=producer_names+5;
    XrXirNominalTable producer_table={NULL,2,producer_ids};
    XrXirTypes producer_types=*current29_program.types;producer_types.nominals=&producer_table;
    XrXirProgramSpec producer_spec=current29_program;producer_spec.types=&producer_types;
    CHECK(xr_xir_compile_program_seal(&context,&producer_spec,&program)==XR_XIR_OK && program);
    CHECK(program->types && program->types!=&producer_types && program->types->nominals!=&producer_table);
    CHECK(program->types->nominals->identities!=producer_ids);
    memset(producer_ids,0xa5,sizeof(XrXirNominalIdentity[2]));memset(producer_names,0xa5,11);
    xr_free(producer_ids);xr_free(producer_names);
    const XrXirNominalIdentity *owned_ids=program->types->nominals->identities;
    CHECK(owned_ids[0].name.length==5 && !memcmp(owned_ids[0].name.bytes,"First",5));
    CHECK(owned_ids[1].name.length==6 && !memcmp(owned_ids[1].name.bytes,"Second",6));
    for (uint32_t i=0;i<2;++i) {
        CHECK(!owned_ids[i].native.native_id);
        for (uint32_t n=0;n<32;++n) CHECK(!owned_ids[i].native.source_fingerprint[n]);
    }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirHostExecutionRequest request={program,&config,1,NULL,0};XrXirCallResult result={0};
    CHECK(xr_xir_host_execute(&request,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.value.type==XR_XIR_I64 && result.value.payload==42);
    xr_xir_call_result_drop(&result);xr_xir_compile_program_drop(program);
    CHECK(runtime_live==base_live && runtime_bytes==base_bytes);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);
    puts("Program28 two actual old strides before guard-page/pointee/callback/lease/work/allocation; current29 old24/63 proof BAD_STRUCTURE; current two-record independent42 physical0 PASS");
    return 0;
}
