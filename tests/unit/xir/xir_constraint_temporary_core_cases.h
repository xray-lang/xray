/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_constraint_temporary_cases.h - Identity-map temporary ownership
 *
 * KEY CONCEPT:
 *   Constraint record comparison releases its temporary identity substitution.
 */
#ifndef XIR_CONSTRAINT_TEMPORARY_CORE_CASES_H
#define XIR_CONSTRAINT_TEMPORARY_CORE_CASES_H
static void constraint_temporary_cases(void) {
    XrXirConstraint empty={0};
    XrXirInterfaceDeclaration declaration={.module={"m",1},.name={"Evidence",8},
        .constraints=&empty,.parameter_count=1};
    XrXirInterfaceTable table={&declaration,1};
    XrXirTypes types={NULL,0,NULL,&table};
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirInterfaceApplication app={0,&parameter,1};
    XrXirConstraint constraint={0,&app,1};
    XrXirBudget b=xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types,&b)==XR_XIR_OK);
    b=xr_xir_default_budget();b.metadata_bytes=0;b.scratch_bytes=sizeof(parameter);
    for(uint32_t i=0;i<32;++i) {
        CHECK(xr_xir_constraint_records_match(&types,constraint,&types,constraint,1,&b)==XR_XIR_OK);
        CHECK(b.scratch_bytes==sizeof(parameter) && b.metadata_bytes==0);
    }
    fail_at=calls;
    CHECK(xr_xir_constraint_records_match(&types,constraint,&types,constraint,1,&b)==XR_XIR_OUT_OF_MEMORY);
    CHECK(b.scratch_bytes==sizeof(parameter) && b.metadata_bytes==0);
    CHECK(live==0);fail_at=SIZE_MAX;
    XrXirType wrong=XR_XIR_I64;XrXirInterfaceApplication wrong_app={0,&wrong,1};
    XrXirConstraint different={0,&wrong_app,1};
    CHECK(xr_xir_constraint_records_match(&types,constraint,&types,different,1,&b)==XR_XIR_BAD_TYPE);
    CHECK(b.scratch_bytes==sizeof(parameter) && b.metadata_bytes==0);
}
#endif // XIR_CONSTRAINT_TEMPORARY_CORE_CASES_H
