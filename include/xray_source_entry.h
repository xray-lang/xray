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
#include "xray_source_value.h"
#ifdef __cplusplus
extern "C" {
#endif
#define XRAY_SOURCE_INVOCATION_VERSION 1u
typedef struct XrSourceInvocation {
    uint32_t api_version, struct_size;
    const char *name;
    size_t name_length;
    const XrSourceValue *const *arguments;
    size_t argument_count;
} XrSourceInvocation;
XRAY_API XrSourceStatus xray_source_invocation_init(XrSourceInvocation *, size_t);
/* Select an ordinary exported, nongeneric root function by its exact UTF-8
 * name span, without a trailing NUL. Arguments borrow through begin/execute;
 * begin retains their real data before returning. Each call owns a fresh
 * Instance and runs ordinary initialization. Names do not confer access to
 * private/library functions, methods or templates. Exact parameter types and
 * Domain/call permissions are checked by that Instance. Lookup, packet
 * validation and temporary argument storage spend the Program's original
 * cumulative compiler ledger; all runtime limits and wait/cancel rules remain.
 * The Program may die after begin succeeds. Failure preserves the output. */
XRAY_API XrSourceStatus xray_source_invoke_begin(XrSourceProgram *, const XrSourceInvocation *,
    const XrSourceCallOptions *, XrSourceCall **);
XRAY_API XrSourceStatus xray_source_invoke(XrSourceProgram *, const XrSourceInvocation *,
    const XrSourceCallOptions *, XrSourceResult **, XrSourceEvent *failure);
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
