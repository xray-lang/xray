/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_visibility_cases.h - Nested native descriptor access boundaries
 */
#ifndef XIR_NOMINAL_VISIBILITY_CASES_H
#define XIR_NOMINAL_VISIBILITY_CASES_H
static void nominal_visibility_cases(const XrXirProgramSpec *base) {
    CHECK(base->entry_count == 9 && base->declarations->slot_count == 5);
    CHECK(base->types->nominals->count == 2 && base->types->nominals->identities);
    XrXirNominalIdentity identities[2];
    memcpy(identities, base->types->nominals->identities, sizeof(identities));
    XrXirNominalTable table = {NULL, 2, identities};
    XrXirType arguments[] = {XR_XIR_I64, (XrXirType)256};
    XrXirType fields[] = {XR_XIR_I64, XR_XIR_STRING, (XrXirType)256, XR_XIR_STRING};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {1, arguments, 1, fields, 2}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, arguments + 1, 1, fields + 2, 2}},
        {XR_XIR_TYPE_CELL, (XrXirType)257, NULL, 0, XR_XIR_UNIT, 0, 0, {0}}};
    XrXirTypes types = {nodes, 3, &table, NULL};
    XrXirProgram *original = NULL;
    CHECK(xr_xir_program_seal(base, (XrXirProgramBudget) {2097152, 16000000}, &original) == XR_XIR_OK);
    xr_xir_program_drop(original);
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirProgramSpec spec = *base; spec.types = &types;
        XrXirDeclarations declarations = *base->declarations;
        XrXirSlot slots[6]; memcpy(slots, declarations.slots, 5 * sizeof(*slots));
        XrXirCallEntry entries[9]; memcpy(entries, base->entries, sizeof(entries));
        XrXirType parameters[] = {(XrXirType)(mode == 3 ? 258 : 257), XR_XIR_STRING};
        if (!mode) {
            slots[5] = (XrXirSlot) {declarations.root_module, (XrXirType)257, 1};
            declarations.slots = slots; declarations.slot_count = 6;
        } else if (mode == 1) entries[8].result = (XrXirType)257;
        else entries[8].parameters = parameters;
        spec.declarations = &declarations; spec.entries = entries;
        XrXirProgram *program = NULL;
        /* Shape-valid descriptor edits still need a matching Checked producer. */
        CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_BAD_STRUCTURE && !program);
        xr_xir_program_drop(program); program = NULL;
        identities[1].exported = 0;
        CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget) {2097152, 16000000}, &program) == (mode ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE) && !program);
        if (!mode) {
            XrXirBudget budget = xr_xir_default_budget();
            budget.metadata_bytes = 65536; budget.work = 100000;
            CHECK(xr_xir_declarations_verify(&declarations, &types, 9,
                &budget) == XR_XIR_BAD_TYPE);
        }
        identities[1].exported = 1;
    }
}
#endif // XIR_NOMINAL_VISIBILITY_CASES_H
