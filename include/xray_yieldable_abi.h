/*
 * xray_yieldable_abi.h - canonical VM/native suspension ABI
 *
 * This header deliberately contains no VM or AOT implementation headers.  It
 * is the shared binary contract used by VM yieldable functions, generated
 * hosted-fragment adapters, and scheduler continuations.
 */

#ifndef XRAY_YIELDABLE_ABI_H
#define XRAY_YIELDABLE_ABI_H

#include "xray_value_abi.h"

struct XrVMRuntime;

#ifndef XR_CFUNC_RESULT_DEFINED
typedef enum XrCFuncResult {
    XR_CFUNC_DONE = 0,
    XR_CFUNC_YIELD,
    XR_CFUNC_BLOCKED,
    XR_CFUNC_ERROR,
    XR_CFUNC_CALL_CLOSURE,
    XR_CFUNC_WOULD_BLOCK
} XrCFuncResult;
#define XR_CFUNC_RESULT_DEFINED
#endif

typedef XrCFuncResult (*XrContinuation)(struct XrVMRuntime *runtime, int status,
                                        XrValue resume_value, void *context,
                                        XrValue *result);

#endif /* XRAY_YIELDABLE_ABI_H */
