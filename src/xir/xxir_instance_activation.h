/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_activation.h - Reclaim one task group within its persistent owner
 */
#ifndef XXIR_INSTANCE_ACTIVATION_H
#define XXIR_INSTANCE_ACTIVATION_H
#include "xxir_program.h"
typedef struct XrXirInstance XrXirInstance;
/* Control reclamation allocates no memory; language cleanup keeps its ordinary
 * budget. Ready slots, callable authority and lifetime limits remain owned by
 * the Instance. Initialization failure stays sticky. A prepared generation is
 * rejected; failed cleanup permanently stops the owner. */
XR_FUNC XrXirCallStatus xr_xir_instance_close_activation(XrXirInstance *instance);
/* A hard host service failure closes only this authenticated execution group.
 * Success borrows its complete original terminal through the existing result. */
XR_FUNC XrXirCallStatus xir_instance_host_failure(XrXirInstance *instance, XrXirInstanceResult *observed);
#endif // XXIR_INSTANCE_ACTIVATION_H
