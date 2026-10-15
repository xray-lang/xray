/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_source_entry.h - Named exported entries and independently owned outcomes
 */
#ifndef XRAY_SOURCE_ENTRY_H
#define XRAY_SOURCE_ENTRY_H
#include "xray_source.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Select an ordinary exported, nongeneric, zero-parameter root function by its
 * exact UTF-8 name bytes, without a trailing NUL. Each call owns a fresh
 * Instance and runs ordinary initialization. Names do not confer access to
 * private/library functions, methods or templates. Parameterized entries
 * return UNSUPPORTED. Lookup and packet validation spend the Program's original
 * cumulative compiler ledger; all runtime limits and wait/cancel rules remain.
 * The Program may die after begin succeeds. Failure preserves the output. */
XRAY_API XrSourceStatus xray_source_call_begin_named(XrSourceProgram *, const char *name,
    size_t length, const XrSourceCallOptions *, XrSourceCall **);
XRAY_API XrSourceStatus xray_source_execute_named(XrSourceProgram *, const char *name,
    size_t length, const XrSourceCallOptions *, XrSourceResult **, XrSourceEvent *failure);
/* Copies the real outcome, including Error/Panic and every owned value/arena.
 * Copying data grants no callable, Task or Instance execution permission.
 * The source and its producer may be freed after success. Output must be NULL;
 * refusal leaves it unchanged. Allocation uses the original compiler ledger. */
XRAY_API XrSourceStatus xray_source_result_copy(const XrSourceResult *, XrSourceResult **);
/* Require a returned outcome of the exact type; refusal preserves output.
 * Bool writes zero or one. F64 preserves all runtime binary64 bits. */
XRAY_API XrSourceStatus xray_source_result_bool(const XrSourceResult *, int *);
XRAY_API XrSourceStatus xray_source_result_f64(const XrSourceResult *, double *);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_ENTRY_H
