/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_vm_result_report.h - Report a live owned scalar VM result
 *
 * KEY CONCEPT:
 *   Diagnostic formatting runs after the execution boundary has returned.
 *   Raw writes preserve the canonical LF even on Windows.
 */
#ifndef LIR_VM_RESULT_REPORT_H
#define LIR_VM_RESULT_REPORT_H
#include "vm/register/xrbc.h"
#include "shared/xr_value_format_core.h"

static int lir_vm_panic_write(void *context, const void *bytes, size_t length) {
    (void)context;
    return !xrt_io_write(2, bytes, length).failed;
}

static int lir_vm_result_report(const XrRbcOutcome *outcome, XrRbcStatus status) {
    if (status == XR_RBC_OK) return 0;
    XrtArithmeticPanic facts;
    const XrtLocationOwner *locations;
    if (xr_rbc_outcome_arithmetic_panic(outcome, &facts, &locations)) {
        XrValueFormatSink sink = {NULL, lir_vm_panic_write};
        if (!xr_value_format_panic(sink, facts.code, 0, 0, 0, 0, NULL, 0, 0)) {
            static const char fallback[] = "XR_RUN_6005: cannot render uncaught typed panic\n";
            (void)xrt_io_write(2, fallback, sizeof(fallback) - 1);
        }
    } else {
        XrtHostClockFailure clock;
        if (xr_rbc_outcome_host_clock_failure(outcome, &clock, &locations)) {
            const XrtSourceLocation *source = xrt_locations_get(locations, clock.source_id);
            fprintf(stderr, "lir_vm_run: host clock %u failed at %s:%u:%u\n", clock.kind,
                source ? source->path : "unknown source", source ? source->line : 0,
                source ? source->column : 0);
        } else fprintf(stderr, "lir_vm_run: execution failed (%u)\n", (unsigned)status);
    }
    return 1;
}
#endif // LIR_VM_RESULT_REPORT_H
