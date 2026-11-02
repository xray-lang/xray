/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_stage_internal.h - Constructor-owned field result contracts
 *
 * KEY CONCEPT:
 *   A field tuple must match its real constructor and complete owning class.
 */
#ifndef XXIR_CLASS_STAGE_INTERNAL_H
#define XXIR_CLASS_STAGE_INTERNAL_H
#include "xxir.h"
XR_FUNC XrXirStatus xir_compile_class_stage_signature(const XrXirModule *module,
    uint32_t helper,XrCompileRequestControl *control);
/* These checks borrow one complete Module. They grant no host entry or
 * standalone Program permission and do not replace ordinary body verification. */
XR_FUNC XrXirStatus xir_compile_class_stage_verify(const XrXirModule *module,
    XrCompileRequestControl *control);
XR_FUNC XrXirStatus xir_compile_class_stage_call(const XrXirModule *module,
    uint32_t caller,uint32_t instruction,XrCompileRequestControl *control);
#endif // XXIR_CLASS_STAGE_INTERNAL_H
