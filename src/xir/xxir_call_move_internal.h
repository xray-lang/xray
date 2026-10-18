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
#endif // XXIR_CALL_MOVE_INTERNAL_H
