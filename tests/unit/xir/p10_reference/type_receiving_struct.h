/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_struct.h - Exact received struct reads retain birth carriers
 */
#include "xir/xxir_struct.h"
#include "../xir_nominal_fixture.h"
static XrXirTypeArena *receiving_struct_arena(SourceFixtureOwner *compiler,unsigned shift) {
    NominalIdentityFixture fixture;nominal_identity_fixture(&fixture);
    fixture.identities[0].field_count=1;fixture.table.count=1;
    XrXirType field=XR_XIR_I64;XrXirTypeNode nodes[2]={0};CHECK(shift<=1);
    if(shift)nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL};
    nodes[shift]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0,.fields=&field,.field_count=1}};
    XrXirTypes types={nodes,shift+1,&fixture.table,NULL};XrXirTypeArena *output=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&types,&output)==XR_XIR_VALUE_OK);return output;
}
static void receiving_struct_reads(SourceFixtureOwner *compiler) {
    XrXirTypeArena *source=receiving_struct_arena(compiler,0),*target=receiving_struct_arena(compiler,1);
    XrXirTypeArena *unrelated=receiving_struct_arena(compiler,1);
    XirTypeReceivingRoot root={type(0),type(1)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    XrXirValue field={XR_XIR_I64,0,73},value={0},output={0};
    CHECK(xr_xir_struct_new(type(0),&field,1,&admission,&value)==XR_XIR_VALUE_OK);
    admission.arena=target;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t attempts=runtime_attempts;
    CHECK(xr_xir_struct_get(&value,0,&admission,&output)==XR_XIR_VALUE_OK && output.type==XR_XIR_I64 && output.payload==73);
    xr_xir_value_drop(&output);
    CHECK(value.type==(uint32_t)type(0) && xr_xir_value_arena(&value)==source);
    uint64_t work=admission.work;admission.work=0;
    CHECK(xr_xir_struct_get(&value,0,&admission,&output)==XR_XIR_VALUE_LIMIT && !output.type && !output.payload);
    admission.work=work;admission.arena=unrelated;
    CHECK(xr_xir_struct_get(&value,0,&admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT && !output.type && !output.payload);
    admission.arena=target;
    CHECK(xr_xir_struct_get(&value,1,&admission,&output)==XR_XIR_VALUE_BAD_ARGUMENT && !output.type && !output.payload);
    CHECK(xr_compile_resources_stats(compiler->context.resources,&after)==XR_COMPILE_RESOURCE_OK &&
        before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && before.work==after.work &&
        runtime_attempts==attempts);
    xr_xir_compile_type_arena_drop(source);
    CHECK(xr_xir_struct_get(&value,0,&admission,&output)==XR_XIR_VALUE_OK && output.payload==73);
    xr_xir_value_drop(&output);xr_xir_value_drop(&value);
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(unrelated);xr_xir_domain_drop(domain);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Received struct field reads resolve exact local identity before using birth ordinals; unrelated clone, missing work and bad field preserve empty output; no allocation and physical0");
}
