/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_source_repl.h - Owned incremental Source execution
 */
#ifndef XRAY_SOURCE_REPL_H
#define XRAY_SOURCE_REPL_H
#include "xray_source_value.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrSourceRepl XrSourceRepl;
/* Owns one compiler, its real Catalog and one cumulative runtime Instance.
 * Options and namespace are copied during construction. Output callbacks and
 * their context remain caller-owned until drop. Every operation requires
 * caller exclusion; mutation during an output callback returns BUSY.
 * OOM/BUDGET from preparation or reading latches the same Compiler state;
 * retry cannot allocate or obtain a fresh allowance after that first failure.
 * Inputs use the common Source checker. A non-Unit tail owns an immutable
 * result declaration; it selects the last successfully displayed and published
 * result in later inputs. Unit and failed inputs preserve it. Top-level it
 * declarations are reserved; local bindings may shadow it. Files are not admitted. */
XRAY_API XrSourceStatus xray_source_repl_new(const XrSourceCompilerOptions *, const char *namespace_id,
    const XrSourceCallOptions *, XrSourceRepl **);
/* Parses once and owns the exact AST through the paired transaction. MEMORY
 * namespace must match construction; source bytes may die after return.
 * A pending input or an untaken terminal result excludes another begin.
 * Compile failure leaves published declarations and values intact. */
XRAY_API XrSourceStatus xray_source_repl_begin(XrSourceRepl *, const XrSourceText *, XrSourceDiagnostic **);
/* Bounded polls publish only an actual normally returned candidate. Other
 * outcomes drain and abort it, preserving ordinary prior-state side effects.
 * A meaningful tail is formatted from the actual owned outcome, within
 * the same cumulative output allowance, before resource-free publication.
 * None suppresses display while publishing its actual optional value.
 * Display failure aborts the candidate, returns the host status and stops the
 * owner. Terminal data stays owned until take/drop. Cleanup failure stops the
 * owner and reports CLEANUP_FAILED. */
XRAY_API XrSourceStatus xray_source_repl_poll(XrSourceRepl *, uint64_t quantum, XrSourceEvent *);
XRAY_API XrSourceStatus xray_source_repl_wait_request(const XrSourceRepl *, uint64_t epoch, uint64_t wake,
    XrSourceWait *, size_t);
XRAY_API XrSourceStatus xray_source_repl_resume(XrSourceRepl *, uint64_t epoch, uint64_t wake);
XRAY_API XrSourceStatus xray_source_repl_cancel(XrSourceRepl *);
/* Transfers one terminal result without allocating. The result and read values
 * survive destruction of the entire REPL, Compiler and original input. */
XRAY_API XrSourceStatus xray_source_repl_take(XrSourceRepl *, XrSourceResult **);
/* Executes the selected declaration's real checked birth reader. Names select
 * published bindings, never numeric slots or runtime permissions. A pending
 * input refuses reads; empty output is preserved on every failure. */
XRAY_API XrSourceStatus xray_source_repl_read(XrSourceRepl *, const char *name, XrSourceValue **);
/* Drains the real transaction before freeing its Compiler/Catalog. Consumes
 * and clears the owner except on BUSY; NULL drop is a successful no-op. */
XRAY_API XrSourceStatus xray_source_repl_drop(XrSourceRepl **, XrSourceEvent *cleanup);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_REPL_H
