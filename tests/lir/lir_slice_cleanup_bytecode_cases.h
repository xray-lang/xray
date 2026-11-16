/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LIR_SLICE_CLEANUP_BYTECODE_CASES_H
#define LIR_SLICE_CLEANUP_BYTECODE_CASES_H
#include "lir_slice_cleanup_fixture.h"

static void slice_cleanup_bytecode(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (size_t family=0;family<2;++family) for (unsigned shape=0;shape<SLICE_CLEANUP_CASE_COUNT;++shape)
        for (unsigned live=0;live<(shape==SLICE_CLEANUP_BRANCH?2u:1u);++live) {
        SliceCleanupFixture c=slice_cleanup_fixture(elements[family],(SliceCleanupCase)shape,live!=0);
        OK(xr_lir_module_finish(c.module,NULL));XrRbcProgram *program=compile(c.module,c.root);
        const XrRbcModule *module=xr_rbc_program_view(program);CHECK(xr_rbc_module_code_owner(module));
        bool target=false;size_t formal=0;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrRbcValueCode *code=xr_rbc_module_value_code(module,f);CHECK(code && code->homes);
            const XrLirValueHomes *homes=code->homes;
            size_t retained=0,physical=0;
            for (size_t n=0;n<homes->event_count;++n) {
                const XrLirHomeEvent *event=&homes->events[n];
                if (event->kind!=XR_LIR_OWNERSHIP_VIEW_END) continue;
                if (event->view_operation==XR_LIR_SLICE_OP_FORMAL_CLEANUP) {
                    CHECK(event->view_cleanup==XR_LIR_SLICE_CLEANUP_CLEAR && !event->view_workspace);++formal;continue;
                }
                if (event->view_operation!=XR_LIR_SLICE_OP_CLEANUP) continue;
                CHECK(event->view_cleanup<=XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED);
                size_t consumers=0;
                for (size_t a=0;a<code->access_count;++a) {
                    const XrRbcValueAccess *access=&code->accesses[a];
                    consumers+=access->operation==XR_RBC_VALUE_SLICE && access->index==XR_LIR_SLICE_OP_CLEANUP &&
                        access->window==n+1;
                }
                CHECK(consumers==(event->view_cleanup==XR_LIR_SLICE_CLEANUP_CLEAR?1u:0u));
                retained+=consumers;
                if (homes->source_function==c.worker.id && event->block==c.cleanup.id &&
                    event->instruction==c.cleanup_instruction && event->source==c.target.id) {
                    CHECK(!target && (event->view_cleanup==XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED)==c.unconstructed);
                    target=true;
                }
            }
            for (size_t a=0;a<code->access_count;++a) if (code->accesses[a].operation==XR_RBC_VALUE_SLICE) {
                CHECK(code->accesses[a].index!=XR_LIR_SLICE_OP_FORMAL_CLEANUP);
                physical+=code->accesses[a].index==XR_LIR_SLICE_OP_CLEANUP;
            }
            CHECK(physical==retained);
        }
        CHECK(target==(shape!=SLICE_CLEANUP_STRICT_END));
        CHECK(formal==((shape==SLICE_CLEANUP_CALL_BEFORE || shape==SLICE_CLEANUP_CALL_AFTER)?2u:0u));
        XrRbcModule foreign=*module;foreign.code_lease=(XrRbcCodeLease){0};XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_module_prepare(&foreign,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
        foreign=*module;++foreign.function_scope_count;
        CHECK(xr_rbc_module_prepare(&foreign,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
        /* The execution owns the same common events after both creators retire. */
        xr_lir_module_destroy(c.module);execute(program,(uint64_t)c.expected);
    }
}
#endif
