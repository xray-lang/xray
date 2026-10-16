#define main materialization_original_main
#include "../p11_materialization/test_materialization.c"
#undef main
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"

static void original_owner_boundaries(void) {
    for (uint64_t remaining=0;remaining<=64;++remaining) {
        XrXirCompileContext context;context_new(&context);
        const XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
        const XrXirTypes types={&node,1,NULL,NULL};
        XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
        CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_compile_domain_new(context.resources,caps.live_bytes,&domain)==XR_XIR_VALUE_OK);
        XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=1000000,.scratch_bytes=1048576};
        XrXirValue scalar={.type=XR_XIR_I64,.payload=42},value={0};
        CHECK(xr_xir_array_new(256,&scalar,1,&admission,&value)==XR_XIR_VALUE_OK);
        XrXirCtfeTypeShape shape={0};
        CHECK(xr_xir_compile_ctfe_type_shape(&context,&types,256,&shape)==XR_XIR_CTFE_OK && shape.work<=UINT64_MAX/32);
        CHECK(xr_compile_resources_work(context.resources,shape.work*32)==XR_COMPILE_RESOURCE_OK);
        uint64_t used=stats(&context).work;CHECK(used<=caps.work-remaining);
        CHECK(xr_compile_resources_work(context.resources,caps.work-used-remaining)==XR_COMPILE_RESOURCE_OK);
        size_t attempts=source_fixture_compile_attempts;XrCompileResourceStats before=stats(&context);
        XrXirDomain *borrowed=NULL;
        XrXirValueStatus status=xr_xir_compile_value_domain_borrow(context.resources,&value,arena,&borrowed);
        CHECK(status==(remaining==64?XR_XIR_VALUE_OK:XR_XIR_VALUE_LIMIT));
        CHECK(borrowed==(remaining==64?domain:NULL));
        CHECK(stats(&context).work-before.work==(remaining==64?64u:0u));
        borrowed=NULL;
        CHECK(xr_xir_compile_value_domain_borrow(context.resources,&value,arena,&borrowed)==XR_XIR_VALUE_LIMIT && !borrowed);
        CHECK(source_fixture_compile_attempts==attempts && stats(&context).live_bytes==before.live_bytes);
        CHECK(stats(&context).work<=caps.work);
        xr_xir_value_drop(&value);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Authentic producer ledger remaining0-64:64 success, all0-63 refuse/retry, no quota reset or allocation, final physical0\n");
}

int main(void) {
    original_owner_boundaries();
    XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};
    ordinary_source(&context,0,&source);XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t factory=named(xr_xir_compile_artifact_module(lowered),"outer");
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=factory,.target=target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,lowered->checked_identity,sizeof(request.evaluation.checked_identity));
    XrXirValue value={0};CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);
    XrXirCtfeTypeShape shape={0};const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(xr_xir_compile_ctfe_type_shape(&context,module->types,(XrXirType)value.type,&shape)==XR_XIR_CTFE_OK && shape.work<=UINT64_MAX/32);
    CHECK(xr_compile_resources_work(context.resources,shape.work*32)==XR_COMPILE_RESOURCE_OK);
    const XrXirTypeArena *arena=xr_xir_value_arena(&value);CHECK(arena);
    XrXirDomain *borrowed=NULL;XrCompileResourceStats before=stats(&context);size_t attempts=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_value_domain_borrow(context.resources,&value,arena,&borrowed)==XR_XIR_VALUE_OK && borrowed);
    CHECK(source_fixture_compile_attempts==attempts && stats(&context).work-before.work==64 && stats(&context).live_bytes==before.live_bytes);
    XrXirDomain *occupied=borrowed;before=stats(&context);
    CHECK(xr_xir_compile_value_domain_borrow(context.resources,&value,arena,&occupied)==XR_XIR_VALUE_BAD_ARGUMENT && occupied==borrowed);
    CHECK(stats(&context).work==before.work && source_fixture_compile_attempts==attempts);
    XrXirCompileContext foreign;context_new(&foreign);XrCompileResourceStats fresh=stats(&foreign);
    XrXirDomain *rejected=NULL;before=stats(&context);
    CHECK(xr_xir_compile_value_domain_borrow(foreign.resources,&value,arena,&rejected)==XR_XIR_VALUE_BAD_ARGUMENT && !rejected);
    CHECK(stats(&foreign).work-fresh.work==64 && stats(&context).work==before.work);
    XrCompileResourceLimits cut_limits=caps;cut_limits.work=fresh.work+63;
    XrXirCompileContext cut={0};CHECK(xr_compile_resources_new(&cut_limits,&cut.resources)==XR_COMPILE_RESOURCE_OK);
    attempts=source_fixture_compile_attempts;XrCompileResourceStats cut_before=stats(&cut);
    CHECK(xr_xir_compile_value_domain_borrow(cut.resources,&value,arena,&rejected)==XR_XIR_VALUE_LIMIT && !rejected);
    CHECK(xr_xir_compile_value_domain_borrow(cut.resources,&value,arena,&rejected)==XR_XIR_VALUE_LIMIT && !rejected);
    CHECK(source_fixture_compile_attempts==attempts && stats(&cut).live_bytes==cut_before.live_bytes && stats(&cut).work>=cut_before.work);
    xr_compile_resources_release(cut.resources);xr_compile_resources_release(foreign.resources);
    XrXirTypeArena *other=NULL;CHECK(xr_xir_compile_type_arena_new(&context,module->types,&other)==XR_XIR_VALUE_OK);
    CHECK(xr_compile_resources_work(context.resources,shape.work*32)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_value_domain_borrow(context.resources,&value,other,&rejected)==XR_XIR_VALUE_BAD_ARGUMENT && !rejected);
    xr_xir_compile_type_arena_drop(other);
    xr_xir_compile_source_result_free(&source);xr_xir_compile_artifact_free(lowered);
    CHECK(xr_compile_resources_work(context.resources,shape.work*32)==XR_COMPILE_RESOURCE_OK);
    XrCompileResources *original=context.resources;xr_compile_resources_release(context.resources);
    XrXirDomain *late=NULL;
    CHECK(xr_xir_compile_value_domain_borrow(original,&value,arena,&late)==XR_XIR_VALUE_OK && late==borrowed);
    borrowed=NULL;late=NULL;xr_xir_value_drop(&value);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Authentic compiler Domain synchronous borrow: zero allocations, exact64 work, occupied/foreign arena/ledger rejection,63 remaining refusal/retry, producer death and final physical0\n");
    return 0;
}
