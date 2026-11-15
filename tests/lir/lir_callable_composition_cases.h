/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir_callable_composition_fixture.h"
static void callable_composition_receiving(void) {
    XrLirModule *module = NULL, *rebuilt = NULL, *inlined = NULL, *folded = NULL;
    XrLirFunction root = {0};
    callable_composition_fixture(&module, &root);
    OK(xr_lir_rebuild(module, &rebuilt));
    OK(xr_lir_inline(rebuilt, &inlined));
    OK(xr_lir_sccp(inlined, &folded));
    xr_lir_module_destroy(module); xr_lir_module_destroy(rebuilt); xr_lir_module_destroy(inlined);
    callable_execution_unavailable(folded, (XrLirFunction){folded, root.id});
    xr_lir_module_destroy(folded);
    puts("Owned function captures preserve valid transformed LIR and reject unavailable execution");
}
