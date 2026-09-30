/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_enum_layout_cases.h - Independent tagged-union storage expectations
 */
#ifndef XIR_ENUM_LAYOUT_CASES_H
#define XIR_ENUM_LAYOUT_CASES_H
static void enum_storage_layout(void) {
    XrXirNominalVariant variants[] = {
        {{"Empty", 5}, 0, 0}, {{"Small", 5}, 0, 2}, {{"Text", 4}, 2, 1}, {{"Last", 4}, 3, 0}};
    const XrXirNominalFieldIdentity fields[] = {{{"a", 1}, 0}, {{"b", 1}, 0}, {{"text", 4}, 0}};
    const XrXirNominalFieldIdentity outer_fields[] = {{{"flag", 4}, 0}, {{"choice", 6}, 0}, {{"end", 3}, 0}};
    XrXirNominalIdentity identities[] = {
        {{"alpha", 5}, {"Choice", 6}, 1, 0, fields, 3, XR_XIR_NOMINAL_ENUM, variants, 4, 0},
        {{"alpha", 5}, {"Outer", 5}, 1, 0, outer_fields, 3, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0}};
    XrXirNominalTable table = {NULL, 2, identities};
    XrXirType inner_types[] = {XR_XIR_I8, XR_XIR_BOOL, XR_XIR_STRING};
    XrXirType outer_types[] = {XR_XIR_BOOL, (XrXirType)256, XR_XIR_I16};
    XrXirTypeNode nodes[2] = {{0}};
    for (uint32_t i = 0; i < 2; ++i) {
        nodes[i].kind = XR_XIR_TYPE_NOMINAL; nodes[i].nominal.declaration = i;
        nodes[i].nominal.fields = i ? outer_types : inner_types; nodes[i].nominal.field_count = 3;
    }
    XrXirTypes types = {nodes, 2, &table, NULL};
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirBudget budget = xr_xir_default_budget(); XrXirLayout layout = {0};
    uint32_t offsets[3] = {99, 99, 99};
    CHECK(xr_xir_nominal_layout(&types, (XrXirType)256, &target, &budget, &layout, offsets, 3) == XR_XIR_OK);
    CHECK(layout.size == 16 && layout.alignment == 8 && offsets[0] == 8 && offsets[1] == 9 && offsets[2] == 8);
    budget = xr_xir_default_budget();
    CHECK(xr_xir_nominal_layout(&types, (XrXirType)257, &target, &budget, &layout, offsets, 3) == XR_XIR_OK);
    CHECK(layout.size == 32 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 8 && offsets[2] == 24);
    budget = xr_xir_default_budget(); budget.work = 1; XrXirBudget saved = budget;
    offsets[0] = offsets[1] = offsets[2] = 99;
    CHECK(xr_xir_nominal_layout(&types, (XrXirType)256, &target, &budget, &layout, offsets, 3) == XR_XIR_BUDGET);
    CHECK(!layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 && offsets[2] == 99);
    CHECK(!memcmp(&budget, &saved, sizeof(budget)));
    XrXirNominalField declarations[3] = {{{"a", 1}, XR_XIR_I8, 0}, {{"b", 1}, XR_XIR_BOOL, 0}, {{"text", 4}, XR_XIR_STRING, 0}};
    XrXirNominalDeclaration declaration = {{"alpha", 5}, {"Choice", 6}, 1, NULL, 0,
        declarations, 3, XR_XIR_NOMINAL_ENUM, variants, 4, 0};
    table.declarations = &declaration; table.identities = NULL; table.count = 1; types.count = 1;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_nominal_layout(&types, (XrXirType)256, &target, &budget, &layout, offsets, 3) == XR_XIR_OK);
    CHECK(layout.size == 16 && layout.alignment == 8 && offsets[0] == 8 && offsets[1] == 9 && offsets[2] == 8);
}
static void enum_tag_widths(void) {
    char names[257][8]; XrXirNominalVariant variants[257];
    for (uint32_t i = 0; i < 257; ++i) {
        int length = snprintf(names[i], sizeof(names[i]), "v%u", i);
        CHECK(length > 0 && length < 8);
        variants[i] = (XrXirNominalVariant) {{names[i], (uint32_t) length}, 0, 0};
    }
    XrXirNominalIdentity identity = {{"alpha", 5}, {"Unit", 4}, 1, 0, NULL, 0, XR_XIR_NOMINAL_ENUM, variants, 1, 0};
    XrXirNominalTable table = {NULL, 1, &identity};
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    XrXirTypes types = {&node, 1, &table, NULL};
    const uint32_t counts[] = {1, 2, 256, 257}, widths[] = {0, 1, 1, 2};
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    for (uint32_t i = 0; i < 4; ++i) {
        identity.variant_count = counts[i];
        XrXirBudget budget = xr_xir_default_budget(); XrXirLayout layout = {0};
        CHECK(xr_xir_nominal_layout(&types, (XrXirType)256, &target, &budget, &layout, NULL, 0) == XR_XIR_OK);
        CHECK(layout.size == widths[i] && layout.alignment == (widths[i] ? widths[i] : 1));
    }
}
#endif // XIR_ENUM_LAYOUT_CASES_H
