/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cell_instruction_storage.c - Preserve the owned Cell reader scratch contract
 */
#define main cell_storage_original_checks
#include "../../xir/test_xir_root_conditional_helpers.c"
#undef main
int main(void) {
    cell_owned_run_literals();
    puts("Original five owned Cell operations: scratch identity, full words, producer death and independent proof replay PASS");
    return 0;
}
