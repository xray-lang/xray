/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_access_temporary_cases.h - Access and declaration graph scratch lifetime
 *
 * KEY CONCEPT:
 *   Visibility and graph traversal use bounded temporary storage, not metadata.
 */
#ifndef XIR_ACCESS_TEMPORARY_CASES_H
#define XIR_ACCESS_TEMPORARY_CASES_H
static void access_temporary_cases(void) {
    XrXirNominalDeclaration nominal={.module={"a",1},.name={"S",1}};
    XrXirNominalTable table={&nominal,1,NULL};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL};
    XrXirTypes types={&node,1,&table,NULL};
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirFunctionIdentity identities[2]={{0},{0}};
    XrXirSourceModule source={"a",1,NULL,0,0};
    XrXirSlot slot={0,type,0};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule module={.stage=XR_XIR_CHECKED,.function_count=2,.declarations=&declarations,.types=&types};
    XrXirBudget b=xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types,&b)==XR_XIR_OK);
    b=xr_xir_default_budget();b.metadata_bytes=0;b.scratch_bytes=1;
    for(uint32_t i=0;i<32;++i) {
        uint64_t work=b.work;
        CHECK(xr_xir_type_access(&module,1,type,&b)==XR_XIR_OK);
        CHECK(b.scratch_bytes==1 && b.metadata_bytes==0 && b.work<work);
    }
    b.scratch_bytes=0;
    CHECK(xr_xir_type_access(&module,1,type,&b)==XR_XIR_BUDGET && !b.scratch_bytes);
    b.scratch_bytes=1;fail_at=calls;
    CHECK(xr_xir_type_access(&module,1,type,&b)==XR_XIR_OUT_OF_MEMORY);
    CHECK(b.scratch_bytes==1 && b.metadata_bytes==0 && !live);fail_at=SIZE_MAX;
    const uint64_t metadata=sizeof(declarations)+sizeof(source)+sizeof(identities)+sizeof(slot)+1;
    b=xr_xir_default_budget();b.metadata_bytes=metadata;b.scratch_bytes=5;
    calls=0;
    CHECK(xr_xir_declarations_verify(&declarations,&types,2,&b)==XR_XIR_OK);
    CHECK(b.metadata_bytes==0 && b.scratch_bytes==5 && !live);
    size_t sites=calls;CHECK(sites==3);
    for(size_t i=0;i<sites;++i) {
        b=xr_xir_default_budget();b.metadata_bytes=metadata;b.scratch_bytes=5;
        uint64_t work=b.work;calls=0;fail_at=i;
        CHECK(xr_xir_declarations_verify(&declarations,&types,2,&b)==XR_XIR_OUT_OF_MEMORY);
        CHECK(b.scratch_bytes==5 && b.work<work && !live);
    }
    fail_at=SIZE_MAX;
    b=xr_xir_default_budget();b.metadata_bytes=metadata;b.scratch_bytes=4;
    CHECK(xr_xir_declarations_verify(&declarations,&types,2,&b)==XR_XIR_BUDGET);
    CHECK(b.scratch_bytes==4 && !live);
    b=xr_xir_default_budget();b.metadata_bytes=metadata-1;b.scratch_bytes=5;
    CHECK(xr_xir_declarations_verify(&declarations,&types,2,&b)==XR_XIR_BUDGET);
    CHECK(b.scratch_bytes==5 && !live);
    b=xr_xir_default_budget();b.work=0;b.scratch_bytes=5;
    CHECK(xr_xir_declarations_verify(&declarations,&types,2,&b)==XR_XIR_BUDGET);
    CHECK(b.scratch_bytes==5 && !live);
    uint32_t dependencies[2]={1,0};
    XrXirSourceModule modules[2]={{"a",1,dependencies,1,0},{"b",1,dependencies+1,1,1}};
    XrXirFunctionIdentity cycle_ids[3]={{0},{.module=1},{0}};
    XrXirDeclarations cycle={.modules=modules,.module_count=2,.functions=cycle_ids,.entry_function=2};
    b=xr_xir_default_budget();b.scratch_bytes=10;
    uint64_t work=b.work;
    CHECK(xr_xir_declarations_verify(&cycle,NULL,3,&b)==XR_XIR_BAD_STRUCTURE);
    CHECK(b.scratch_bytes==10 && b.work<work && !live);
    cycle_ids[2].module=1;cycle.root_module=1;
    XrXirModule private_scope={.stage=XR_XIR_CHECKED,.function_count=3,.declarations=&cycle,.types=&types};
    b=xr_xir_default_budget();b.metadata_bytes=0;b.scratch_bytes=1;work=b.work;
    CHECK(xr_xir_type_access(&private_scope,2,type,&b)==XR_XIR_BAD_TYPE);
    CHECK(b.scratch_bytes==1 && b.metadata_bytes==0 && b.work<work && !live);
    calls=0;fail_at=SIZE_MAX;
}
#endif // XIR_ACCESS_TEMPORARY_CASES_H
