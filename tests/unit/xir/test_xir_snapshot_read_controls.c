/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_snapshot_read_controls.c - Current body readers with original controls
 */
#define main snapshot_original_main
#include "test_xir_root_conditional_helpers.c"
#undef main
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--cell-literals")) {
        cell_owned_run_literals();CHECK(!rp_live && !rp_live_bytes);return 0;
    }
    return snapshot_original_main(argc,argv);
}
