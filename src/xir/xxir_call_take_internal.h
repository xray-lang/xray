/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_take_internal.h - Synchronous transfer from real compiler frames
 *
 * KEY CONCEPT:
 *   The trusted VM and generated-C adapters lend their exact compact frame.
 *   Mode facts remain in the actual declaration or callable signature.
 */
#ifndef XXIR_CALL_TAKE_INTERNAL_H
#define XXIR_CALL_TAKE_INTERNAL_H
#include "xxir.h"
#include "xxir_call_internal.h"
#include "xxir_types.h"
#include "xxir_value_take_internal.h"
typedef struct XirCallTakeArguments {
    const XrXirFunction *caller,*callee;
    const XrXirTypeNode *signature;
    const XrXirFunctionLayout *layout;
    const XrXirValue *arguments;
    void *frame;
    uint32_t instruction,target,count;
} XirCallTakeArguments;
/* All borrowed coordinates expire with this current callback's child prepare. */
XR_FUNC XrXirCallStatus xir_call_arguments_stage(XrXirCallView *view,const XirCallTakeArguments *take);
/* Task metadata is prepared before this synchronous root-frame transfer.
 * Success commits the source words; no fallible publication may follow it. */
XR_FUNC XrXirCallStatus xir_call_task_new_budgeted(const XrXirCallConfig *config,
    const XrXirCallRequest *request,XrXirCallBudget *budget,
    XrXirCallView *source,const XrXirExecutorBinding *binding,XrXirCall **output);
/* The driver already owns this logical argument independently of its caller. */
XR_FUNC XrXirValueStatus xir_call_owned_argument_take(XrXirCallView *view,uint32_t parameter,XrXirValue *output);
XR_FUNC XrXirValueStatus xir_call_cell_write_take_prepare(const XrXirCallView *view,
    const XrXirValue *cell,const XrXirValue *value,XirCellWriteTake *output);
XR_FUNC XrXirCallStatus xir_call_compact_return_move(XrXirCallView *view,
    void *word,XrXirAction *output);
/* The returned logical value is already owned by the actual driver inbox. */
XR_FUNC XrXirValueStatus xir_call_compact_inbox_take(XrXirCallView *view,
    void *word,XrXirType type);
#endif /* XXIR_CALL_TAKE_INTERNAL_H */
