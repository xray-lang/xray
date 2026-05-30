#include "xm.h"
#include "xm_helper_table.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static XmIns *emit_call_c(XmFunc *func, XmBlock *blk, void *fn_ptr, uint8_t flags) {
    XmRef fn = xm_const_ptr(func, fn_ptr);
    XmRef extra = xm_const_i64(func, 0);
    XmRef result = xm_emit(func, blk, XM_CALL_C, XR_REP_I64, fn, extra);
    assert(xm_ref_is_vreg(result));
    XmIns *ins = &blk->ins[blk->nins - 1];
    ins->flags |= flags;
    return ins;
}

int main(void) {
    XmFunc *func = xm_func_new("call_protocol");
    XmBlock *blk = xm_func_add_block(func, "entry");

    XmIns *call_func = emit_call_c(func, blk, xm_helper_func(XM_HELPER_call_func), 0);
    assert(xm_helper_call_c_post_call_protocol(func, call_func) == XM_HPC_DEOPT);
    assert(xm_helper_call_c_needs_deopt_check(func, call_func));
    assert(xm_helper_call_c_protocol_matches_flags(func, call_func));

    XmIns *chan_send = emit_call_c(func, blk, xm_helper_func(XM_HELPER_chan_send), 0);
    assert(xm_helper_call_c_post_call_protocol(func, chan_send) == XM_HPC_SUSPEND);
    assert(xm_helper_call_c_needs_deopt_check(func, chan_send));
    assert(xm_helper_call_c_protocol_matches_flags(func, chan_send));

    XmIns *throw_call = emit_call_c(func, blk, xm_helper_func(XM_HELPER_throw), 0);
    assert(xm_helper_call_c_post_call_protocol(func, throw_call) == XM_HPC_THROW);
    assert(!xm_helper_call_c_needs_deopt_check(func, throw_call));
    assert(!xm_helper_call_c_protocol_matches_flags(func, throw_call));
    throw_call->flags |= XM_FLAG_MAY_THROW;
    assert(xm_helper_call_c_protocol_matches_flags(func, throw_call));

    XmIns *unknown_call = emit_call_c(func, blk, (void *) (uintptr_t) 0x12345678u, 0);
    assert(xm_helper_call_c_post_call_protocol(func, unknown_call) ==
           (XM_HPC_DEOPT | XM_HPC_SUSPEND));
    assert(xm_helper_call_c_needs_deopt_check(func, unknown_call));
    assert(xm_helper_call_c_protocol_matches_flags(func, unknown_call));

    XmIns probe = {0};
    probe.op = XM_ADD;
    assert(!xm_ins_is_call_site(&probe));
    probe.op = XM_CALL_C;
    assert(xm_ins_is_call_site(&probe));
    probe.op = XM_CALL_KNOWN;
    assert(xm_ins_is_call_site(&probe));
    probe.op = XM_CALL_C_LEAF;
    assert(xm_ins_is_call_site(&probe));

    xm_func_destroy(func);
    fprintf(stderr, "call protocol checks passed\n");
    return 0;
}
