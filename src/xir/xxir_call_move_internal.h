/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_move_internal.h - Atomic return transfer from owned frame slots
 *
 * KEY CONCEPT:
 *   The driver owns a staged result before producer state can be released.
 */
#ifndef XXIR_CALL_MOVE_INTERNAL_H
#define XXIR_CALL_MOVE_INTERNAL_H
#include "xxir_call.h"

/* The producer owns an aligned Value slot inside its actual frame state.
 * Success empties that slot and stages one exact RETURN action; failure leaves
 * both the slot and action untouched. Borrowed callback views are not slots. */
XR_FUNC XrXirCallStatus xr_xir_call_return_move(XrXirCallView *view,
    XrXirValue *source, XrXirAction *action);
/* All fallible return admission and terminal work precede commit. This is a
 * synchronous C-only callback: no guest reentry, source mutation or retained
 * context. Failure leaves source/action owned and unstaged. Success permits
 * only the existing no-fail move into the actual frame's pending return. */
typedef XrXirCallStatus (*XirCallReturnCommit)(void *context);
XR_FUNC XrXirCallStatus xir_call_return_move_commit(XrXirCallView *view,
    XrXirValue *source,XrXirAction *action,XirCallReturnCommit commit,void *context);
#endif // XXIR_CALL_MOVE_INTERNAL_H
