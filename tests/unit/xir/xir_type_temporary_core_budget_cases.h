/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_type_temporary_budget_cases.h - Non-retained type proof storage
 *
 * KEY CONCEPT:
 *   Independent proofs consume work while returning temporary storage exactly.
 */
#ifndef XIR_TYPE_TEMPORARY_CORE_BUDGET_CASES_H
#define XIR_TYPE_TEMPORARY_CORE_BUDGET_CASES_H
static void type_temporary_budget_cases(void) {
    XrXirTypeNode node={XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0,{0}};
    XrXirTypes from={&node,1,NULL,NULL},to=from;
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirBudget b=xr_xir_default_budget(); b.metadata_bytes=0; b.scratch_bytes=1;
    uint64_t before_work=b.work;
    for(uint32_t i=0;i<32;++i) {
        CHECK(xr_xir_type_expression_shape(&from,type,0,&b)==XR_XIR_OK);
        CHECK(b.metadata_bytes==0 && b.scratch_bytes==1);
    }
    CHECK(b.work<before_work);
    b.scratch_bytes=0;
    CHECK(xr_xir_type_expression_shape(&from,type,0,&b)==XR_XIR_BUDGET);
    CHECK(b.metadata_bytes==0 && b.scratch_bytes==0);
    uint64_t minimum=0;
    for(uint64_t bytes=1;bytes<=128;++bytes) {
        b=xr_xir_default_budget(); b.metadata_bytes=0; b.scratch_bytes=bytes;
        XrXirStatus status=xr_xir_type_substitution_matches_between(&from,&to,NULL,0,type,type,&b);
        CHECK(b.metadata_bytes==0 && b.scratch_bytes==bytes);
        if(status==XR_XIR_OK) { minimum=bytes;break; }
        CHECK(status==XR_XIR_BUDGET);
    }
    CHECK(minimum>1);
    b=xr_xir_default_budget(); b.metadata_bytes=0; b.scratch_bytes=minimum;
    before_work=b.work;
    for(uint32_t i=0;i<32;++i) {
        CHECK(xr_xir_type_substitution_matches_between(&from,&to,NULL,0,type,type,&b)==XR_XIR_OK);
        CHECK(b.metadata_bytes==0 && b.scratch_bytes==minimum);
    }
    CHECK(b.work<before_work);
    XrXirTypeNode invalid=node;invalid.element=XR_XIR_BOOL;
    XrXirTypes wrong={&invalid,1,NULL,NULL};
    CHECK(xr_xir_type_substitution_matches_between(&from,&wrong,NULL,0,type,type,&b)==XR_XIR_BAD_TYPE);
    CHECK(b.metadata_bytes==0 && b.scratch_bytes==minimum);
    b.scratch_bytes=minimum-1;
    CHECK(xr_xir_type_substitution_matches_between(&from,&to,NULL,0,type,type,&b)==XR_XIR_BUDGET);
    CHECK(b.metadata_bytes==0 && b.scratch_bytes==minimum-1);
    b.scratch_bytes=minimum; b.work=0;
    CHECK(xr_xir_type_substitution_matches_between(&from,&to,NULL,0,type,type,&b)==XR_XIR_BUDGET);
    CHECK(b.metadata_bytes==0 && b.scratch_bytes==minimum && b.work==0);
}
/* Include after test_xir_allocations.c has installed its counted type
 * implementation. Two exact allocation sites fail without retaining scratch. */
static void type_temporary_allocation_failures(void) {
    XrXirTypeNode node={XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0,{0}};
    XrXirTypes from={&node,1,NULL,NULL},to=from;
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(live==0);
    for(uint32_t mode=0;mode<2;++mode) {
        XrXirBudget b=xr_xir_default_budget();b.metadata_bytes=0;b.scratch_bytes=128;
        fail_at=calls;
        XrXirStatus status=mode ? xr_xir_type_substitution_matches_between(&from,&to,NULL,0,type,type,&b) :
            xr_xir_type_expression_shape(&from,type,0,&b);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && b.metadata_bytes==0 && b.scratch_bytes==128);
        CHECK(live==0);
        fail_at=SIZE_MAX;
    }
}
#endif // XIR_TYPE_TEMPORARY_CORE_BUDGET_CASES_H
