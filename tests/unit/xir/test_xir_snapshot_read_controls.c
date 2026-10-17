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
#include "xir_invocation_row_data_cases.h"
#include "xir_invocation_row_reference_cases.h"
#include "xir_invocation_row_phase_cases.h"
#include "xir_invocation_final_vector_cases.h"
#include "xir_snapshot_run_extent_cases.h"
#include "xir_snapshot_literal_block_cases.h"
#include "xir_invocation_final_row_lifetime_cases.h"
#include "xir_snapshot_streaming_writer_cases.h"
#include "xir_snapshot_exact_writer_cases.h"
#include "xir_snapshot_record_owner_cases.h"
#include "xir_invocation_queued_child_cases.h"
#include "xir_invocation_new_child_front_cases.h"
#include "xir_invocation_cause_directory_cases.h"
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--queued-child-producers")) {
        invocation_queued_child_returned_controls();CHECK(!rp_live && !rp_live_bytes);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--codec-literals")) {
        invocation_cause_directory_controls();invocation_new_child_front_controls();invocation_queued_child_controls();snapshot_record_owner_controls();snapshot_exact_writer_controls();snapshot_streaming_writer_controls();snapshot_literal_block_controls();conditional_compact_literals();snapshot_run_extent_controls();CHECK(!rp_live && !rp_live_bytes);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--final-vectors")) {
        invocation_final_vector_controls();invocation_final_row_lifetime_controls();CHECK(!rp_live && !rp_live_bytes);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--row-data")) {
        invocation_row_data_controls();invocation_row_reference_controls();invocation_row_phase_controls();
        CHECK(!rp_live && !rp_live_bytes);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--class-literals")) {
        conditional_class_rows_literals();conditional_class_cell_literals();
        CHECK(!rp_live && !rp_live_bytes);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--cell-literals")) {
        cell_owned_run_literals();CHECK(!rp_live && !rp_live_bytes);return 0;
    }
    return snapshot_original_main(argc,argv);
}
