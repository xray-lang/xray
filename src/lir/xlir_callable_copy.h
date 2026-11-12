/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLIR_CALLABLE_COPY_H
#define XLIR_CALLABLE_COPY_H
#include "xlir_callable.h"

static inline XrLirStatus lir_copy_closure_target(const XrLirModule *input,
    XrLirFunction source, XrLirFunction target) {
    XrLirClosureTarget closure = {0};
    XrLirStatus status = xr_lir_closure_target_view(input, source, &closure);
    if (status == XR_LIR_OK && closure.callable)
        status = xr_lir_set_closure_target((XrLirModule *)target.module, target, closure);
    return status;
}

#endif // XLIR_CALLABLE_COPY_H
