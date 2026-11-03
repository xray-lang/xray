/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_signal_internal.h - Sealed registration and deferred Root delivery
 */
#ifndef XXIR_INSTANCE_SIGNAL_INTERNAL_H
#define XXIR_INSTANCE_SIGNAL_INTERNAL_H
#include "xxir_program.h"

/* Only the actual native frame reached through one retained, direct checked
 * CALL can register. The complete native declaration, sys authority and actual
 * owned FunctionBinding producer are independently rechecked. Raw host entry,
 * worker/callback registration and unreceived generations grant no authority.
 * The registered function remains Unit/Never and may suspend. This operation
 * itself does not invoke it or manufacture a NoSuspend/Sendable promise. */
XR_FUNC XrXirCallStatus xir_instance_signal_register(XrXirCallView *view,bool *output);
#endif // XXIR_INSTANCE_SIGNAL_INTERNAL_H
