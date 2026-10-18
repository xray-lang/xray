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
typedef enum XrSourceReplBindingKind {
    XR_SOURCE_REPL_VALUE, XR_SOURCE_REPL_FUNCTION, XR_SOURCE_REPL_TYPE,
    XR_SOURCE_REPL_IMPORT, XR_SOURCE_REPL_MODULE
} XrSourceReplBindingKind;
typedef struct XrSourceReplBindingInfo {
    const char *name, *type_name;
    uint64_t generation;
    XrSourceReplBindingKind kind;
    int is_const, implicit_result;
} XrSourceReplBindingInfo;
/* Borrows immutable published display facts until the next successful
 * publication or drop. Internal generated names are absent; the one it row
 * is marked separately from user declarations. A NULL type_name means no
 * admitted display spelling. Observations grant no type or runtime authority.
 * Pending input and callback reentry return BUSY, preserving empty outputs. */
XRAY_API XrSourceStatus xray_source_repl_bindings(const XrSourceRepl *,
    const XrSourceReplBindingInfo **, size_t *);
/* Owns one compiler, its real Catalog and one cumulative runtime Instance.
 * Options and namespace are copied during construction. Output callbacks and
 * their context remain caller-owned until drop. Every operation requires
 * caller exclusion; mutation during an output callback returns BUSY.
 * Each call selects this explicit owner. Concurrently live owners keep their
 * own domains and executors; there is no ambient current-VM selection.
 * OOM/BUDGET from preparation or reading latches the same Compiler state;
 * retry cannot allocate or obtain a fresh allowance after that first failure.
 * Inputs use the common Source checker. A non-Unit tail owns an immutable
 * result declaration; it selects the last successfully displayed and published
 * result in later inputs. Unit and failed inputs preserve it. Top-level it
 * declarations are reserved; local bindings may shadow it. */
XRAY_API XrSourceStatus xray_source_repl_new(const XrSourceCompilerOptions *, const char *namespace_id,
    const XrSourceCallOptions *, XrSourceRepl **);
/* Parses once and owns the exact AST through the paired transaction. MEMORY
 * namespace must match construction. Rooted text requires an absolute_path
 * and NULL namespace; it derives the real nearest project/script authority,
 * lockfile and relative imports without creating an entry file. Invalid nearer
 * projects never fall back to SCRIPT. Repeated paths retain exact birth state.
 * Input bytes and path may die after return.
 * A pending input or an untaken terminal result excludes another begin.
 * Compile failure leaves published declarations and values intact. */
XRAY_API XrSourceStatus xray_source_repl_begin(XrSourceRepl *, const XrSourceText *, XrSourceDiagnostic **);
/* Reads an exact regular-file snapshot with the original Compiler ledger and
 * genuine FILE authority, then parses once into the same paired transaction.
 * Relative paths resolve from the real working directory on that same ledger.
 * Relative imports use its real project/script root and lockfile. The path and
 * file contents may change after return; execution retains the admitted AST.
 * Missing/nonregular files and invalid nearer configuration never fall back
 * to MEMORY or an unsaved input. Lifecycle and sticky limits match begin. */
XRAY_API XrSourceStatus xray_source_repl_begin_file(XrSourceRepl *, const char *path, XrSourceDiagnostic **);
typedef enum XrSourceInputState { XR_SOURCE_INPUT_COMPLETE, XR_SOURCE_INPUT_INCOMPLETE } XrSourceInputState;
/* Scans an exact input span with the real lexer and this Compiler's cumulative
 * ledger. Empty input is complete; open delimiters or unterminated literals
 * need more input. Other syntax errors are complete and remain for the parser.
 * This observation grants no checking or execution authority. Pending input,
 * callback reentry and failed resource state preserve the output. */
XRAY_API XrSourceStatus xray_source_repl_check_input(XrSourceRepl *, const char *bytes, size_t length,
    XrSourceInputState *);
/* Inspects exactly one expression. The same checker evaluates its real
 * typeName once, including ordinary effects, and returns/displays that String.
 * The temporary candidate is aborted; published declarations and it stay fixed.
 * Uses the same poll/wait/resume/cancel/take lifecycle and cumulative limits. */
XRAY_API XrSourceStatus xray_source_repl_begin_type(XrSourceRepl *, const XrSourceText *, XrSourceDiagnostic **);
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
/* Borrows a typed value for the selected mutable declaration's real checked
 * birth writer. Instance admission requires its actual type and arena proof;
 * no conversion, retagging or implicit declaration occurs. Pending input and
 * callback reentry return BUSY. Uses the same cumulative limits, publishes no
 * input and preserves it. Failed cleanup stops the owner. */
XRAY_API XrSourceStatus xray_source_repl_write(XrSourceRepl *, const char *name, const XrSourceValue *);
/* Drains the real transaction before freeing its Compiler/Catalog. Consumes
 * and clears the owner except on BUSY; NULL drop is a successful no-op. */
XRAY_API XrSourceStatus xray_source_repl_drop(XrSourceRepl **, XrSourceEvent *cleanup);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_REPL_H
