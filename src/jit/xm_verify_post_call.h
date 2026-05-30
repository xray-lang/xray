/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_verify_post_call.h - Post-call protocol verifier
 *
 * Verifies that every helper call emitted by the JIT driver includes
 * the correct post-call checks as declared in helpers.def (via
 * XM_HELPER_POST_CALL_*).  The driver records what it actually emitted
 * for each call site; at finalization time the verifier cross-checks
 * every record against the declared protocol.
 *
 * Three post-call obligations (combinable as bitmask):
 *   XM_HPC_DEOPT   — deopt bailout check after call return
 *   XM_HPC_THROW   — exception propagation after call return
 *   XM_HPC_SUSPEND — coroutine suspend check after call return
 *
 * A mismatch means the driver emitted code that silently skips a
 * required post-call obligation, or emits a redundant one. Both are
 * contract violations.
 */

#ifndef XM_VERIFY_POST_CALL_H
#define XM_VERIFY_POST_CALL_H

#include <stdbool.h>
#include <stdint.h>

#include "xm_helper_table.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"

#define XM_MAX_POST_CALL_RECORDS 512

typedef struct {
    uint32_t code_offset;
    uint16_t helper_id;        /* XM_HELPER_xxx or XM_HELPER__COUNT if unknown */
    uint8_t declared_protocol; /* from helpers.def (XM_HPC_*) */
    uint8_t emitted_protocol;  /* what the driver actually emitted */
} XmPostCallRecord;

typedef struct {
    XmPostCallRecord records[XM_MAX_POST_CALL_RECORDS];
    uint32_t count;
} XmPostCallTracker;

static inline void xm_post_call_init(XmPostCallTracker *t) {
    t->count = 0;
}

/*
 * Resolve the helper ID from a CALL_C instruction's first argument
 * (const ptr to function address). Returns XM_HELPER__COUNT if the
 * helper cannot be resolved.
 */
static inline XmHelperId xm_post_call_resolve_helper(const XmFunc *func, const XmIns *ins) {
    if (!func || !ins || !xm_ref_is_const(ins->args[0]))
        return XM_HELPER__COUNT;
    uint32_t ci = XM_REF_INDEX(ins->args[0]);
    if (ci >= func->nconst)
        return XM_HELPER__COUNT;
    return xm_helper_lookup((void *) (uintptr_t) func->consts[ci].val.raw);
}

/*
 * Record a call site's post-call state after the driver has finished
 * emitting the call and its post-call sequence.
 *
 * emitted_protocol is the XM_HPC_* bitmask the driver actually emitted
 * checks for at this site.
 */
static inline void xm_post_call_record(XmPostCallTracker *t, uint32_t code_offset,
                                       const XmFunc *func, const XmIns *ins,
                                       uint8_t emitted_protocol) {
    if (t->count >= XM_MAX_POST_CALL_RECORDS)
        return;
    XmHelperId hid = xm_post_call_resolve_helper(func, ins);
    uint8_t declared =
        (hid < XM_HELPER__COUNT) ? xm_helper_post_call(hid) : (XM_HPC_DEOPT | XM_HPC_SUSPEND);
    t->records[t->count++] = (XmPostCallRecord) {
        .code_offset = code_offset,
        .helper_id = (uint16_t) hid,
        .declared_protocol = declared,
        .emitted_protocol = emitted_protocol,
    };
}

typedef enum {
    XM_POST_CALL_VERIFY_OK = 0,
    XM_POST_CALL_VERIFY_MISSING_DEOPT,
    XM_POST_CALL_VERIFY_MISSING_SUSPEND,
    XM_POST_CALL_VERIFY_REDUNDANT_DEOPT,
    XM_POST_CALL_VERIFY_MISSING_THROW,
    XM_POST_CALL_VERIFY_OVERFLOW,
} XmPostCallVerifyError;

static inline const char *xm_post_call_verify_strerror(XmPostCallVerifyError e) {
    switch (e) {
        case XM_POST_CALL_VERIFY_OK:
            return "ok";
        case XM_POST_CALL_VERIFY_MISSING_DEOPT:
            return "helper requires deopt/suspend check but none emitted";
        case XM_POST_CALL_VERIFY_MISSING_SUSPEND:
            return "helper requires suspend check but none emitted";
        case XM_POST_CALL_VERIFY_REDUNDANT_DEOPT:
            return "driver emitted deopt/suspend check for helper that doesn't require one";
        case XM_POST_CALL_VERIFY_MISSING_THROW:
            return "helper has THROW in POST_CALL but FLAGS lacks THROW";
        case XM_POST_CALL_VERIFY_OVERFLOW:
            return "too many call sites to track";
    }
    return "unknown post-call verify error";
}

/*
 * Verify all recorded post-call sites. Returns true if every record is
 * consistent; logs and returns false on mismatch.
 *
 * DEOPT and SUSPEND are grouped because both share the same deopt-id
 * check mechanism: xm_helper_call_c_needs_deopt_check() returns true
 * when either flag is set.
 *
 * THROW verification: if a helper declares THROW in POST_CALL, the
 * helper's FLAGS must also contain THROW (checked at the flag-consistency
 * level via helpers.def). Runtime THROW propagation is handled by the
 * exception unwinder, not by explicit codegen checks, so we verify the
 * flag contract rather than emitted code.
 */
static inline bool xm_verify_post_call_records(const XmPostCallTracker *t, const char *arch_name) {
    bool ok = true;
    for (uint32_t i = 0; i < t->count; i++) {
        const XmPostCallRecord *r = &t->records[i];
        bool needs_deopt = (r->declared_protocol & (XM_HPC_DEOPT | XM_HPC_SUSPEND)) != 0;
        bool emitted_deopt = (r->emitted_protocol & (XM_HPC_DEOPT | XM_HPC_SUSPEND)) != 0;

        if (needs_deopt && !emitted_deopt) {
            xr_log_warning(arch_name, "post-call verify: helper %u at code+%u %s", r->helper_id,
                           r->code_offset,
                           xm_post_call_verify_strerror(XM_POST_CALL_VERIFY_MISSING_DEOPT));
            ok = false;
        }
        if (!needs_deopt && emitted_deopt) {
            xr_log_warning(arch_name, "post-call verify: helper %u at code+%u %s", r->helper_id,
                           r->code_offset,
                           xm_post_call_verify_strerror(XM_POST_CALL_VERIFY_REDUNDANT_DEOPT));
            ok = false;
        }

        /* THROW flag consistency: if POST_CALL declares THROW, the helper
         * FLAGS must contain THROW (XM_HF_THROW). This is the flag-level
         * part of THROW verification; runtime propagation is the exception
         * unwinder's responsibility. */
        if (r->helper_id < XM_HELPER__COUNT) {
            bool post_throw = (r->declared_protocol & XM_HPC_THROW) != 0;
            bool flag_throw = xm_helper_may_throw(r->helper_id);
            if (post_throw && !flag_throw) {
                xr_log_warning(arch_name, "post-call verify: helper %u at code+%u %s", r->helper_id,
                               r->code_offset,
                               xm_post_call_verify_strerror(XM_POST_CALL_VERIFY_MISSING_THROW));
                ok = false;
            }
        }
    }
    if (t->count >= XM_MAX_POST_CALL_RECORDS) {
        xr_log_warning(arch_name, "post-call verify: %s",
                       xm_post_call_verify_strerror(XM_POST_CALL_VERIFY_OVERFLOW));
    }
    return ok;
}

#endif /* XM_VERIFY_POST_CALL_H */
