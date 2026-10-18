/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effect_analysis_context_controls.c - Original contextual identity controls
 *
 * KEY CONCEPT:
 *   Execute original literal assertions independently of their complete
 *   resource sweeps. These controls do not replace the complete helper gate.
 */
#define main original_conditional_helper_main
#include "test_xir_root_conditional_helpers.c"
#undef main
int main(void) {
    initial_ordinary_controls();ordinary_material_controls();
    conditional_literals();conditional_new_literals();conditional_sparse_literals();
    conditional_storage_literals();conditional_copy_literals();conditional_metadata_literals();
    conditional_physical_literals();conditional_dense_scratch_literals();conditional_dense_scratch_failures();
    conditional_descriptor_literals();conditional_class_rows_literals();conditional_class_cell_literals();
    conditional_parameter_literals();conditional_initial_role_literals();context_constraint_vector_controls();
    conditional_compact_literals();conditional_generation_literals();conditional_owned_error_literals();
    CHECK(!rp_live&&!rp_live_bytes);puts("Original20 contextual control functions passed; full resource sweeps remain separate");return 0;
}
