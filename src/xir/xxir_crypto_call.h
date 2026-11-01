/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_crypto_call.h - An active scoped ref loan supplies the private entropy destination
 *
 * KEY CONCEPT:
 *   A synchronous borrowed call view authenticates a real ref loan. The Value
 *   operation prepares its transaction before observing the final call status.
 */
#ifndef XXIR_CRYPTO_CALL_H
#define XXIR_CRYPTO_CALL_H
#include "xxir_call.h"
XR_FUNC XrXirCallStatus xir_call_crypto_fill(const XrXirCallView *,const XrXirValue *,XrXirFaultDetail *);
#endif // XXIR_CRYPTO_CALL_H
