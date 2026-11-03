/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_fixed_array_scalar_value.c - Scalar stride and failure-atomic projections
 *
 * KEY CONCEPT:
 *   Shared boxed values and inline Array elements use the same transaction.
 */
#include "xir/xxir_fixed_array.h"
#include "xir/xxir_array.h"
#include "xir/xxir_value_place.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"fixed scalar value %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "../xir_source_fixture_owner.h"
XR_FUNC void fixed_bytes_fail_next(void);
XR_FUNC bool fixed_bytes_failure_observed(void);
static void fixed_scalar_family(const XrXirCompileContext *context,XrXirType element,uint32_t stride) {
    XrXirType fixed=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,array=(XrXirType)(fixed+1);
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_FIXED_ARRAY,.element=element,.parameter_count=3},
        {.kind=XR_XIR_TYPE_ARRAY,.element=fixed}};
    XrXirTypes types={nodes,2,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)==XR_XIR_VALUE_OK);
    const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(arena,fixed);
    CHECK(layout && layout->value.size==3u*stride && layout->value.alignment==stride);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    uint64_t baseline=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,1048576};
    uint64_t bits=element==XR_XIR_U8?255:element==XR_XIR_U32?UINT32_MAX:UINT64_MAX;
    int64_t payload=0;memcpy(&payload,&bits,sizeof(payload));
    XrXirValue inputs[]={{element,0,0},{element,0,payload},{element,0,42}};
    XrXirValue original={0},copy={0},packed={0},restored={0},out={0};XrXirFaultDetail fault={0};
    fixed_bytes_fail_next();
    CHECK(xr_xir_fixed_array_new(fixed,inputs,3,&admission,&out)==XR_XIR_VALUE_OOM &&
        fixed_bytes_failure_observed() && !out.type && xr_xir_domain_stats(domain).live_bytes==baseline);
    CHECK(xr_xir_fixed_array_new(fixed,inputs,3,&admission,&original)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&original,&copy)==XR_XIR_VALUE_OK);
    XrXirValuePlace root={fixed,&copy.payload};XrXirValuePathStep step={XR_XIR_PATH_INDEX,fixed,1};
    XrXirValuePath path={&step,1};XrXirValue replacement={element,0,7};
    fixed_bytes_fail_next();
    CHECK(xr_xir_value_path_write(&root,&path,&replacement,&admission,&fault)==XR_XIR_VALUE_OOM &&
        fixed_bytes_failure_observed() && copy.payload==original.payload);
    admission.work=0;
    CHECK(xr_xir_value_path_write(&root,&path,&replacement,&admission,&fault)==XR_XIR_VALUE_LIMIT &&
        copy.payload==original.payload);
    admission.work=1000000;step.selector=3;
    CHECK(xr_xir_value_path_write(&root,&path,&replacement,&admission,&fault)==XR_XIR_VALUE_BOUNDS &&
        copy.payload==original.payload && fault.index==3 && fault.length==3);
    step.selector=1;CHECK(xr_xir_value_path_write(&root,&path,&replacement,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(copy.payload!=original.payload);
    CHECK(xr_xir_fixed_array_get(&copy,1,&out,&fault)==XR_XIR_VALUE_OK && out.type==element && out.payload==7);
    xr_xir_value_drop(&out);
    CHECK(xr_xir_fixed_array_get(&original,1,&out,&fault)==XR_XIR_VALUE_OK && out.payload==payload);
    xr_xir_value_drop(&out);
    CHECK(xr_xir_array_new(array,&original,1,&admission,&packed)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&packed,0,&admission,&restored,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_fixed_array_get(&restored,1,&out,&fault)==XR_XIR_VALUE_OK && out.type==element && out.payload==payload);
    xr_xir_value_drop(&out);xr_xir_value_drop(&restored);
    XrXirValuePathStep nested[]={{XR_XIR_PATH_INDEX,array,0},{XR_XIR_PATH_INDEX,fixed,2}};
    XrXirValuePath nested_path={nested,2};XrXirValuePlace nested_root={array,&packed.payload};
    CHECK(xr_xir_value_path_write(&nested_root,&nested_path,&replacement,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&packed,0,&admission,&restored,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_fixed_array_get(&restored,2,&out,&fault)==XR_XIR_VALUE_OK && out.payload==7);
    xr_xir_value_drop(&out);xr_xir_value_drop(&restored);
    if(element!=XR_XIR_U8) {
        const uint8_t raw[]={1,2,3};
        CHECK(xr_xir_fixed_array_from_bytes(fixed,raw,3,&admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && !out.type);
    }
    xr_xir_value_drop(&copy);xr_xir_value_drop(&original);xr_xir_value_drop(&packed);
    CHECK(xr_xir_domain_stats(domain).live_bytes==baseline);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
}
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    fixed_scalar_family(&owner.context,XR_XIR_U8,1);
    fixed_scalar_family(&owner.context,XR_XIR_U32,4);
    fixed_scalar_family(&owner.context,XR_XIR_U64,8);
    source_fixture_owner_free(&owner);return 0;
}
