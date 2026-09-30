/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_constraint_query_allocations.h - Independent constraints in all query owners
 */
#ifndef XIR_CONSTRAINT_QUERY_ALLOCATIONS_H
#define XIR_CONSTRAINT_QUERY_ALLOCATIONS_H
static void constraint_query_allocations(void) {
    for (unsigned mixed = 0; mixed < 2; ++mixed) {
        size_t sites = 0;
        for (size_t site = 0; site <= sites; ++site) {
            XrXirType argument = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
            XrXirInterfaceApplication application = {0,&argument,1};
            XrXirConstraint constraint = {mixed ? XR_XIR_CONSTRAINT_SENDABLE : 0,&application,1};
            XrXirSourceDeclaration declaration = {0}; declaration.name = "generic";
            declaration.generic_constraints = &constraint; declaration.generic_parameter_count = 1;
            XrXirNominalDeclaration nominal = {
                {"alpha",5},{"Box",3},1,&constraint,1,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0};
            XrXirNominalTable nominals = {&nominal,1,NULL};
            XrXirInterfaceDeclaration interface = {{"alpha",5},{"Needs",5},1,&constraint,1,NULL,0,NULL,0};
            XrXirInterfaceTable interfaces = {&interface,1};
            XrXirTypes types = {NULL,0,&nominals,&interfaces};
            XrXirSourceView view = {0}; view.types = &types; view.declarations = &declaration; view.declaration_count = 1;
            XrXirBudget budget = xr_xir_default_budget();
            attempts = 0; fail_at = site ? site - 1 : SIZE_MAX;
            XrXirSourceSnapshot *snapshot = NULL;
            XrXirStatus status = xr_xir_source_snapshot_copy(&view,&budget,&snapshot);
            if (!site) {
                CHECK(status == XR_XIR_OK && snapshot); sites = attempts;
                XrXirBudget required = xr_xir_default_budget();
                required.work -= budget.work; required.metadata_bytes -= budget.metadata_bytes;
                for (unsigned boundary = 0; boundary < 3; ++boundary) {
                    XrXirBudget limit = required; XrXirSourceSnapshot *bounded = NULL;
                    if (boundary == 0) --limit.work;
                    if (boundary == 1) --limit.metadata_bytes;
                    CHECK(xr_xir_source_snapshot_copy(&view,&limit,&bounded) == (boundary == 2 ? XR_XIR_OK : XR_XIR_BUDGET));
                    CHECK((bounded != NULL) == (boundary == 2)); xr_xir_source_snapshot_free(bounded);
                }
                memset(&constraint,0xcc,sizeof(constraint)); memset(&application,0xcc,sizeof(application));
                argument = XR_XIR_UNIT; memset(&nominal,0xcc,sizeof(nominal));
                memset(&interface,0xcc,sizeof(interface)); memset(&declaration,0xcc,sizeof(declaration));
                const XrXirSourceView *copy = xr_xir_source_snapshot_view(snapshot);
                const XrXirConstraint *owners[] = {copy->declarations[0].generic_constraints,
                    copy->types->nominals->declarations[0].constraints,copy->types->interfaces->declarations[0].constraints};
                for (unsigned i = 0; i < 3; ++i) {
                    CHECK(owners[i]->markers == (mixed ? XR_XIR_CONSTRAINT_SENDABLE : 0));
                    CHECK(owners[i]->interface_count == 1 && owners[i]->interfaces[0].declaration == 0);
                    CHECK(owners[i]->interfaces[0].argument_count == 1);
                    CHECK(owners[i]->interfaces[0].arguments[0] == XR_XIR_TYPE_PARAMETER_BASE);
                }
                CHECK(owners[0] != owners[1] && owners[0]->interfaces != owners[1]->interfaces);
            } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
            xr_xir_source_snapshot_free(snapshot); CHECK(!live);
        }
        fail_at = SIZE_MAX;
        printf("Constraint query snapshots (mixed=%u): %zu allocation failure sites\n",mixed,sites);
    }
}
#endif // XIR_CONSTRAINT_QUERY_ALLOCATIONS_H
