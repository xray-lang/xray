/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_view_internal.h - Current Instance permission for module views
 *
 * KEY CONCEPT:
 *   Published module storage grants a shared view only to its actual module.
 */
#ifndef XXIR_INSTANCE_VIEW_INTERNAL_H
#define XXIR_INSTANCE_VIEW_INTERNAL_H
#include "xxir_call.h"

XR_FUNC XrXirCallStatus xir_instance_view_module_root(XrXirCallView *view,const XrXirValue *root);
#endif /* XXIR_INSTANCE_VIEW_INTERNAL_H */
