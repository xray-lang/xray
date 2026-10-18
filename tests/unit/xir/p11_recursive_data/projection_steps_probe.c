#define main materialization_original_main
#include "../p11_materialization/test_materialization.c"
#undef main
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include "xir/xxir_cell_owner_internal.h"

static void query(const XrXirValue *cell,XrXirValueAdmission admission,uint32_t expected,
    const XrXirCompileContext *context) {
    /* This immutable five-node fixture has a fixed complete shallow root span. */
    CHECK(xr_compile_resources_work(context->resources,256)==XR_COMPILE_RESOURCE_OK);
    admission.work=64;uint32_t count=UINT32_MAX;size_t attempts=source_fixture_compile_attempts;
    XrCompileResourceStats before=stats(context);
    CHECK(xr_xir_cell_projection_steps(cell,&admission,&count)==XR_XIR_VALUE_OK && count==expected && !admission.work);
    CHECK(source_fixture_compile_attempts==attempts && stats(context).work-before.work==64);
    CHECK(stats(context).live_bytes==before.live_bytes);
    for (uint64_t work=0;work<64;++work) {
        admission.work=work;count=123;before=stats(context);
        CHECK(xr_xir_cell_projection_steps(cell,&admission,&count)==XR_XIR_VALUE_LIMIT && count==123);
        CHECK(xr_xir_cell_projection_steps(cell,&admission,&count)==XR_XIR_VALUE_LIMIT && count==123);
        CHECK(admission.work==work && stats(context).work==before.work && source_fixture_compile_attempts==attempts);
    }
}
int main(void) {
    XrXirCompileContext context;context_new(&context);
    XrXirTypeNode nodes[5]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=256},
        {.kind=XR_XIR_TYPE_CELL,.element=257},
        {.kind=XR_XIR_TYPE_CELL,.element=256},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64}
    };
    XrXirTypes types={nodes,5,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(context.resources,caps.live_bytes,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=1000000,.scratch_bytes=1048576};
    XrXirValue scalar={.type=XR_XIR_I64,.payload=42},inner={0},outer={0},cell={0},first={0},deep={0};
    CHECK(xr_xir_array_new(256,&scalar,1,&admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(257,&inner,1,&admission,&outer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,258,&outer,&admission,&cell)==XR_XIR_VALUE_OK);
    XrXirCellAuthority authority={&context,&admission,1};
    XrXirValuePathStep step={XR_XIR_PATH_INDEX,257,0};XrXirValuePath path={&step,1};
    CHECK(xr_xir_cell_project_authorized(259,&cell,&path,&authority,&admission,&first)==XR_XIR_VALUE_OK);
    step.container=256;
    CHECK(xr_xir_cell_project_authorized(260,&first,&path,&authority,&admission,&deep)==XR_XIR_VALUE_OK);
    query(&cell,admission,0,&context);query(&first,admission,1,&context);query(&deep,admission,2,&context);
    XrXirTypeArena *foreign_arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&foreign_arena)==XR_XIR_VALUE_OK);
    XrXirValueAdmission rejected=admission;rejected.arena=foreign_arena;uint32_t count=123;
    CHECK(xr_xir_cell_projection_steps(&deep,&rejected,&count)==XR_XIR_VALUE_BAD_ARGUMENT && count==123);
    XrXirDomain *foreign_domain=NULL;CHECK(xr_xir_compile_domain_new(context.resources,caps.live_bytes,&foreign_domain)==XR_XIR_VALUE_OK);
    rejected=admission;rejected.domain=foreign_domain;
    CHECK(xr_xir_cell_projection_steps(&deep,&rejected,&count)==XR_XIR_VALUE_BAD_ARGUMENT && count==123);
    CHECK(xr_xir_cell_projection_steps(&scalar,&admission,&count)==XR_XIR_VALUE_BAD_ARGUMENT && count==123);
    CHECK(xr_xir_cell_projection_steps(&deep,&admission,NULL)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_compile_type_arena_drop(foreign_arena);xr_xir_domain_drop(foreign_domain);
    xr_xir_value_drop(&outer);xr_xir_value_drop(&inner);xr_xir_value_drop(&cell);xr_xir_value_drop(&first);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);xr_compile_resources_release(context.resources);
    query(&deep,admission,2,&context);
    source_fixture_compile_fail_at=source_fixture_compile_attempts;xr_xir_value_drop(&deep);
    CHECK(!source_fixture_compile_injected && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Real Cell selector metadata:0/1/2, exact64 work, local0-63 retry, foreign arena/Domain, producer death, final drop physical0\n");
    return 0;
}
